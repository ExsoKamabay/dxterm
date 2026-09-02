/*
 * Minimal stand-in for Samba's lib/replace "replace.h", sized for exactly what
 * talloc.c needs on Android/bionic.
 *
 * Why this file exists at all
 * --------------------------
 * talloc ships as part of the Samba tree and expects waf to run a configure
 * pass that probes the host and emits config.h plus a large portability layer.
 * Cross-compiling waf to Android means maintaining a "cross answers" file: a
 * hand-written list of what every probe would have returned had it been able to
 * run on the target. That file is a second source of truth that drifts silently
 *, a wrong answer produces a library that builds and then misbehaves.
 *
 * talloc.c is one translation unit and the portability surface it actually
 * touches is small enough to state explicitly, which is what this header does.
 * Every define below is a claim about bionic that can be checked by reading the
 * NDK sysroot, not a guess carried over from another platform.
 *
 * If a talloc upgrade starts needing something new, the compiler says so. That
 * is the intended failure mode: a build error, not a silent behavioural change.
 */
#ifndef DRACXTERM_TALLOC_BIONIC_REPLACE_H
#define DRACXTERM_TALLOC_BIONIC_REPLACE_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>

/*
 * talloc.c guards its randomised-magic initialiser on this. clang supports
 * __attribute__((constructor)) on Android; without the define talloc falls back
 * to a fixed magic value and prints a #warning saying its hardening is off.
 */
#ifndef HAVE_CONSTRUCTOR_ATTRIBUTE
#define HAVE_CONSTRUCTOR_ATTRIBUTE 1
#endif

/*
 * Without this talloc redefines va_copy as a plain struct assignment, which is
 * wrong on AArch64: va_list is an aggregate there, and copying it by assignment
 * makes the two lists share state. clang provides a real va_copy.
 */
#ifndef HAVE_VA_COPY
#define HAVE_VA_COPY 1
#endif

/*
 * bionic declares memset_explicit only from API 34 (see string.h,
 * __INTRODUCED_IN(34)). This project targets minSdk 24, so on anything below 34
 * the symbol is neither declared nor linkable and talloc.c fails to compile.
 *
 * The replacement must not be optimised away, that is the entire point of the
 * function, which talloc uses to scrub freed memory. The empty asm with a
 * "memory" clobber and the pointer as an input is the standard way to make the
 * compiler treat the write as observable.
 */
#if !defined(__ANDROID_API__) || __ANDROID_API__ < 34
static inline void *dracxterm_memset_explicit(void *block, int c, size_t size)
{
	memset(block, c, size);
	__asm__ __volatile__("" : : "r"(block) : "memory");
	return block;
}
#define memset_explicit(b, c, n) dracxterm_memset_explicit((b), (c), (n))
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef PRINTF_ATTRIBUTE
#define PRINTF_ATTRIBUTE(a, b) __attribute__((__format__(__printf__, a, b)))
#endif

#ifndef _PUBLIC_
#define _PUBLIC_ __attribute__((visibility("default")))
#endif
#ifndef _DEPRECATED_
#define _DEPRECATED_ __attribute__((deprecated))
#endif

#ifndef discard_const
#define discard_const(ptr) ((void *)((uintptr_t)(ptr)))
#endif
#ifndef discard_const_p
#define discard_const_p(type, ptr) ((type *)discard_const(ptr))
#endif

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#endif /* DRACXTERM_TALLOC_BIONIC_REPLACE_H */
