/*
 * mali_aarch64.s  -  __chkstk for the CRT-free ARM64 Windows link recipe.
 *
 * WHY THIS EXISTS
 * ---------------
 * clang emits a call to __chkstk for any function whose stack frame (or
 * alloca) exceeds the probe size (4 KiB by default). MSVC provides it in
 * vcruntime.lib, which is not installed for ARM64 on this host, so a link
 * fails with
 *
 *     lld-link: error: undefined symbol: __chkstk
 *
 * Measured 2026-11-09 on Mesa's panvk_physical_device.c
 * (panvk_physical_device_init) and panvk_vX_cmd_draw.c
 * (panvk_v6_cmd_resolve_attachments).
 *
 * CONTRACT (Windows ARM64 ABI)
 * ----------------------------
 *   x15 = number of 16-byte units of stack about to be allocated
 *   bl  __chkstk
 *   sub sp, sp, x15, lsl #4
 *
 * __chkstk touches every page the caller is about to use so a stack-overflow
 * guard page faults cleanly. It must not move the stack pointer and must
 * preserve every register the caller still reads afterwards; only x16/x17 may
 * be clobbered.
 *
 * This is compiler-rt's lib/builtins/aarch64/chkstk.S, which is the canonical
 * implementation (Apache-2.0 WITH LLVM-exception):
 *
 *     lsl    x16, x15, #4
 *     mov    x17, sp
 *   1: sub    x17, x17, #4096
 *      subs   x16, x16, #4096
 *      ldr    xzr, [x17]
 *      b.gt   1b
 *      ret
 *
 * Built by tools/build-runtime.ps1 (see the .s branch there).
 *
 * SPDX-License-Identifier: MIT
 */

        .text
        .globl  __chkstk
        .p2align 2
__chkstk:
        lsl     x16, x15, #4
        mov     x17, sp
1:
        sub     x17, x17, #4096
        subs    x16, x16, #4096
        ldr     xzr, [x17]
        b.gt    1b

        ret
