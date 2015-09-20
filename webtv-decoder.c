/* Decoder functions for Interactive TV/WebTV data transmitted on Line 21 TEXT2 channel.
 *
 * TODO: Code (if asked) to pick out the various fields and return them to the caller.
 *
 * Fields of the string:
 *
 * <url>
 * [t:p]        Link type: Program
 * [t:n]        Link type: Network
 * [t:s]        Link type: Station
 * [t:a]        Link type: Sponsor
 * [t:o]        Link type: Operator
 * [type:...]   Link type: (whatever follows type:)
 * [n:...]      Descriptive name of the program
 * [e:YYYYMMDD] Expiration date of the link
 * [e:YYYYMMDDTHHMM] Expiration date with hour and minute in UTC. Example: 20100401T0430 for April 1st 2010, 4:30 AM UTC
 * [s:...]      Javascript trigger (?)
 * [v:...]      View format. Can be 'web' or 'tv'
 *
 * Note: [n:...] can also be [name:...]
 *       [s:...] or [script:...] 
 *       [e:...] or [expires:...]
 *       [v:...] or [view:...]
 */

#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>

#include <isp-utils-v4/eia-608/webtv-decoder.h>

void eia608_webtv_reader_init(eia608_webtv_reader *i) {
	memset(i,0,sizeof(*i));
}

void eia608_webtv_reader_reset(eia608_webtv_reader *i) {
	i->write = 0;
	i->assembly[i->write] = 0;
}

int eia608_webtv_checksum(eia608_webtv_reader *i) {
	unsigned long chk,mychk;
	unsigned char *c,*f;

	/* look for the checksum at the end. */
	c = i->assembly + i->write - 1;
	while (c >= i->assembly && *c != ']') c--;
	if (c < i->assembly) return 0;
	if (*c != ']') return 0;
	c--;
	while (c >= i->assembly && *c != '[') c--;
	if (c < i->assembly) return 0;
	if (*c != '[') return 0;
	f = c;
	c++; /* step forward again, read 4 hex digits */
	chk = (unsigned long)strtoul((char*)c,NULL,16);

	c = i->assembly;
	mychk = 0;
	while (c < f) {
		mychk += ((unsigned long)(*c++)) << 8UL;
		if (c < f) mychk += (unsigned long)(*c++);
		while (mychk >= 0x10000UL) mychk = (mychk & 0xFFFFUL) + (mychk >> 16UL);
	}
	mychk = mychk ^ 0xFFFF;
	return (mychk == chk);
}

/* example data: <http://www.kcpq.com>[t:s][n: ][e:20100101][v:web][583B] */
int eia608_webtv_on_complete_line(eia608_webtv_reader *i) {
	int r;

	if (i->write == 0) return 0;
	r = eia608_webtv_checksum(i);
	i->write = 0;
	return r;
}

int eia608_webtv_take_word(eia608_webtv_reader *i,uint16_t word) {
	word &= 0x7F7F;
	if ((word&0x6000) != 0) {
		if ((i->write+2) < sizeof(i->assembly)) {
			i->assembly[i->write++] = word >> 8;
			i->assembly[i->write++] = word & 0xFF;
			i->assembly[i->write  ] = 0;
		}
	}
	else {
		word &= ~0x800;
		if (word == 0x142D) {
			if (eia608_webtv_on_complete_line(i)) return 1;
		}
	}

	return 0;
}

