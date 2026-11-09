#!/usr/bin/env bash
#
# gen-libpan-wsl.sh - generate the PanVK "libpan" shader artifacts on a Linux
# host (WSL2 works) so the Windows ARM64 build does not need mesa_clc.
#
# WHY THIS EXISTS
#   Mesa 25.1+ compiles src/panfrost/libpan/*.cl and src/poly/cl/*.cl to SPIR-V
#   at build time with `mesa_clc`, which needs LLVM + Clang + SPIRV-Tools +
#   SPIRV-LLVM-Translator. PanVK links libpan unconditionally and enables
#   src/poly, so a PanVK build is impossible without it. This host has no LLVM
#   headers and its clang has no SPIR-V target, so we run the two host-only
#   tools on Linux and feed the generated C to Windows (-Dpanfrost-pregen).
#
#   Both tools are build-time only and their output is plain C, with no ABI
#   dependency on the machine that produced it. This is exactly what a normal
#   cross build does; only the "host" half runs on Linux instead of Windows.
#
# WHAT IT WRITES
#   - packages are fetched with `apt-get download` and unpacked with `dpkg -x`;
#     nothing is installed and no system package state changes.
#   - the .deb cache lives in the repo: external/wsl-deps/deb
#   - build scratch lives in $MT6789_WSL_SCRATCH (default ~/.cache/mt6789-mesa),
#     because Meson does not quote paths containing spaces correctly and this
#     repo path contains one ("iOS 27"). Only the generated C comes back.
#   - the generated files land in the repo, at the paths the Windows build
#     consumes them from: windrv/mesa/pregen/
#
# USAGE
#   windrv/mesa/tools/gen-libpan-wsl.sh              # normal run
#   windrv/mesa/tools/gen-libpan-wsl.sh --refresh-src # re-copy Mesa sources
#   windrv/mesa/tools/gen-libpan-wsl.sh --clean        # wipe scratch first
#
# PREREQUISITES (host, already present on a stock Ubuntu 26.04 WSL image)
#   clang/clang++ >= 17, llvm-dev, libclang-dev, meson, ninja, cmake,
#   pkg-config, python3, and the Mesa source at external/mesa-v9 with the
#   FourFectVK patch stack applied (see patches-v9/).
#
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SRC="$REPO/external/mesa-v9"
DEBS="$REPO/external/wsl-deps/deb"
OUT="$REPO/windrv/mesa/pregen"
SCRATCH="${MT6789_WSL_SCRATCH:-$HOME/.cache/mt6789-mesa}"
BUILD="$SCRATCH/build"
SRCCOPY="$SCRATCH/mesa"
DEPS="$SCRATCH/deps"

REFRESH_SRC=0
for a in "$@"; do
  case "$a" in
    --refresh-src) REFRESH_SRC=1 ;;
    --clean) rm -rf "$SCRATCH" ;;
    *) echo "unknown arg: $a" >&2; exit 2 ;;
  esac
done

die() { echo "ERROR: $*" >&2; exit 1; }

echo "== repo    : $REPO"
echo "== scratch : $SCRATCH"

[ -d "$SRC" ] || die "Mesa source not found at $SRC (run fetch-mesa + apply patches-v9)"
mkdir -p "$DEBS" "$SCRATCH" "$OUT"

# ---------------------------------------------------------------------------
# 1. dependencies: download (if needed) and unpack into the scratch prefix
# ---------------------------------------------------------------------------
PKGS="libclc-21 libclc-21-dev libllvmspirvlib-21-dev libllvmspirvlib21.1 \
      spirv-tools spirv-tools-dev spirv-tools-headers python3-mako python3-markupsafe"

have_all=1
for p in $PKGS; do
  compgen -G "$DEBS/${p}_*.deb" >/dev/null || have_all=0
done
if [ "$have_all" -eq 0 ]; then
  echo "== fetching packages into $DEBS (download only, no install)"
  ( cd "$DEBS" && apt-get download $PKGS ) || die "apt-get download failed"
fi

echo "== unpacking packages into $DEPS"
rm -rf "$DEPS"; mkdir -p "$DEPS"
for f in "$DEBS"/*.deb; do dpkg -x "$f" "$DEPS"; done

# pkg-config files ship prefix=/usr; point them at the unpacked prefix.
sed -E -i "s|^libexecdir=.*|libexecdir=$DEPS/usr/lib/clc|" \
  "$DEPS/usr/lib/pkgconfig/libclc.pc"
sed -E -i "s|^prefix=.*|prefix=$DEPS/usr|; s|^libdir=.*|libdir=\${prefix}/lib/x86_64-linux-gnu|" \
  "$DEPS/usr/lib/x86_64-linux-gnu/pkgconfig/LLVMSPIRVLib.pc"
sed -E -i "s|^prefix=.*|prefix=$DEPS/usr|" \
  "$DEPS/usr/lib/x86_64-linux-gnu/pkgconfig/SPIRV-Tools.pc"

export PKG_CONFIG_PATH="$DEPS/usr/lib/pkgconfig:$DEPS/usr/lib/x86_64-linux-gnu/pkgconfig"
export CMAKE_PREFIX_PATH="$DEPS/usr"
export PYTHONPATH="$DEPS/usr/lib/python3/dist-packages"
export CC="${CC:-clang}"
export CXX="${CXX:-clang++}"

# ---------------------------------------------------------------------------
# 2. Mesa source copy (space-free path, patch stack preserved)
# ---------------------------------------------------------------------------
if [ "$REFRESH_SRC" -eq 1 ] || [ ! -d "$SRCCOPY" ]; then
  echo "== copying Mesa source into the scratch tree"
  rm -rf "$SRCCOPY"; mkdir -p "$SRCCOPY"
  ( cd "$SRC"; tar --exclude=.git --exclude=build-wsl -cf - . ) | ( cd "$SRCCOPY"; tar -xf - )
fi

# ---------------------------------------------------------------------------
# 3. configure + build the libpan chain
# ---------------------------------------------------------------------------
if [ ! -f "$BUILD/build.ninja" ]; then
  echo "== meson setup"
  meson setup "$BUILD" "$SRCCOPY" \
    --buildtype=release \
    -Dplatforms= \
    -Dgallium-drivers= \
    -Dvulkan-drivers=panfrost \
    -Dopengl=false \
    -Dgles2=disabled \
    -Dglx=disabled \
    -Dllvm=enabled \
    -Dpanfrost-kmds=panfrost \
    -Dallow-fallback-for=libdrm
fi

TARGETS=""
for v in 4 5 6 7 9 10 11 12 13 14; do
  TARGETS="$TARGETS src/panfrost/libpan/libpan_shaders_v$v.c src/panfrost/libpan/libpan_v$v.cpp"
done
# libpoly is a second mesa_clc consumer on the panfrost path (src/poly/cl).
TARGETS="$TARGETS src/poly/cl/libpoly.cpp"

echo "== building mesa_clc / vtn_bindgen2 / panfrost_compile + libpan artifacts"
ninja -C "$BUILD" $TARGETS

# ---------------------------------------------------------------------------
# 4. copy the generated files back into the repo
# ---------------------------------------------------------------------------
echo "== copying generated artifacts into $OUT"
mkdir -p "$OUT"
for v in 4 5 6 7 9 10 11 12 13 14; do
  for f in "libpan_v$v.cpp" "libpan_v$v.h" "libpan_shaders_v$v.c" "libpan_shaders_v$v.h"; do
    [ -f "$BUILD/src/panfrost/libpan/$f" ] || die "missing generated file: $f"
    cp -f "$BUILD/src/panfrost/libpan/$f" "$OUT/$f"
  done
done
for f in libpoly.cpp libpoly.h; do
  [ -f "$BUILD/src/poly/cl/$f" ] || die "missing generated file: $f"
  cp -f "$BUILD/src/poly/cl/$f" "$OUT/$f"
done

echo "== manifest"
( cd "$OUT" && sha256sum libpan_*.c libpan_*.cpp libpan_*.h libpoly.* | sort -k2 ) > "$OUT/SHA256SUMS"
cat "$OUT/SHA256SUMS"
echo "== done"