/*
 * mali_crt_exe.cpp  -  the minimal console-exe entry point for the CRT-free
 *                      ARM64 link recipe (Jalur B).
 *
 * WHY THIS EXISTS
 * ---------------
 * With /nodefaultlib there is no libcmt/vcruntime, so there is no
 * mainCRTStartup. Without an entry symbol lld-link refuses to link an
 * executable ("subsystem must be defined"). This file provides the entry the
 * way a minimal CRT would: build argc/argv from the Win32 command line, call
 * main, and pass the result to exit (so _crt_atexit handlers, including the
 * stdio flush UCRT registers, still run).
 *
 * It is a separate archive member from the rest of the shim on purpose: the
 * linker pulls it only for executables, and only when the default entry is
 * wanted. A DLL never references mainCRTStartup, so its `main` stays optional.
 *
 * `main` is resolved through /alternatename so a program without one (every
 * Meson link probe is `int main(void) { return 0; }`, but a shared library has
 * none) still links.
 *
 * LIMITATIONS (documented, not hidden)
 * ------------------------------------
 * - The command line is parsed with simple quote handling; it does not
 *   reproduce the CRT's full CommandLineToArgv rules.
 * - envp is passed as null. Programs that call getenv() on their own
 *   environment rely on the UCRT's own environment block, not on envp.
 * Mesa's desktop tools are not built in this configuration, so this is a
 * build-enabling shim, not an application runtime.
 *
 * SPDX-License-Identifier: MIT
 */

#include <windows.h>
#include <stdlib.h>

extern int main(int argc, char **argv, char **envp);
extern "C" int __cdecl mali_crt_default_main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    return 0;
}
#pragma comment(linker, "/alternatename:main=mali_crt_default_main")

#define MALI_CRT_MAX_ARGS 64
#define MALI_CRT_CMDLINE  4096

static char  s_cmdline[MALI_CRT_CMDLINE];
static char *s_argv[MALI_CRT_MAX_ARGS];

static int mali_crt_build_argv(void)
{
    const char *src = GetCommandLineA();
    int argc = 0;
    int n = 0;

    if (!src)
        src = "";

    while (*src && n < MALI_CRT_CMDLINE - 1) {
        while (*src == ' ' || *src == '\t')
            src++;
        if (!*src)
            break;
        if (argc < MALI_CRT_MAX_ARGS)
            s_argv[argc++] = &s_cmdline[n];

        bool in_quote = false;
        while (*src && n < MALI_CRT_CMDLINE - 1) {
            char c = *src;
            if (c == '"') {
                in_quote = !in_quote;
                src++;
                continue;
            }
            if (!in_quote && (c == ' ' || c == '\t'))
                break;
            s_cmdline[n++] = c;
            src++;
        }
        s_cmdline[n++] = '\0';
    }
    while (argc < MALI_CRT_MAX_ARGS)
        s_argv[argc] = nullptr;
    return argc;
}

extern "C" void __cdecl mainCRTStartup(void)
{
    int argc = mali_crt_build_argv();
    exit(main(argc, s_argv, nullptr));
}
