# Experimental patches (not applied by default)

`tools\patch-mesa.ps1` only globs `patches\*.patch` (non-recursive), so anything
in this folder is deliberately **not** applied to the Mesa checkout.

## Why these two are parked here

They exist to force PanVK to build for **Valhall v9** (Mali-G57, the GPU in the
itel VistaTab 30 Pro / MT6789). Upstream Mesa 25.0.7 does **not** support Vulkan
on v9:

- `docs/drivers/panfrost.rst` (25.0.7) leaves the **Vulkan** column empty for
  `G57` / `Valhall (v9)`; only OpenGL/GLES 3.1 and OpenCL are listed.
- `src/panfrost/vulkan/meson.build` has `jm_archs = [6, 7]` and
  `foreach arch : [6, 7, 10]` — no `panvk_v9` library is built, even though
  `valhall_archs = [9, 10]` already lists 9.
- The JM backend (`jm\panvk_vX_cmd_draw.c`, `panvk_vX_cmd_fb_preload.c`, ...) is
  written for **Bifrost** (v6/v7) and hard-uses the `RENDERER_STATE` genxml
  descriptor, which does not exist on Valhall. The handful of
  `PAN_ARCH >= 9` lines in `jm\` only touch shared tiler-context structs.
  Upstream release 24.3.0 states it directly:
  *"panvk: Don't pretend we can compile for v9."*

Enabling v9 therefore stops the build deep inside `libpanvk_v9`
(`no member named 'tsd'`, `undeclared identifier 'MALI_RENDERER_STATE'`, ...).
Finishing it is writing a new Valhall-JM Vulkan backend, not a port — tracked as
a long-term item in `windrv\GRAPHICS-STACK.md`, not a build fix.

## Files

| Patch | What it does | Why it is not enough |
| --- | --- | --- |
| `0004-panvk-enable-valhall-v9.patch` | adds `9` to `jm_archs` and to the `foreach arch` list, so `libpanvk_v9` is generated | the per-arch JM sources are Bifrost-only |
| `0006-panvk-valhall-jm-fb-preload.patch` | routes arch 9 to the JM branch of `panvk_vX_cmd_fb_preload.c` | that branch packs Bifrost `RENDERER_STATE`/`DRAW` fields |

To experiment: move them back up one directory and run
`tools\patch-mesa.ps1`, then `tools\build-mesa.ps1 -Reconfigure`.
