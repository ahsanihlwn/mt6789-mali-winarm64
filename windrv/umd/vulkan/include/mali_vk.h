/*
 * mali_vk.h  -  internal types shared by the Mali-G57 Vulkan ICD (windrv/umd).
 *
 * This is Jalur A from windrv/GRAPHICS-STACK.md: a small ICD we own that drives
 * the GPU through the UMD winsys (mali_umd.h -> IOCTL ABI v2). It exists to
 * prove the Vulkan -> BO -> job -> fence path end to end. It is NOT a
 * shader-capable driver: pipelines need Mesa's NIR/Valhall compiler (Jalur B).
 *
 * Command buffers record host-side; vkQueueSubmit turns what it can into real
 * Mali job chains (WRITE_VALUE) and executes them synchronously. Fences are
 * therefore signalled on return from vkQueueSubmit.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES 1
#endif

#include <vulkan/vulkan.h>
#include <stdint.h>
#include "mali_umd.h"

/* ------------------------------------------------------------------ */
/* Opaque objects (Vulkan handle types are cast to/from these)         */
/* ------------------------------------------------------------------ */

typedef struct MaliVkInstance_T       { uint32_t Magic; } MaliVkInstance;
typedef struct MaliVkPhysicalDevice_T { uint32_t Index; } MaliVkPhysicalDevice;
typedef struct MaliVkDevice_T         MaliVkDevice;
typedef struct MaliVkQueue_T          MaliVkQueue;

struct MaliVkQueue_T { MaliVkDevice *Dev; uint32_t Family; };

struct MaliVkDevice_T {
    PMALI_UMD_DEVICE Umd;
    MALI_UMD_BO      ChainBo;     /* GPU-visible, host-mapped BO for job chains */
    uint32_t         ChainUsed;   /* bytes consumed in ChainBo                    */
    MaliVkQueue      Queue;       /* the single queue family/queue we expose      */
};

typedef struct MaliVkMemory_T {
    MaliVkDevice *Dev;
    MALI_UMD_BO   Bo;
} MaliVkMemory;

typedef struct MaliVkBuffer_T {
    MaliVkDevice *Dev;
    uint64_t      Size;
    MaliVkMemory *Mem;        /* bound memory, NULL until vkBindBufferMemory */
    uint64_t      MemOffset;
} MaliVkBuffer;

typedef struct MaliVkCommandPool_T {
    MaliVkDevice *Dev;
    uint32_t      Count;
    void         *Buffers[64];   /* MaliVkCmdBuffer*, tracked for pool reset/free */
} MaliVkCommandPool;

/* A recorded GPU action. */
#define MALI_VK_OP_FILL 1u
#define MALI_VK_OP_COPY 2u

typedef struct MaliVkOp {
    uint32_t    Kind;      /* MALI_VK_OP_*                                 */
    uint32_t    Value;     /* fill pattern                                 */
    uint64_t    Size;
    uint64_t    DstGpuVa;  /* destination GPU VA                           */
    void       *DstCpu;    /* destination CPU pointer (copy)               */
    const void *SrcCpu;    /* source CPU pointer (copy)                    */
} MaliVkOp;

#define MALI_VK_MAX_OPS 256

typedef struct MaliVkCmdBuffer_T {
    MaliVkDevice             *Dev;
    VkCommandBufferLevel      Level;
    VkCommandBufferUsageFlags Usage;
    VkResult                  State;   /* VK_SUCCESS while recording */
    uint32_t                  OpCount;
    MaliVkOp                  Ops[MALI_VK_MAX_OPS];
} MaliVkCmdBuffer;

typedef struct MaliVkFence_T {
    MaliVkDevice *Dev;
    uint32_t      Signaled;   /* VkBool32 */
} MaliVkFence;

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

#define MALI_VK_CHAIN_BO_SIZE   (64u * 1024u)
#define MALI_VK_MAX_ZERO_JOBS   256u          /* cap GPU zero-fill job count  */

/* Per-device execution (icd_queue.c). */
void MaliVkExecuteSubmit(MaliVkDevice *Dev, MaliVkCmdBuffer *Cmd);

/* Small helpers (icd_util.c), CRT-free. */
int  MaliVkStrEq(const char *a, const char *b);
void MaliVkZero(void *p, uint64_t n);
void MaliVkLog(const char *s);
