/*
 * err.h  -  minimal BSD <err.h> stand-in for Windows.
 *
 * src/panfrost/midgard/midgard_compile.c includes <err.h> (currently without
 * using anything from it), and BSD/Mingw environments provide it. MSVC does
 * not, so the build stops with "'err.h' file not found".
 *
 * The functions below follow the BSD interface. They are static inline, not
 * macros, so they cannot collide with identifiers named err/warn.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_COMPAT_ERR_H
#define MALI_COMPAT_ERR_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static inline void vwarnx(const char *fmt, va_list ap)
{
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

static inline void vwarn(const char *fmt, va_list ap)
{
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

static inline void warnx(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwarnx(fmt, ap);
    va_end(ap);
}

static inline void warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwarn(fmt, ap);
    va_end(ap);
}

static inline void verrx(int eval, const char *fmt, va_list ap)
{
    vwarnx(fmt, ap);
    exit(eval);
}

static inline void verr(int eval, const char *fmt, va_list ap)
{
    vwarn(fmt, ap);
    exit(eval);
}

static inline void errx(int eval, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    verrx(eval, fmt, ap);
    va_end(ap);
}

static inline void err(int eval, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    verr(eval, fmt, ap);
    va_end(ap);
}

#endif /* MALI_COMPAT_ERR_H */