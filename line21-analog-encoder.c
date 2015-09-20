#include <assert.h>
#include <stdint.h>
#include <fcntl.h>

#include <math.h>

#include "line21-analog-encoder.h"

#include "config.h"

unsigned char*			eia608_line21_sinewave = NULL;

void eia608_line21_sine_free() {
	if (eia608_line21_sinewave) free(eia608_line21_sinewave);
	eia608_line21_sinewave = NULL;
}

int eia608_line21_sine_init() {
	size_t i=0;
	double a;

	if (eia608_line21_sinewave) return 0;
	if ((eia608_line21_sinewave=malloc(LINE21_SINE_LENGTH)) == NULL) return -1;

	/* generate 7 cycles */
	for (i=0;i < LINE21_SINE_LENGTH;i++) {
		a = ((double)i * 7 * M_PI * 2) / LINE21_SINE_LENGTH;
		a = (sin(a - (M_PI/2)) + 1) / 2;
		eia608_line21_sinewave[i] = 16 + (int)floor((a*(128-16)) + 0.5);
	}

	return 0;
}

/* NTS: This code makes no attempt to add the parity bits. Caller must do it. */
/* NTS: This code generates the sine wave at 50 IRE, but the data bits at higher
 *      amplitude. My ViewSonic TV set appears to have issues with closed
 *      captioning when the sine wave maximum amplitude is the same as the data bits --J.C. */
void eia608_decoder_generate_analog_line21(unsigned char *out,size_t width,uint16_t word) {
	const size_t f4_step = 1 << 4UL;
	const size_t total_bits = 1/*trailing blank*/ + 16/*CC word*/ + 3/*start*/ + 7/*clock*/;
	size_t pulse_width_f4 = (width << 4UL) / total_bits; /* pixels per bit */
	size_t start_at = (pulse_width_f4 + 1UL) >> 1UL; /* start at 1/2 of a clock which seems to be a norm somehow */
	const unsigned char startcode = 1; /* 001 */
	unsigned char *fence = out + width;
	size_t i,dst,strt,idx,si,sif,sis;

	if (eia608_line21_sine_init()) return;

	/* leading blank */
	for (i=0;i < start_at;i += f4_step) *out++ = 16;

	/* 7-tick clock run-in (sine wave) */
	/* TODO: We should really consider using a lookup table for this part */
	strt = i;
	dst = i + (7 * pulse_width_f4);
	si = sif = 0;
	sis = (LINE21_SINE_LENGTH << (12+4)) / (7 * pulse_width_f4);
	for (;i < dst;i += f4_step) {
#if ENABLE_debug
		assert(si < LINE21_SINE_LENGTH);
#endif
		*out++ = eia608_line21_sinewave[si];
		sif += sis; si += sif >> 12; sif &= 0xFFF;
	}
	assert(i < (width << 4UL));

	/* 3-tick start code (0 0 1) */
	strt = i;
	dst = i + (3 * pulse_width_f4);
	for (;i < dst;i += f4_step) {
		idx = ((i-strt) * 3) / (dst-strt);
		assert(idx < 3);
		*out++ = (startcode & (1 << (2-idx))) ? 160 : 16;
	}
	assert(i < (width << 4UL));

	/* reformat word to proper byte order */
	word = (word << 8) | (word >> 8);

	/* 16-tick word value (lsb to msb) */
	strt = i;
	dst = i + (16 * pulse_width_f4);
	for (;i < dst;i += f4_step) {
		idx = ((i-strt) * 16) / (dst-strt);
		assert(idx < 16);
		*out++ = (word & (1 << idx)) ? 160 : 16;
	}
	assert(i < (width << 4UL));

	while (out < fence) *out++ = 16;
}

