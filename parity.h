
#ifndef __ISP_UTILS_V4_MISC_PARITY_H
#define __ISP_UTILS_V4_MISC_PARITY_H

#include <stdint.h>

static inline unsigned char parity8_eo(unsigned char c,unsigned char p) {
	unsigned char pp;
	for (pp=0;pp < 8;pp++) p ^= (c>>pp)&1;
	return p;
}

#ifndef parity8_odd
#define parity8_odd(c) parity8_eo(c,1)
#endif

#ifndef parity8_even
#define parity8_even(c) parity8_eo(c,0)
#endif

#ifndef u8_parity
#define u8_parity(c) parity8_even(c)
#endif

/* SMPTE HD-SDI vertical ancillary parity functions */

static inline unsigned int smpte_sdi_vanc_8_10_parity(unsigned int x) {
	x &= 0xFF;
	x |= parity8_even(x) << 8;
	x |= ((~x) & 0x100) << 1;
	return x;
}

static inline unsigned int smpte_sdi_vanc_9_10_parity(unsigned int x) {
	x &= 0x1FF;
	x |= ((~x) & 0x100) << 1;
	return x;
}

static inline uint16_t eia608_cc_parity(uint16_t word) {
	/* make sure the word has the right parity bits */
	word &= 0x7F7F;
	word |= u8_parity(word>>8) == 0   ? 0x8000 : 0x0000;
	word |= u8_parity(word&0xFF) == 0 ?   0x80 :   0x00;
	return word;
}

#endif /* __ISP_UTILS_V4_MISC_PARITY_H */

