/*
 * mali_mem.h  -  GPU buffer objects (BO) for the Windows KMD.
 *
 * Each BO is a physically-contiguous, non-cached allocation. Its GPU address
 * is its physical address (identity MMU) for the first bring-up. User mode
 * gets a mapped VA via the same MDL.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <ntddk.h>
#include <wdf.h>

#define MALI_BO_FLAG_CPU_READ   0x00000001u
#define MALI_BO_FLAG_CPU_WRITE  0x00000002u
#define MALI_BO_FLAG_GPU_READ   0x00000004u
#define MALI_BO_FLAG_GPU_WRITE  0x00000008u
#define MALI_BO_FLAG_CACHED     0x00000010u   /* reserved for later */

typedef struct _MALI_BO {
    LIST_ENTRY Link;
    ULONG      Handle;
    ULONG      Flags;
    UINT64     Size;          /* page-rounded */
    PVOID      SystemVa;      /* kernel VA (non-cached contiguous) */
    UINT64     PhysBase;
    PMDL       Mdl;
    PVOID      UserVa;
    BOOLEAN    UserMapped;
    HANDLE     OwnerPid;      /* process the user mapping belongs to */
} MALI_BO, *PMALI_BO;

struct _MALI_KMD_DEVICE;

NTSTATUS MaliMemInitialize(struct _MALI_KMD_DEVICE *Ctx);
VOID     MaliMemCleanup(struct _MALI_KMD_DEVICE *Ctx);

NTSTATUS MaliMemAlloc(struct _MALI_KMD_DEVICE *Ctx, ULONG Size, ULONG Flags, PMALI_BO *Out);
NTSTATUS MaliMemFree(struct _MALI_KMD_DEVICE *Ctx, ULONG Handle);
NTSTATUS MaliMemFind(struct _MALI_KMD_DEVICE *Ctx, ULONG Handle, PMALI_BO *Out);

/* Must be called in the context of the target process (passive level). */
NTSTATUS MaliMemMapUser(struct _MALI_KMD_DEVICE *Ctx, PMALI_BO Bo, PVOID *UserVa);
VOID     MaliMemUnmapUser(struct _MALI_KMD_DEVICE *Ctx, PMALI_BO Bo);