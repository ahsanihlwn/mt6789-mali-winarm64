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

/*
 * _setjmpex / longjmp  -  the setjmp half the CRT-free recipe needs.
 *
 * WHY THIS EXISTS
 * ---------------
 * panfrost -> libvtn (SPIR-V -> NIR) uses setjmp/longjmp for parse-error
 * recovery (src/compiler/spirv/vtn_private.h).  On aarch64-pc-windows-msvc
 * clang lowers `setjmp()` to a call to `_setjmpex` and pairs it with
 * `longjmp`; both live in vcruntime140.dll, which is not reliably present on
 * Windows 11 ARM64 and is deliberately not a dependency of this project.
 *
 * WHY A PRIVATE LAYOUT IS CORRECT HERE
 * ------------------------------------
 * The compiler only ever passes the jmp_buf *address* around -- it never
 * inspects the contents.  Defining both halves of the pair in this file makes
 * the on-disk layout a private implementation detail; the only contract that
 * matters is the calling convention, which was measured by disassembling a
 * probe (2026-11-09):
 *
 *     int  _setjmpex(jmp_buf buf [x0], void *frame [x1]);
 *     void longjmp (jmp_buf buf [x0], int value [w1]);
 *
 * The buffer layout below is *also* the MSVC _JUMP_BUFFER layout
 * (VC\...\include\setjmp.h, _M_ARM64 branch: 24 x u64 = 192 bytes), so the
 * buffer is exactly the size <setjmp.h> declares:
 *
 *     0   Frame          (unused: this longjmp unwinds nothing)
 *     8   Reserved
 *     16  X19..X28       (10 callee-saved GPRs)
 *     96  Fp   (x29)
 *     104 Lr   (x30)
 *     112 Sp
 *     120 Fpcr (u32)
 *     124 Fpsr (u32)
 *     128 D8..D15        (8 doubles, eight-byte aligned)
 *
 * longjmp restores the callee-saved state and returns straight to the
 * instruction after the _setjmpex call, exactly as setjmp/longjmp require.
 * It performs no SEH unwinding: Mesa's use is plain C (state is owned by
 * ralloc on the heap), so there are no frames to unwind.
 *
 * SPDX-License-Identifier: MIT
 */

        .text
        .globl  _setjmpex
        .p2align 2
_setjmpex:
        str     x1, [x0, #0]            /* Frame   (kept for layout fidelity) */
        str     xzr, [x0, #8]           /* Reserved */
        stp     x19, x20, [x0, #16]
        stp     x21, x22, [x0, #32]
        stp     x23, x24, [x0, #48]
        stp     x25, x26, [x0, #64]
        stp     x27, x28, [x0, #80]
        stp     x29, x30, [x0, #96]     /* Fp, Lr */
        mov     x2, sp
        str     x2, [x0, #112]          /* Sp */
        mrs     x2, fpcr
        str     w2, [x0, #120]          /* Fpcr */
        mrs     x2, fpsr
        str     w2, [x0, #124]          /* Fpsr */
        stp     d8,  d9,  [x0, #128]
        stp     d10, d11, [x0, #144]
        stp     d12, d13, [x0, #160]
        stp     d14, d15, [x0, #176]
        mov     w0, #0
        ret

        .globl  longjmp
        .p2align 2
longjmp:
        ldp     x19, x20, [x0, #16]
        ldp     x21, x22, [x0, #32]
        ldp     x23, x24, [x0, #48]
        ldp     x25, x26, [x0, #64]
        ldp     x27, x28, [x0, #80]
        ldr     x2, [x0, #112]
        mov     sp, x2
        ldp     x29, x30, [x0, #96]     /* Fp, Lr */
        ldr     w3, [x0, #120]
        msr     fpcr, x3
        ldr     w3, [x0, #124]
        msr     fpsr, x3
        ldp     d8,  d9,  [x0, #128]
        ldp     d10, d11, [x0, #144]
        ldp     d12, d13, [x0, #160]
        ldp     d14, d15, [x0, #176]
        cmp     w1, #0
        csinc   w0, w1, wzr, ne         /* value == 0 ? 1 : value */
        ret
