/*
 * linux/dma-heap.h  -  build-time stand-in for the Android/Linux dma-buf heap
 * uAPI, for Windows MSVC/ARM64.
 *
 * panfrost/lib/kmod/kbase_kmod.c includes this unconditionally and uses it to
 * allocate shareable BOs through /dev/dma_heap/system. That device node does
 * not exist on Windows, so kbase_kmod_dev_create() leaves dev->dma_heap_fd at
 * -1, the "can share" predicate stays false, and no ioctl here is ever issued.
 * Only the types and the request number therefore have to exist. The values
 * follow the Linux uAPI so the header stays correct if this ever runs against
 * a real dma-heap.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_COMPAT_LINUX_DMA_HEAP_H
#define MALI_COMPAT_LINUX_DMA_HEAP_H

#include <linux/types.h>
#include <linux/ioctl.h>

struct dma_heap_allocation_data {
    __u64 len;
    __u32 fd;
    __u32 fd_flags;
    __u64 heap_flags;
};

#define DMA_HEAP_IOC_MAGIC      'H'
#define DMA_HEAP_IOCTL_ALLOC \
    _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)

#endif /* MALI_COMPAT_LINUX_DMA_HEAP_H */