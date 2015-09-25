/* Scenarist Closed Caption *.scc file reader library
 * (C) 2013-2015 Jonathan Campbell */

#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>

#include "scc-reader.h"

void scc_eia608_reader_free(scc_eia608_reader *x) {
	memset(x,0,sizeof(*x));
}

void scc_eia608_reader_init(scc_eia608_reader *x) {
	memset(x,0,sizeof(*x));
	x->timebase_num = 30000; /* assume NTSC */
	x->timebase_den = 1001;
}

scc_eia608_reader *scc_eia608_reader_create() {
	scc_eia608_reader *x = (scc_eia608_reader*)malloc(sizeof(scc_eia608_reader));
	if (x == NULL) return NULL;
	scc_eia608_reader_init(x);
	return x;
}

scc_eia608_reader *scc_eia608_reader_destroy(scc_eia608_reader *x) {
	if (x) {
		scc_eia608_reader_free(x);
		free(x);
	}
	return NULL;
}

signed long scc_eia608_reader_get_word(scc_eia608_reader *x,sliding_window_v4 *w,int eof) {
	signed long r = -1;
	const char *p;

	if (x == NULL || w == NULL) return -1L;

	while (x->wait_for_newline && w->data < w->end) {
		if (*(w->data) == '\n') {
			x->wait_for_newline = 0;
			x->got_timecode = 0;
		}
		(w->data)++;
	}

	if (x->wait_for_newline) return -1L;
	if (w->data == w->end) return -1L;
	if (*(w->data) == '\n') {
		x->wait_for_newline = 0;
		x->got_timecode = 0;
		(w->data)++;
	}

	p = (const char*)(w->data);

	if (!x->got_timecode) {
		unsigned int H,M,S,F;

		if (!eof && sliding_window_v4_data_available(w) < (eof?1:128)/*MORE THAN ENOUGH FOR AN ASCII TIMECODE*/)
			return -1L;

		/* skip whitespace */
		while (w->data < w->end && (*(w->data) == ' ' || *(w->data) == '\t')) (w->data)++;
		if (w->data >= w->end) return -1L;

		/* the timecode takes the form 00:00:00:00 immediately followed by a tab */
		if (!isdigit(*(w->data))) {
			x->wait_for_newline = 1; /* Ooookay, it's not a timecode. ignore it */
			return -1L;
		}

		/* store the pointer so we can rollback to the start of the line in case the buffer does not contain the whole timecode */
		p = (const char*)(w->data);

		/* hour field */
		if (!sliding_window_v4_safe_strtoui(w,&H,10)) goto abrupt_ending;
		if (*(w->data) != ':') goto invalid_line_data;
		(w->data)++;

		/* minute field */
		if (!sliding_window_v4_safe_strtoui(w,&M,10)) goto abrupt_ending;
		if (*(w->data) != ':') goto invalid_line_data;
		(w->data)++;

		/* second field */
		if (!sliding_window_v4_safe_strtoui(w,&S,10)) goto abrupt_ending;
		if (*(w->data) != ':' && *(w->data) != ';') goto invalid_line_data;
		(w->data)++;

		/* frame field */
		if (!sliding_window_v4_safe_strtoui(w,&F,10)) goto abrupt_ending;

		/* It's supposed to be a \t but prepare to skip spaces too */
		while (w->data < w->end && (*(w->data) == ' ' || *(w->data) == '\t')) (w->data)++;
		if (w->data >= w->end) goto abrupt_ending;

		/* it's good. the remainder of the line is 4-digit hexadecimal codes */
		p = (const char*)(w->data);
		x->got_timecode = 1;

		/* use the timebase to convert the timecode to frames */
		{
			unsigned long long t;

			t  = (unsigned long long)H * 3600ULL;
			t += (unsigned long long)M * 60ULL;
			t += (unsigned long long)S;

			t *= (unsigned long long)x->timebase_num;
			t /= (unsigned long long)x->timebase_den;
			t += (unsigned long long)F;

			if (x->current_frame < t) x->current_frame = t;
		}
	}

	/* skip whitespace */
	while (w->data < w->end && (*(w->data) == ' ' || *(w->data) == '\t')) (w->data)++;
	if (w->data >= w->end) goto abrupt_ending;

	/* it might be a newline */
	if (*(w->data) == '\n') {
		x->wait_for_newline = 0;
		x->got_timecode = 0;
		return -1L;
	}

	/* should be a hexadecimal number */
	if (isxdigit(*(w->data))) {
		unsigned int t;

		if (!sliding_window_v4_safe_strtoui(w,&t,16)) goto abrupt_ending;
		r = (signed long)t;
		x->current_frame++;
	}
	else {
		x->wait_for_newline = 1;
		return -1L;
	}

	return r;
invalid_line_data:
	x->wait_for_newline = 1; /* remember to skip the remainder of the line */
	w->data = (unsigned char*)p; /* rollback and exit */
	return -1L;
abrupt_ending:
	w->data = (unsigned char*)p; /* rollback and exit */
	return -1LL;
}

