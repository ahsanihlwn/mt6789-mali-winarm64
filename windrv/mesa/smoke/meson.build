# Mesa cross-toolchain smoke project (Jalur B).
#
# Configure it against the generated cross file:
#   tools\build-smoke.ps1
# or manually:
#   .venv\Scripts\meson.exe setup build\smoke smoke --cross-file build\cross.ini
#
# Success here means "Meson accepts our aarch64-windows-msvc cross file and can
# compile an ARM64 object", which is the first real question of Jalur B. It does
# not imply Mesa itself configures.

project('mali-mesa-smoke', 'c',
        version : '0.1.0',
        meson_version : '>= 1.0.0',
        default_options : ['warning_level=2'])

mali_smoke = static_library('mali_mesa_smoke', 'hello.c')

# Print what the cross environment resolved to, for the record.
message('system   : ' + host_machine.system())
message('cpu      : ' + host_machine.cpu_family())
