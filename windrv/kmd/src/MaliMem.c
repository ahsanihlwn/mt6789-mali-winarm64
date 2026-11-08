/*
 * MaliMem.c  -  GPU buffer object allocator for the Windows KMD.
 *
 * BOs are physically-contiguous, non-cached allocations. Their GPU address is
 * their physical address (identity MMU), and they are mapped to the calling
 * process on demand through an MDL.
 *
 * SPDX-License-Identifier: MIT
 */
#include <ntddk.h>
#include <wdf.h>
#include "MaliKmd.h"

#define MALI_BO_TAG          'loMB'
#define MALI_BO_MAX_SIZE     (256ull * 1024ull * 1024ull)

static VOID MaliFreeBo(PMALI_KMD_DEVICE Ctx, PMALI_BO Bo)
{
    MaliMmuUnmap(Ctx, Bo->PhysBase, Bo->Size);
    MaliMemUnmapUser(Ctx, Bo);

    if (Bo->Mdl != NULL) {
        IoFreeMdl(Bo->Mdl);
        Bo->Mdl = NULL;
    }
    if (Bo->SystemVa != NULL) {
        MmFreeContiguousMemory(Bo->SystemVa);
        Bo->SystemVa = NULL;
    }
    ExFreePoolWithTag(Bo, MALI_BO_TAG);
}

NTSTATUS MaliMemInitialize(PMALI_KMD_DEVICE Ctx)
{
    InitializeListHead(&Ctx->BoList);
    KeInitializeGuardedMutex(&Ctx->BoLock);
    Ctx->NextBoHandle = 1;
    return STATUS_SUCCESS;
}

NTSTATUS MaliMemAlloc(PMALI_KMD_DEVICE Ctx, ULONG Size, ULONG Flags, PMALI_BO *Out)
{
    PHYSICAL_ADDRESS Low, High, Boundary;
    PMALI_BO Bo;
    UINT64 Rounded;
    NTSTATUS Status;

    if (Size == 0 || (UINT64)Size > MALI_BO_MAX_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }

    Rounded = ((UINT64)Size + MALI_PAGE_SIZE - 1) & ~(MALI_PAGE_SIZE - 1);

    Bo = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*Bo), MALI_BO_TAG);
    if (Bo == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Bo->Flags = Flags;
    Bo->Size  = Rounded;

    Low.QuadPart      = 0;
    High.QuadPart     = MALI_VA_LIMIT - 1;
    Boundary.QuadPart = 0;

    Bo->SystemVa = MmAllocateContiguousMemorySpecifyCache(
        (SIZE_T)Rounded, Low, High, Boundary, MmNonCached);
    if (Bo->SystemVa == NULL) {
        ExFreePoolWithTag(Bo, MALI_BO_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Bo->SystemVa, (SIZE_T)Rounded);
    Bo->PhysBase = MmGetPhysicalAddress(Bo->SystemVa).QuadPart;

    /* Identity-map into the GPU page tables immediately. */
    Status = MaliMmuMap(Ctx, Bo->PhysBase, Rounded);
    if (!NT_SUCCESS(Status)) {
        MmFreeContiguousMemory(Bo->SystemVa);
        ExFreePoolWithTag(Bo, MALI_BO_TAG);
        return Status;
    }

    Bo->Mdl = IoAllocateMdl(Bo->SystemVa, (ULONG)Rounded, FALSE, FALSE, NULL);
    if (Bo->Mdl != NULL) {
        MmBuildMdlForNonPagedPool(Bo->Mdl);
    }

    KeAcquireGuardedMutex(&Ctx->BoLock);
    Bo->Handle = Ctx->NextBoHandle++;
    if (Ctx->NextBoHandle == 0) {
        Ctx->NextBoHandle = 1;
    }
    InsertTailList(&Ctx->BoList, &Bo->Link);
    KeReleaseGuardedMutex(&Ctx->BoLock);

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "MaliKmd: BO #%u size 0x%llx PA 0x%llx\n",
               Bo->Handle, (unsigned long long)Bo->Size,
               (unsigned long long)Bo->PhysBase);

    *Out = Bo;
    return STATUS_SUCCESS;
}

NTSTATUS MaliMemFind(PMALI_KMD_DEVICE Ctx, ULONG Handle, PMALI_BO *Out)
{
    PLIST_ENTRY Entry;

    KeAcquireGuardedMutex(&Ctx->BoLock);
    for (Entry = Ctx->BoList.Flink; Entry != &Ctx->BoList; Entry = Entry->Flink) {
        PMALI_BO Bo = CONTAINING_RECORD(Entry, MALI_BO, Link);
        if (Bo->Handle == Handle) {
            KeReleaseGuardedMutex(&Ctx->BoLock);
            *Out = Bo;
            return STATUS_SUCCESS;
        }
    }
    KeReleaseGuardedMutex(&Ctx->BoLock);
    return STATUS_NOT_FOUND;
}

NTSTATUS MaliMemFree(PMALI_KMD_DEVICE Ctx, ULONG Handle)
{
    PLIST_ENTRY Entry;
    PMALI_BO Bo = NULL;

    KeAcquireGuardedMutex(&Ctx->BoLock);
    for (Entry = Ctx->BoList.Flink; Entry != &Ctx->BoList; Entry = Entry->Flink) {
        PMALI_BO Candidate = CONTAINING_RECORD(Entry, MALI_BO, Link);
        if (Candidate->Handle == Handle) {
            RemoveEntryList(&Candidate->Link);
            Bo = Candidate;
            break;
        }
    }
    KeReleaseGuardedMutex(&Ctx->BoLock);

    if (Bo == NULL) {
        return STATUS_NOT_FOUND;
    }
    MaliFreeBo(Ctx, Bo);
    return STATUS_SUCCESS;
}

NTSTATUS MaliMemMapUser(PMALI_KMD_DEVICE Ctx, PMALI_BO Bo, PVOID *UserVa)
{
    UNREFERENCED_PARAMETER(Ctx);

    if (Bo->UserMapped && Bo->UserVa != NULL) {
        *UserVa = Bo->UserVa;
        return STATUS_SUCCESS;
    }
    if (Bo->Mdl == NULL) {
        return STATUS_UNSUCCESSFUL;
    }

    Bo->UserVa = MmMapLockedPagesSpecifyCache(
        Bo->Mdl, UserMode, MmNonCached, NULL, FALSE,
        NormalPagePriority | MdlMappingNoExecute);
    if (Bo->UserVa == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Bo->UserMapped = TRUE;
    Bo->OwnerPid   = PsGetCurrentProcessId();
    *UserVa = Bo->UserVa;
    return STATUS_SUCCESS;
}

VOID MaliMemUnmapUser(PMALI_KMD_DEVICE Ctx, PMALI_BO Bo)
{
    UNREFERENCED_PARAMETER(Ctx);

    if (Bo->UserMapped && Bo->UserVa != NULL && Bo->Mdl != NULL) {
        /*
         * The mapping lives in the owning process. If the process is already
         * gone the mapping was torn down with it, so only unmap from the owner.
         */
        if (Bo->OwnerPid == NULL || Bo->OwnerPid == PsGetCurrentProcessId()) {
            MmUnmapLockedPages(Bo->UserVa, Bo->Mdl);
        }
        Bo->UserVa = NULL;
        Bo->UserMapped = FALSE;
        Bo->OwnerPid = NULL;
    }
}

VOID MaliMemCleanup(PMALI_KMD_DEVICE Ctx)
{
    for (;;) {
        PLIST_ENTRY Entry;
        PMALI_BO Bo;

        KeAcquireGuardedMutex(&Ctx->BoLock);
        if (IsListEmpty(&Ctx->BoList)) {
            KeReleaseGuardedMutex(&Ctx->BoLock);
            break;
        }
        Entry = RemoveHeadList(&Ctx->BoList);
        KeReleaseGuardedMutex(&Ctx->BoLock);

        Bo = CONTAINING_RECORD(Entry, MALI_BO, Link);
        MaliFreeBo(Ctx, Bo);
    }
}