/* Mesa cross-toolchain smoke target (Jalur B).
 *
 * Nothing GPU-specific: this exists only to make Meson exercise the aarch64
 * cross toolchain (compile an ARM64 COFF object, run the compiler sanity link).
 * It is a static library so it needs no runtime of its own.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>

uint32_t mali_mesa_smoke(uint32_t x)
{
    return (x * 2u) + 1u;
}
