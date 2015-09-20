#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

#include <isp-utils-v4/misc/parity.h>
#include <isp-utils-v4/eia-608/eia608-decoder.h>
#include <isp-utils-v4/eia-608/line21-analog-decoder.h>

long eia608_decoder_parse_analog_line21(unsigned char *scanline,size_t in_scan_width,eia608_analog_line21_decoder_state *s) {
	size_t i,pulsew,pulsewa,minpulse,maxpulse,avgpulse,thr,reading[256],mins=255,maxs=0,skipped;
	const size_t total_samples = ((((size_t)(16 + 3 + 7)) * 2));
	eia608_analog_line21_decoder_state os;
	long ret = -1L;

	if (in_scan_width < 64) return -1L; /* not worth it */
	if (s->recursion == 0) os = *s;

	for (i=0;i < in_scan_width;i++) {
		if (mins > scanline[i]) mins = scanline[i];
		if (maxs < scanline[i]) maxs = scanline[i];
	}
	if (s->offset >= 0) {
		if (s->minv > (mins+3)) s->minv -= 4;
		else if (s->minv < (mins-3)) s->minv += 4;
		if (s->maxv > (maxs+3)) s->maxv -= 4;
		else if (s->maxv < (maxs-3)) s->maxv += 4;
		mins = s->minv;
		maxs = s->maxv;
	}
	if (mins > maxs) mins = maxs;
	if (maxs < mins) maxs = mins;
	thr = (mins+maxs)/2UL;
	s->last_thr = thr;

	/* skip whitespace */
	if (s->offset >= 0) {
		i=(size_t)s->offset + 8;
		if (i > 1) i -= 2;
		for (;i <= ((size_t)s->offset + 8 + 2);i++) {
			if (scanline[i] >= thr) break;
		}
	}
	else {
		for (i=0;i < (in_scan_width/4);i++) {
			if (scanline[i] >= thr) break;
		}
	}
	i -= 8;/*<-FIXME*/
	if ((ssize_t)i < 0) i = 0;
	in_scan_width -= i;
	if ((ssize_t)in_scan_width < 64) return -1L;
	scanline += i;
	skipped = i;

	/* 16 data bits + 3 start + 7 clock + 1 extra clock at end, 4 bit fixed pt, enough for 2 samples of each bit */
	if (s->offset >= 0 && s->scale > 0) {
		avgpulse = (size_t)s->scale;
		minpulse = (avgpulse * 19UL) / 20UL;
		maxpulse = (avgpulse * 21UL) / 20UL;
	}
	else {
		avgpulse = (in_scan_width << 4UL) / total_samples;
		minpulse = (avgpulse * 6UL) / 10UL;
		maxpulse = (avgpulse * 14UL) / 10UL;
	}

	/* look for a 7-tick sine wave */
	pulsewa = 1;
	for (pulsew=minpulse;pulsew <= maxpulse;pulsew += pulsewa) {
		int clock_tolerance = (s->offset >= 0 ? 4 : 0);
		unsigned int w=0;

		for (i=0;i < total_samples;i++) {
			size_t j = (size_t)((i*pulsew)>>4);
			if (j >= in_scan_width) j = in_scan_width - 1;
			reading[i] = (scanline[j] >= thr) ? 1 : 0;
		}

		/* look for 0 1|0 1|0 1|0 1 | 0 1|0 1|0 1|0 0 | 0 0|0 0|1 1 (7 clock pulses) */
		for (i=0;i < 14;i += 2) {
			if (!(reading[i] == 0 && reading[i+1] == 1)) {
				if (--clock_tolerance < 0) break;
			}
		}
		/* no match, continue searching */
		if (i < 14) continue;

		if (!(reading[14] == 0 && reading[15] == 0 &&
			reading[16] == 0 && reading[17] == 0 &&
			reading[18] == 1 && reading[19] == 1)) continue;

		/* OK. Assemble the 16-bit word */
		for (i=0;i < 16;i++) {
			char ok = 0;
			size_t io = (i*2) + 20;
			assert((io+1) < total_samples);

			ok = (i == 15 || reading[io] == reading[io+1]);
			if (ok) {
				w |= reading[io] << i;
			}
			else {
				break;
			}
		}
		if (i < 16) continue;

		/* check parity */
		if (u8_parity(w>>8) == 0 || u8_parity(w&0xFF) == 0) continue;

		/* convert to the hi-byte lo-byte order the rest of this code is accustomed to */
		ret = (signed long)(((w >> 8) | (w << 8)) & 0xFFFF);

		/* store state */
		s->offset = (int)skipped;
		s->scale = pulsew;
		s->failures = 0;
		s->minv = mins;
		s->maxv = maxs;
		break;
	}

	if (ret == -1L) {
		if (s->recursion < 4) {
			s->recursion++;
			ret = eia608_decoder_parse_analog_line21(scanline,in_scan_width,s);
			s->recursion--;
		}
	}

	/* don't let temporary failure mess up the state */
	if (s->recursion == 0) {
		if (ret == -1L) {
			if (os.failures >= 10) {
				eia608_decoder_analog_line21_decoder_state_init(s);
			}
			else {
				*s = os;
				s->failures++;
			}
		}
		else {
			if (s->failures > 0)
				s->failures--;
		}
	}

	return ret;
}

void eia608_decoder_analog_line21_decoder_state_init(eia608_analog_line21_decoder_state *s) {
	s->recursion = 0;
	s->failures = 0;
	s->last_thr = 0;
	s->offset = -1;
	s->scale = 0;
	s->minv = 0;
	s->maxv = 0;
}

void eia608_decoder_analog_line21_decoder_state_dump(FILE *f,eia608_analog_line21_decoder_state *s) {
	fprintf(f,"failures=%-2u offset=%-4d scale=%-4u min=%-3u max=%-3u last-thr=%-3u recur=%d\n",
		s->failures,	s->offset,	s->scale,
		s->minv,	s->maxv,	s->last_thr,
		s->recursion);
}

