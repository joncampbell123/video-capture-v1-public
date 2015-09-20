/* Windows GUID support
 * Common Windows-based structures
 * (C) 2012-2015 Jonathan Campbell */

#ifndef __ISP_UTILS_V4_GUID_H
#define __ISP_UTILS_V4_GUID_H

#include <stdint.h>

#include "informational.h"

#ifdef __cplusplus
extern "C" {
#endif

/* [doc] windows_GUID
 *
 * Packed portable representation of the Microsoft Windows GUID
 * structure.
 */
typedef struct {					/* (sizeof) (offset hex) (offset dec) */
	uint32_t _Little_Endian_	a;		/* (4)   +0x00 +0 */
	uint16_t _Little_Endian_	b,c;		/* (2,2) +0x04 +4 */
	uint8_t _Little_Endian_		d[2];		/* (2)   +0x08 +8 */
	uint8_t _Little_Endian_		e[6];		/* (6)   +0x0A +10 */
} __attribute__((packed)) windows_GUID;			/* (16)  =0x10 =16 */
#define windows_GUID_size (16)

unsigned char windows_IsEqualGUID(const windows_GUID *a,const windows_GUID *b);

#ifdef __cplusplus
}
#endif

#endif

