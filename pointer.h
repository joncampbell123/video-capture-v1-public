#ifndef __ISP_UTILS_V4_CPU_POINTER_H
#define __ISP_UTILS_V4_CPU_POINTER_H

#include <stdint.h>
#include <string.h>

/* convert point to raw size_t for numerical use */
#define rawptr(x)			((size_t)((unsigned char*)(x)))

/* pointer to raw size_t combined (logical OR) for use in determining whether ALL pointers are aligned
 * (whether the OR of all pointer values ANDed by the LSB mask is zero), for example to detect if 3
 * pointers are all SSE aligned and act on it:
 *
 * if (sse_aligned(ptr3_aligncombo(p1,p2,p3))) { ... } */
#define ptr2_aligncombo(a,b)		(rawptr(a) | rawptr(b))
#define ptr3_aligncombo(a,b,c)		(rawptr(a) | rawptr(b) | rawptr(c))
#define ptr4_aligncombo(a,b,c,d)	(rawptr(a) | rawptr(b) | rawptr(c) | rawptr(d))

/* test whether a pointer is aligned to various units: */
/* is the pointer aligned to a page boundary? */
#define page_aligned(x)			((rawptr(x) & (SYSTEM_PAGE_SIZE-(size_t)1)) == 0)
/* is the pointer aligned for use with Intel AVX instructions (32-byte boundary?) */
#define avx_aligned(x)			((rawptr(x) & ((size_t)0x1F)) == 0)
/* is the pointer aligned for use with Intel SSE instructions (16-byte boundary?) */
#define sse_aligned(x)			((rawptr(x) & ((size_t)0x0F)) == 0)
/* is the pointer aligned for use with Intel MMX instructions (8-byte boundary?) */
#define mmx_aligned(x)			((rawptr(x) & ((size_t)0x07)) == 0)
/* is the pointer quadword (64-bit) aligned? */
#define uint64_aligned(x)		((rawptr(x) & ((size_t)0x07)) == 0)
/* is the pointer doubleword (32-bit) aligned? */
#define uint32_aligned(x)		((rawptr(x) & ((size_t)0x03)) == 0)
/* is the pointer word (16-bit) aligned? */
#define uint16_aligned(x)		((rawptr(x) & ((size_t)0x01)) == 0)

/* alloca() with padding to force 16-byte alignment for use with SSE operations */
#define alloca_sse(x)			((void*) ((((size_t)alloca(((size_t)x)+15)) + 15) & (~15)))

/* macro to detect pointer overflow cases.
 * code can use this to increase security by detecting whether the sum of
 * a memory location and a length would overflow the datatype and wrap
 * back around from the beginning of memory, potentially referring to
 * or corrupting unrelated data there.
 *
 * NTS: Do not use when compiling with GCC and the -f option to enable
 *      checking for arithmetic overflow, because this code uses arithmetic
 *      overflow on purpose to detect overflow */
static inline size_t _size_t_overflow(const size_t a,const size_t b) { return (a+b) < (a); }
#define size_t_overflow(a,b) _size_t_overflow((const size_t)(a),(const size_t)(b))

#if defined(PAGE_SIZE)
# define SYSTEM_PAGE_SIZE		((size_t)(PAGE_SIZE))
#elif defined(__amd64__)
# define SYSTEM_PAGE_SIZE		((size_t)4096)
#elif defined(__i386__)
# define SYSTEM_PAGE_SIZE		((size_t)4096)
#else
# define SYSTEM_PAGE_SIZE		((size_t)4096)
#endif

#endif /* __ISP_UTILS_V4_CPU_POINTER_H */

