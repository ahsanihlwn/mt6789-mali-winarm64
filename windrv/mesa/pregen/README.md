# Pregenerated PanVK shader artifacts

These files are the build-time output of Mesa's shader generators for
`src/panfrost/libpan` and `src/poly/cl`. They are checked in because the
Windows/ARM64 build host cannot run the generators (see below), and because
they are deterministic.

Consumed by `-Dpanfrost-pregen=true`
([`patches/0008-panfrost-pregen.patch`](../patches/0008-panfrost-pregen.patch)),
staged into the Mesa source tree by
[`tools/patch-mesa.ps1`](../tools/patch-mesa.ps1).

## What is here

| File(s) | Produced by | Consumed as |
| --- | --- | --- |
| `libpan_v{4..14}.spv` (not kept) | `mesa_clc` on `src/panfrost/libpan/*.cl` | input to the two below |
| `libpan_v{4..14}.cpp`, `.h` | `vtn_bindgen2` | libpan's NIR blobs |
| `libpan_shaders_v{4..14}.c`, `.h` | `panfrost_compile libpan_shaders <ver>` | libpan's precompiled ISA |
| `libpoly.cpp`, `.h` | `mesa_clc` + `vtn_bindgen2` on `src/poly/cl/*.cl` | `src/poly/nir` lowering helpers |

`SHA256SUMS` is written by the generator; `tools/patch-mesa.ps1` does not
verify it, but it makes hand-editing obvious in review.

## Why they are committed

Mesa 25.1+ compiles those OpenCL kernels at build time with `mesa_clc`, which
links LLVM, Clang, SPIRV-Tools and SPIRV-LLVM-Translator. PanVK links `libpan`
unconditionally and enabling `vulkan-drivers=panfrost` also enables `src/poly`,
so there is no configuration of a v9-capable PanVK without that toolchain.

The build host here has the MSVC/LLVM toolchain but no LLVM headers, and its
`clang` has no SPIR-V target, so `mesa_clc` cannot be built. The two generators
are *host* tools: their output is plain C and does not depend on the machine
that ran them, which is exactly what a normal cross build does - only the
"host" half runs on Linux instead of Windows.

Valhall v9 (`libpan_shaders_v9`) is Mali-G57 (MT6789), the target SoC.

## Regenerating

On any Linux with clang >= 17 (SPIR-V capable), `llvm-dev`, `libclang-dev`,
`meson`, `ninja`, `cmake` and `pkg-config`:

```sh
windrv/mesa/tools/gen-libpan-wsl.sh
```

The script downloads the extra packages with `apt-get download` and unpacks
them with `dpkg -x` (nothing is installed), generates every architecture, and
rewrites this directory plus `SHA256SUMS`. It must be re-run when the
`src/panfrost/libpan/*.cl` or `src/poly/cl/*.cl` sources change, which the
FourFectVK v9 stack does - so **after updating the stack, regenerate.**

WSL2 note: the build scratch lives in `$MT6789_WSL_SCRATCH`
(default `~/.cache/mt6789-mesa`) rather than in this repository, because Meson
does not quote paths containing spaces and the repository path contains one
(`iOS 27`), and because `/tmp` is a small tmpfs that does not survive between
`wsl` invocations.