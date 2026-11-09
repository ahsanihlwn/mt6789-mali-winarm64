/*
 * strings.h  -  minimal BSD <strings.h> stand-in for Windows MSVC/ARM64.
 *
 * panfrost/lib/pan_env.h includes <strings.h> unconditionally and uses
 * strcasecmp() to parse the PAN_* debug knobs ("false"/"off"/"no" switch a
 * knob off). The UCRT ships no <strings.h>; it has the same predicates as
 * _stricmp()/_strnicmp() in <string.h>.
 *
 * Only the two functions the tree actually reaches are declared. The
 * definitions - and the __imp_* pointers the dllimport declarations need -
 * live in runtime/mali_posix.cpp.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_COMPAT_STRINGS_H
#define MALI_COMPAT_STRINGS_H

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* MALI_COMPAT_STRINGS_H */
