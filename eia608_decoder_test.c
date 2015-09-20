
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

#include <SDL/SDL.h>

#include <linux/videodev2.h>		/* for realtime CC capture mode */

#include "unicode.h"
#include "line21-analog-decoder.h"
#include "extended-data-service-decoder.h"
#include "eia608-font-8x14.h"
#include "eia608-decoder.h"
#include "webtv-decoder.h"
#include "eia608-demux.h"
#include "scc-reader.h"
#include "vgafont_8x14.h"
#include "parity.h"
#include "rawint.h"

struct v4l2_mmap_tracking {
	unsigned char*          mmap;
	size_t                  size;
};

/* NTS: To make the 8-wide font more readable especially when italicized,
 *      we render and compute as if 9 pixels wide per char */
#define RENDERED_FONT_WIDTH			(CC_FONT_WIDTH+1)

static int					DIE = 0;
static int					cc2 = 0;
static int					even = 0;
static int					do_xds = 0;
static int					is_scc = 0;
static int					do_text = 0;
static int					rawtext = 0;
static unsigned int				speedup = 0;
static int					dmuxdbg = 0;
static int					rawinput = 0;
static int					is_line21 = 0;
static int					is_v4l_vbi = 0;
static unsigned int				field_rate = 60;
static int					demux_match = 0;
static int					verbose = 0,raw = 0;
static int					mpeg_jumbled_CC1_CC2 = 1; /* Too many DVD samples it seems have jumbled CC1/CC3 so default to on */
static int					mpeg_jumbled_CC1_CC2_backwards = 0; /* ugh... */
static SDL_Surface*				video_surface_SDL = NULL;
static scc_eia608_reader*			scc_reader = NULL;
static eia608_decoder*				eia608_cc = NULL;
static int					line21_line = 21;
static int					fuzz = 0;
static xds_data_assembly*			xds_asm = NULL;
static int					do_webtv = 0;
static eia608_analog_line21_decoder_state	ast;
static int					eia608_debug_thr = 0;
static int					eia608_debug_state = 0;
static int					mpeg_mode = 0;
static int					show_video = 1;
static eia608_demux				eia608_dmux;
static eia608_webtv_reader			webtv;

enum {
	MM_AUTO=0,
	MM_DVD,
	MM_ATSC
};

/* emulates one frame time */
static unsigned long long			cc_framecounter=0;
static unsigned char				emitted_rawtext=0;
static unsigned int				cc_counter=0,cc_space=0;

static void help() {
	fprintf(stderr,"castus_demo_eia608 [options] <pes file>\n");
	fprintf(stderr,"    -h --help                 This help\n");
	fprintf(stderr,"    -v                        Verbose\n");
	fprintf(stderr,"    -text                     Text display mode\n");
	fprintf(stderr,"    -fuzz                     Feed random data into CC decoder\n");
	fprintf(stderr,"    -raw                      Show raw control codes\n");
	fprintf(stderr,"    -rawtext                  Show raw text\n");
	fprintf(stderr,"    -cc2                      Decode CC2/CC4 rather than CC1/CC2\n");
	fprintf(stderr,"    -xds                      Parse XDS data too\n");
	fprintf(stderr,"    -dmuxdbg                  Demux debug\n");
	fprintf(stderr,"    -text                     Decode TEXT channel\n");
	fprintf(stderr,"    -webtv                    Listen to CC2/CC4 for Interactive TV/WebTV data\n");
	fprintf(stderr,"    -even                     Video4linux: read from even field Line 21 to read CC3/CC4\n");
	fprintf(stderr,"    -id                       Input file is a raw dump of 16-bit CC words\n");
	fprintf(stderr,"    -scc                      Input file is a SCC file\n");
	fprintf(stderr,"    -line21[=scan width]      Input file is a raw analog capture of Line 21\n");
	fprintf(stderr,"    -v4l-vbi[=width]          Input file is Video4Linux2 VBI capture device\n");
	fprintf(stderr,"    -jumbled-cc1-cc2          The M2V/MPEG input forgot to set odd or even field number\n");
	fprintf(stderr,"                              in the CC data, thus CC1/CC2 are jumbled together. If\n");
	fprintf(stderr,"                              closed captions are garbled, play slowly, or pop-on CCs\n");
	fprintf(stderr,"                              do not show or flash too briefly, try this option.\n");
	fprintf(stderr,"                              This option may be needed for some DVDs where this mis-authoring\n");
	fprintf(stderr,"                              occurs in the video bitstream.\n");
	fprintf(stderr,"                              Due to the number of MPEG streams out there with jumbled CC1/CC3\n");
	fprintf(stderr,"                              streams this is now default.\n");
	fprintf(stderr,"    -no-jumbled-cc1-cc2       Assume the CC1/CC3 data is NOT jumbled together\n");
	fprintf(stderr,"    -jumbled-backwards        The jumbled CC1/CC3 data is backwards\n");
	fprintf(stderr,"    -mpg-dvd                  Assume data is in MPEG CC user packets (DVD-style CC data)\n");
	fprintf(stderr,"    -mpg-atsc                 Assume data is in MPEG ATSC user packets (EIA-708 carrying EIA-608)\n");
	fprintf(stderr,"    -p60                      Emulate 60fps progressive\n");
	fprintf(stderr,"    -eia608-thr               EIA-608 debugging: show Line21 analog through threshhold\n");
	fprintf(stderr,"    -eia608-state             EIA-608 debugging: show Line21 analog state\n");
	fprintf(stderr,"    -line21on=N               v4l-vbi input: read scan line N instead of Line 21\n");
	fprintf(stderr,"    -video                    Show video (if possible) along with CC\n");
	fprintf(stderr,"    -no-video                 Don't show video\n");
	fprintf(stderr,"\n");
	fprintf(stderr,"   Closed caption 'streams' and how to access them:\n");
	fprintf(stderr,"     CC1                      Main caption channel [default]\n");
	fprintf(stderr,"     CC2                      Secondary caption channel [-cc2]\n");
	fprintf(stderr,"     CC3                      Third caption channel [-even]\n");
	fprintf(stderr,"     CC4                      Fourth caption channel [-even -cc2]\n");
	fprintf(stderr,"     TEXT1                    Main textual channel [-text]\n");
	fprintf(stderr,"     TEXT2                    Secondary textual channel [-text -cc2]\n");
	fprintf(stderr,"     TEXT3                    Third textual channel [-text -even]\n");
	fprintf(stderr,"     TEXT4                    Fourth textual channel [-text -even -cc2]\n");
	fprintf(stderr,"     XDS                      eXtended Data Services (embedded in CC3) [-xds -even]\n");
	fprintf(stderr,"\n");
	fprintf(stderr,"   Supplementary data contained within CC streams:\n");
	fprintf(stderr,"     WebTV/Interactive TV     WebTV link data (embedded in TEXT2) [-text -cc2 -webtv]\n");
	fprintf(stderr,"\n");
	fprintf(stderr,"   If neither -id or -scc is specified, the input file is assumed to be\n");
	fprintf(stderr,"   an MPEG program stream.\n");
	fprintf(stderr,"\n");
	fprintf(stderr,"   Read EIA-608 Closed Caption data and simulate the decoding process on-screen.\n");
	fprintf(stderr,"   (C) 2012-2013 Impact Studio Pro/Castus corporation.\n");
	fprintf(stderr,"   Written by Jonathan Campbell.\n");
}

static void sigma(int __attribute__((unused)) x) {
	if (++DIE >= 10) abort();
}

static int lock_video_screen() {
	if (!SDL_MUSTLOCK(video_surface_SDL)) return 0;
	if (SDL_LockSurface(video_surface_SDL) < 0) return -1;
	assert(video_surface_SDL->pixels != NULL);
	return 0;
}

static void unlock_video_screen() {
	if (!SDL_MUSTLOCK(video_surface_SDL)) return;
	SDL_UnlockSurface(video_surface_SDL);
}

static uint16_t rgb16(unsigned char r,unsigned char g,unsigned char b) {
	return (((uint16_t)(r >> 3)) << 11) +
		(((uint16_t)(g >> 2)) << 5) +
		(((uint16_t)(b >> 3)) << 0);
}

static void scrollup(unsigned int ystart,unsigned int yend) {
	uint16_t *buf;
	size_t stride;

	assert(lock_video_screen() == 0);
	buf = (uint16_t*)(video_surface_SDL->pixels);
	stride = video_surface_SDL->pitch >> 1;

	/* center into title-safe */
	ystart += (video_surface_SDL->h - (CC_FONT_HEIGHT * CC_ROWS)) / 2;
	yend += (video_surface_SDL->h - (CC_FONT_HEIGHT * CC_ROWS)) / 2;
	assert(yend < (unsigned int)video_surface_SDL->h);

	/* update ptr */
	buf += (ystart * stride);

	/* scroll it up */
	memmove(buf,buf+stride,sizeof(*buf)*stride*(yend-ystart));
	buf += stride*(yend-ystart);
	for (unsigned int cc=0;cc < stride;cc++) *buf++ = rgb16(128,128,128);

	unlock_video_screen();

}

static uint16_t rgb16blend(uint16_t a,uint16_t b) {
	unsigned int or,og,ob;

	or = ((a >> 11) + (b >> 11)) >> 1;
	og = (((a >> 5) & 0x3F) + ((b >> 5) & 0x3F)) >> 1;
	ob = ((a & 0x1F) + (b & 0x1F)) >> 1;
	return (or << 11) + (og << 5) + ob;
}

static void redraw_cc_char(unsigned int x,unsigned int y) {
	uint16_t bkcolor,fgcolor,gray;
	CCCHAR *chent;
	uint16_t *buf;
	size_t stride;

	gray = rgb16(128,128,128);
	bkcolor = rgb16(0,0,0);
	fgcolor = rgb16(192,192,192);

	assert(lock_video_screen() == 0);
	buf = (uint16_t*)(video_surface_SDL->pixels);
	stride = video_surface_SDL->pitch >> 1;
	chent = eia608_cc->current_page + (y * CC_COLUMNS) + x;

	/* convert to pixels */
	x *= RENDERED_FONT_WIDTH;
	y *= CC_FONT_HEIGHT;

	/* center into title-safe */
	x += (video_surface_SDL->w - (RENDERED_FONT_WIDTH * CC_COLUMNS)) / 2;
	y += (video_surface_SDL->h - (CC_FONT_HEIGHT * CC_ROWS)) / 2;

	/* update ptr */
	buf += (y * stride) + x;

	/* render char */
	if (!eia608_decoder_cchar_is_transparent(chent->ch)) {
		const unsigned char *c;
		unsigned char *font_bmp = eia608_get_font_8x13_bmp(eia608_cc,chent);
		unsigned int cn = CC_FONT_HEIGHT;

		c = eia608_decoder_get_color(eia608_cc,chent->color);
		fgcolor = rgb16(c[0],c[1],c[2]);
		c = eia608_decoder_get_background_color(eia608_cc,chent->background_color);
		bkcolor = rgb16(c[0],c[1],c[2]);

		do {
			unsigned int bf = ((unsigned int)(*font_bmp++)) << 1U;
			unsigned int ccc = 0;

			if (chent->italic) { /* italic */
				unsigned int shf = cn / 5;

				while (shf-- > 0) {
					*buf++ = bkcolor;
					ccc++;
				}
			}

			if (chent->background_transparent) {
				for (;ccc < 9;ccc++) {
					*buf = (bf & 0x100) ? fgcolor : rgb16blend(*buf,bkcolor);
					bf <<= 1;
					buf++;
				}
			}
			else {
				for (;ccc < 9;ccc++) {
					*buf++ = (bf & 0x100) ? fgcolor : bkcolor;
					bf <<= 1;
				}
			}

			buf += stride - 9;
		} while (--cn != 0);
	}
	else if (!show_video) {
		unsigned int cn = CC_FONT_HEIGHT;

		do {
			for (unsigned int ccc=0;ccc < 9;ccc++) *buf++ = gray;
			buf += stride - 9;
		} while (--cn != 0);
	}

	unlock_video_screen();
}

static void redraw_cc(unsigned char xstart,unsigned char ystart,unsigned char xend,unsigned char yend) {
	unsigned int x,y;

	for (y=ystart;y <= yend;y++) {
		for (x=xstart;x <= xend;x++) {
			redraw_cc_char(x,y);
		}
	}
}

static void on_cc_rollup_screen(void __attribute__((unused)) *ctx,unsigned char ystart,unsigned char yend) {
	if (!show_video) scrollup(ystart*CC_FONT_HEIGHT,((yend+1)*CC_FONT_HEIGHT)-1);
}

static void on_cc_update_screen(void __attribute__((unused)) *ctx,unsigned char xstart,unsigned char ystart,unsigned char xend,unsigned char yend) {
	if (!show_video) redraw_cc(xstart,ystart,xend,yend);
}

static void on_xds_packet(void __attribute__((unused)) *ctx,xds_data_stream *s) {
	char tmp[256];

	if (s->classno == 7) {
		if (s->typeno == 4) { /* Local time zone */
			int delta = 24-((s->data[2]-0x40)&0x1F); /* FIXME: Is this right??? */
			if (delta >= 12) delta -= 24;

			fprintf(stdout,"XDS: Local time zone. GMT%c%u dst=%u\n",
				delta >= 0 ? '+' : '-',
				abs(delta),
				s->data[2]&0x20);
		}
		else if (s->typeno == 1) { /* Time of day */
			const char *dayofweek[8] = {
				"",		"Sunday",	"Monday",	"Tuesday",
				"Wednesday",	"Thursday",	"Friday",	"Saturday"
			};
			unsigned char H,M,DST,zerosec,tapedelay;
			unsigned char leapday,month,day,weekday;
			unsigned int year;

			H = (((unsigned char)s->data[3])&0x1F);
			M = (((unsigned char)s->data[2])&0x7F)-0x40;
			DST = (s->data[3]&0x20)?1:0;
			zerosec = (s->data[5]&0x20)?1:0;
			tapedelay = (s->data[5]&0x10)?1:0;
			leapday = (s->data[4]&0x20)?1:0;
			month = s->data[5]&0xF;
			day = s->data[4]&0x1F;
			year = (s->data[7]-0x40)+1990;
			weekday = s->data[6]&7;

			fprintf(stdout,"XDS: UTC Time of day %04u-%02u-%02u %02u:%02u:%s %s leap=%u dst=%u tapedelay=%u\n",
				year,month,day,
				H,M,zerosec?"00":"xx",
				dayofweek[weekday],
				leapday,
				DST,
				tapedelay);
		}
		else {
			fprintf(stdout,"Unknown class=%u type=%u XDS\n",s->classno,s->typeno);
		}
	}
	else if (s->classno == 5) { /* channel class */
		if (s->typeno == 1) { /* network name */
			if (s->data_length >= sizeof(tmp))
				s->data_length = sizeof(tmp)-1;
			if (s->data_length > 2) {
				memcpy(tmp,s->data+2,s->data_length-2);
				tmp[s->data_length-2] = 0;
				fprintf(stdout,"XDS: Network name '%s'\n",tmp);
			}
		}
		else if (s->typeno == 2) { /* network call letters */
			unsigned int x = s->data_length;
			while (x < 6) s->data[x++] = ' ';
			memcpy(tmp,s->data+2,4); tmp[4] = 0;
			fprintf(stdout,"XDS: Network call letters '%s'\n",tmp);
			if (s->data_length >= (2+4+2)) {
				memcpy(tmp,s->data+2+4,2); tmp[2] = 0;
				fprintf(stdout,"    Broadcast channel %s\n",tmp);
			}
		}
		else if (s->typeno == 4) { /* transmission signal id */
			fprintf(stdout,"XDS: Transmission ID %x%x%x%x\n",
				(s->data[5]&0xF),(s->data[4]&0xF),
				(s->data[3]&0xF),(s->data[2]&0xF));
		}
		else {
			fprintf(stdout,"Unknown class=%u type=%u XDS\n",s->classno,s->typeno);
		}
	}
	else if (s->classno == 1) {
		if (s->typeno == 3) { /* program name */
			if (s->data_length >= sizeof(tmp))
				s->data_length = sizeof(tmp)-1;
			if (s->data_length > 2) {
				memcpy(tmp,s->data+2,s->data_length-2);
				tmp[s->data_length-2] = 0;
				fprintf(stdout,"XDS: Program name '%s'\n",tmp);
			}
		}
		else if (s->typeno == 1) { /* program start */
			if (s->data_length >= sizeof(tmp))
				s->data_length = sizeof(tmp)-1;
			if (s->data_length >= 6) {
				const char *months[16] = {
					"",		"January",	"February",	"March",
					"April",	"May",		"June",		"July",
					"August",	"September",	"October",	"November",
					"December",	"?",		"?",		"?"
				};
				unsigned int H,M;

				H = (unsigned char)(s->data[3]&0x1F)-0x40;
				M = (unsigned char)(s->data[2]&0x7F)-0x40;
				fprintf(stdout,"XDS: UTC Program start: %u:%02u:%s %s %s\n",
					((H+11)%12)+1,M,
					s->data[4]&0x20?"xx":"00",
					H >= 12 ? "PM" : "AM",
					s->data[3]&0x20?"DST":"   ");
				if (s->data[4]&0x10) fprintf(stdout,"    Tape delayed\n");
				if (s->data[5]&0x20) fprintf(stdout,"    Leap day\n");
				fprintf(stdout,"    Month: %s\n",months[s->data[5]&0xF]);
				fprintf(stdout,"    Day: %u\n",s->data[4]&0x1F);
			}
		}
		else if (s->typeno == 2) { /* program length */
			if (s->data_length >= sizeof(tmp))
				s->data_length = sizeof(tmp)-1;
			if (s->data_length >= 6) {
				fprintf(stdout,"XDS: Program length/time:\n");
				fprintf(stdout,"    Length: %u:%02u:00\n",
					((unsigned char)s->data[3])-0x40,
					((unsigned char)s->data[2])-0x40);
				fprintf(stdout,"    Elapsed: %u:%02u:%02u\n",
					((unsigned char)s->data[5])-0x40,
					((unsigned char)s->data[4])-0x40,
					(s->data_length > 6) ? 
						((unsigned char)s->data[6])-0x40 : 0x00);
			}
		}
		else if (s->typeno == 5) { /* program rating (V-chip) */
			if (s->data_length >= sizeof(tmp))
				s->data_length = sizeof(tmp)-1;
			if (s->data_length > 2) {
				fprintf(stdout,"XDS: Program rating (std %02x)\n",s->data[2]);
				if ((s->data[2]&0x58) == 0x48) { /* +0x20 if suggestive dialog */
					fprintf(stdout,"    TPG ");
					switch (s->data[3]&7) {
						case 0:	fprintf(stdout,"None"); break;
						case 1:	fprintf(stdout,"TV-Y"); break;
						case 2:	fprintf(stdout,"TV-Y7"); break;
						case 3:	fprintf(stdout,"TV-G"); break;
						case 4:	fprintf(stdout,"TV-PG"); break;
						case 5:	fprintf(stdout,"TV-14"); break;
						case 6:	fprintf(stdout,"TV-MA"); break;
						case 7:	fprintf(stdout,"None"); break;
					};
					fprintf(stdout,"\n");
				}
				else if ((s->data[2]&0x58) == 0x40) {
					fprintf(stdout,"    MPAA ");
					switch (s->data[3]&7) {
						case 0:	fprintf(stdout,"None"); break;
						case 1:	fprintf(stdout,"G"); break;
						case 2:	fprintf(stdout,"PG"); break;
						case 3:	fprintf(stdout,"PG-13"); break;
						case 4:	fprintf(stdout,"R"); break;
						case 5:	fprintf(stdout,"NC-17"); break;
						case 6:	fprintf(stdout,"X"); break;
						case 7:	fprintf(stdout,"NR"); break;
					};
					fprintf(stdout,"\n");
				}

				if (s->data[2]&0x20)
					fprintf(stdout,"    * May contain sexually suggestive dialogue\n");
				if (s->data[3]&0x08)
					fprintf(stdout,"    * May contain coarse language\n");
				if (s->data[3]&0x10)
					fprintf(stdout,"    * May contain sexual situations\n");
				if (s->data[3]&0x20)
					fprintf(stdout,"    * May contain violent content\n");
			}
		}
		else if (s->typeno == 8) { /* Copy Generation Management System */
			fprintf(stdout,"CGMS: Source is %s. ",s->data[2]&1 ? "analog" : "digital");
			switch ((s->data[2]>>3)&3) {
				case 0: fprintf(stdout,"Unlimited copy. "); break;
				case 2: fprintf(stdout,"Allow copy once. "); break;
				case 3: fprintf(stdout,"Prohbit recording. "); break;
				default:fprintf(stdout,"??"); break;
			};
			fprintf(stdout,"\n");
			fprintf(stdout,"   Macrosivion: ");
			switch ((s->data[3]>>1)&3) {
				case 0: fprintf(stdout,"None."); break;
				case 1: fprintf(stdout,"Pseudo-sync, no colorstripe."); break;
				case 2: fprintf(stdout,"Pseudo-sync with 2-line colorstripe."); break;
				case 3: fprintf(stdout,"Pseudo-sync with 4-line colorstripe."); break;
			};
			fprintf(stdout,"\n");
		}
		else if (s->typeno == 9) { /* program aspect ratio */
			fprintf(stdout,"XDS: Program A/R active_from_top=%u active_from_bottom=%u anamorphic=%d\n",
				s->data[2]-0x40,
				s->data[3]-0x40,
				s->data[4] == 'A' ? 1 : 0);
		}
		else {
			fprintf(stdout,"Unknown class=%u type=%u len=%zu XDS\n",s->classno,s->typeno,s->data_length);
		}
	}
	else {
		fprintf(stdout,"Unknown class=%u type=%u XDS\n",s->classno,s->typeno);
	}
}

static void on_xds_data(uint16_t cc) {
	if (xds_asm != NULL) {
		if (raw) fprintf(stderr,"XDS data %04x\n",cc);
		xds_data_assembly_take_eia608_word(xds_asm,cc);
	}
}

static void render_line21_thr(unsigned char *row,size_t len,unsigned char thr) {
	unsigned int step,f,x,y;
	unsigned char *scan;
	size_t stride;
	uint16_t *buf;

	assert(lock_video_screen() == 0);
	buf = (uint16_t*)(video_surface_SDL->pixels);
	stride = video_surface_SDL->pitch >> 1;
	buf += stride * (video_surface_SDL->h - 4);

	for (y=0;y < 4;y++) {
		f = 0;
		scan = row;
		step = (len << 16UL) / ((size_t)video_surface_SDL->w);
		for (x=0;x < (unsigned int)video_surface_SDL->w;x++) {
			*buf++ = (*scan >= thr) ? rgb16(192,192,192) : rgb16(16,16,16);
			f += step;
			scan += f >> 16;
			f &= 0xFFFF;
		}
	}
	unlock_video_screen();
	if (speedup <= 1 || (cc_framecounter&1023) == 0) SDL_Flip(video_surface_SDL);
}

static void render_line21(unsigned char *row,size_t len) {
	unsigned int step,f,x,y;
	unsigned char *scan;
	size_t stride;
	uint16_t *buf;

	assert(lock_video_screen() == 0);
	buf = (uint16_t*)(video_surface_SDL->pixels);
	stride = video_surface_SDL->pitch >> 1;
	buf += stride * (video_surface_SDL->h - 4);

	for (y=0;y < 4;y++) {
		f = 0;
		scan = row;
		step = (len << 16UL) / ((size_t)video_surface_SDL->w);
		for (x=0;x < (unsigned int)video_surface_SDL->w;x++) {
			*buf++ = rgb16(*scan,*scan,*scan);
			f += step;
			scan += f >> 16;
			f &= 0xFFFF;
		}
	}
	unlock_video_screen();
	if (speedup <= 1 || (cc_framecounter&1023) == 0) SDL_Flip(video_surface_SDL);
}

/* video frame */
static uint16_t*			video_frame=NULL;	/* in RGB16 format */
static size_t				video_width,video_height,video_stride;

static void redraw_entire_screen() {
	unsigned char deint = 0;
	size_t render_stride;
	unsigned int x,y;
	uint16_t *buf;
	size_t stride;

	if (video_height > 288) {
		deint = 1;
		render_stride = video_stride << 1;
	}
	else {
		render_stride = video_stride;
	}

	assert(lock_video_screen() == 0);
	buf = (uint16_t*)(video_surface_SDL->pixels);
	stride = video_surface_SDL->pitch >> 1;

	if (video_frame != NULL && video_width > 0 && video_height > 0 && video_stride > 0) {
		unsigned int sx,sy,ax,ay;

		ax = (video_width << 12) / video_surface_SDL->w;
		if (deint)	ay = (video_height << 11) / video_surface_SDL->h;
		else		ay = (video_height << 12) / video_surface_SDL->h;
		for (y=0,sy=0;y < (unsigned int)video_surface_SDL->h;y++,sy+=ay) {
			for (x=0,sx=0;x < (unsigned int)video_surface_SDL->w;x++,sx+=ax) {
				buf[(y*stride)+x] = video_frame[((sy>>12)*(render_stride>>1))+(sx>>12)];
			}
		}
	}
	else {
		for (y=0;y < (unsigned int)video_surface_SDL->h;y++) {
			for (x=0;x < (unsigned int)video_surface_SDL->w;x++) {
				buf[(y*stride)+x] = rgb16(128,128,128);
			}
		}
	}
	unlock_video_screen();

	redraw_cc(0,0,CC_COLUMNS-1,CC_ROWS-1);
}

static void on_webtv_data(unsigned int word) {
	if (eia608_webtv_take_word(&webtv,word)) {
		fprintf(stderr,"WebTV data: '%s'\n",webtv.assembly);
	}
}

static void dump_decoder() {
	char temp[20],*w,*wf;
	unsigned int x,y;
	CCCHAR *chent;
	int uc;

	printf("\n");
	for (y=0;y < CC_ROWS;y++) {
		chent = eia608_cc->current_page + (y * CC_COLUMNS);
		for (x=0;x < CC_COLUMNS;x++) {
			w=temp;
			wf=temp+sizeof(temp)-1;
			if (eia608_decoder_cchar_is_transparent(chent[x].ch)) {
				printf(" ");
			}
			else {
				uc=eia608_decoder_cchar_to_unicode(chent[x].ch);
				if (uc >= 0) {
					utf8_encode(&w,wf,(uint32_t)uc); *w = 0;
					printf("%s",temp);
				}
				else {
					printf("?");
				}
			}
		}
		printf("\n");
	}
}

static void emit_cc(unsigned int word) {
	unsigned int delay = 1000000/field_rate;
	SDL_Event event;
	int pause=0;
	int which;

	if (speedup >= 10) delay = 0;
	else if (speedup) delay /= 30;
	else if (is_v4l_vbi) delay /= 2;

	/* input I/O */
	do {
		if (SDL_PollEvent(&event)) {
			switch (event.type) {
				case SDL_QUIT:
					DIE = 1;
					break;
				case SDL_KEYDOWN: {
					SDL_KeyboardEvent *key = &event.key;

					if (key->keysym.sym == SDLK_ESCAPE) {
						DIE = 1;
					}
					else if (key->keysym.sym == SDLK_RIGHT) {
						if (key->keysym.mod & KMOD_SHIFT)
							speedup = 10;
						else
							speedup = 1;
					}
					else if (key->keysym.sym == SDLK_SPACE) {
						pause = !pause;
					}
					else if (key->keysym.sym == 'r') {
						fprintf(stderr,"Resetting EIA-608 decoder\n");
						eia608_decoder_analog_line21_decoder_state_init(&ast);
						eia608_decoder_reset(eia608_cc);
					}
					else if (key->keysym.sym == 'd') {
						fprintf(stderr,"Dumping CC screen to console\n");
						dump_decoder();
					}
				}break;
				case SDL_KEYUP: {
					SDL_KeyboardEvent *key = &event.key;

					if (key->keysym.sym == SDLK_ESCAPE) {
					}
					else if (key->keysym.sym == SDLK_RIGHT) {
						speedup = 0;
					}
				}break;
			}
		}
	} while (pause && !DIE);

	if (raw > 2)
		fprintf(stderr,"CC=%04x\n",word);

	/* take ONLY the data the demux gives us */
	which = eia608_demux_take_word(&eia608_dmux,word,even?1:0);
	if (dmuxdbg) fprintf(stderr,"Demux in mode %u ret %u, mask 0x%x, wanted=%u [%04x]\n",
		eia608_dmux.current_mode,which,eia608_dmux.want_modes,demux_match,word);

	if (which == EIA608_DM_XDS && do_xds) {
		/* it's ok */
	}
	else if (which == EIA608_DM_TEXT2 && do_webtv) {
		on_webtv_data(word);
	}
	else if (which != demux_match || which < 0) {
		if (verbose) fprintf(stderr,"Demux rejected %04x\n",word);
		usleep(delay);
		return;
	}

	if (raw > 1)
		fprintf(stderr,"CC=%04x\n",word);
	else if (raw && (word&0x7F7F) != 0)
		fprintf(stderr,"CC=%04x\n",word);

	if (rawtext) {
		if ((word&0x6000) != 0) {
			char tmp[48],*w=tmp,*wf=tmp+sizeof(tmp)-1;
			int uc;

			if ((word>>8)&0x7F) {
				uc = eia608_decoder_cchar_to_unicode((word>>8)&0x7F);
				if (uc >= 0) utf8_encode(&w,wf,(uint32_t)uc);
				else fprintf(stderr,"raw text warning: no equivalent char\n");
			}

			if (word&0x7F) {
				uc = eia608_decoder_cchar_to_unicode(word&0x7F);
				if (uc >= 0) utf8_encode(&w,wf,(uint32_t)uc);
				else fprintf(stderr,"raw text warning: no equivalent char\n");
			}

			*w = 0;

			fprintf(stdout,"%s",tmp);
			emitted_rawtext++;
			fflush(stdout);
		}
		else if (emitted_rawtext != 0) {
			if ((word&0x7F7F) != 0) {
				fprintf(stdout,"\n");
				emitted_rawtext = 0;
			}
		}
	}

	if (which == EIA608_DM_XDS) {
		on_xds_data(word);
	}
	else if (which == EIA608_DM_TEXT2 && do_webtv) {
		/* it's ok */
	}
	else {
		if (eia608_decoder_is_nonspace_data(eia608_cc,word))
			cc_counter++;
		else
			cc_space++;

		if (cc_counter == 0) delay = 1;

		eia608_decoder_step_field(eia608_cc);
		eia608_decoder_take_word_with_delay(eia608_cc,word);
	}

	if (eia608_cc->updated) {
		eia608_cc->updated=0;
		if (!show_video) {
			if (speedup <= 1 || (cc_framecounter&1023) == 0) SDL_Flip(video_surface_SDL);
		}
	}

	/* wait for 1/30th of a second */
	usleep(delay);

	/* second field */
	eia608_decoder_step_field(eia608_cc);
	if (eia608_cc->updated) {
		eia608_cc->updated=0;
		if (!show_video) {
			if (speedup <= 1 || (cc_framecounter&1023) == 0) SDL_Flip(video_surface_SDL);
		}
	}

	/* wait for 1/30th of a second */
	usleep(delay);

	/* counters */
	if (cc_space >= 1200) {
		cc_counter = 0;
		cc_space = 0;
	}

	cc_framecounter++;
}

struct v4l2_format vidfmt;
struct v4l2_requestbuffers vidbufs;

void v4l_video_take(unsigned char *src) {
	if (src == NULL) src = (unsigned char*)video_frame;

	if (vidfmt.fmt.pix.pixelformat == V4L2_PIX_FMT_BGR24) {
		unsigned char *i;
		unsigned int x,y;
		uint16_t *o;

		for (y=0;y < video_height;y++) {
			i = src+(y*video_stride);
			o = (uint16_t*)((unsigned char*)video_frame+(y*video_stride));
			for (x=0;x < video_width;x++) {
				unsigned char r,g,b;

				b = i[0] >> 3;
				g = i[1] >> 2;
				r = i[2] >> 3;
				*o++ = (uint16_t)((r << 11) + (g << 5) + b);
				i += 3;
			}
		}
	}
	else if (vidfmt.fmt.pix.pixelformat == V4L2_PIX_FMT_UYVY) {
		unsigned char *i;
		unsigned int x,y;
		uint16_t *o;

		for (y=0;y < video_height;y++) {
			i = src+(y*video_stride);
			o = (uint16_t*)((unsigned char*)video_frame+(y*video_stride));
			for (x=0;x < video_width;x += 2) {
				int Y,U,V,r,g,b;

				U = i[0] - 128;
				V = i[2] - 128;

				Y = i[1] - 16;
				r = ((Y * 0x1F) / (235-16)) + ((V*21)/128); if (r < 0) r = 0; else if (r > 0x1F) r = 0x1F;
				g = ((Y * 0x3F) / (235-16)) - ((U*10)/128) - ((V*22)/128); if (g < 0) g = 0; else if (g > 0x3F) g = 0x3F;
				b = ((Y * 0x1F) / (235-16)) + ((U*26)/128); if (b < 0) b = 0; else if (b > 0x1F) b = 0x1F;
				*o++ = (uint16_t)((r << 11) + (g << 5) + b);

				Y = i[3] - 16;
				r = ((Y * 0x1F) / (235-16)) + ((V*21)/128); if (r < 0) r = 0; else if (r > 0x1F) r = 0x1F;
				g = ((Y * 0x3F) / (235-16)) - ((U*10)/128) - ((V*22)/128); if (g < 0) g = 0; else if (g > 0x3F) g = 0x3F;
				b = ((Y * 0x1F) / (235-16)) + ((U*26)/128); if (b < 0) b = 0; else if (b > 0x1F) b = 0x1F;
				*o++ = (uint16_t)((r << 11) + (g << 5) + b);

				i += 4;
			}
		}
	}
	else {
		fprintf(stderr,"WARNING: Unknown format\n");
	}
}

int main(int argc,char **argv) {
	unsigned long long file_offset = 0;
	unsigned long long SCR = 0ULL;
	sliding_window_v4 *wnd = NULL;
	unsigned char tgt_field = 0;
	unsigned int cc_field = 0;
	char *path = NULL;
	int fd,rd,i,sw=0;
	int eof;

	for (i=1;i < argc;) {
		char *p = argv[i++];

		if (*p == '-') {
			do { p++; } while (*p == '-');

			if (!strcmp(p,"h") || !strcmp(p,"help")) {
				help();
				return 1;
			}
			else if (!strcmp(p,"even")) {
				tgt_field = 1;
				even = 1;
			}
			else if (!strcmp(p,"eia608-thr")) {
				eia608_debug_thr = 1;
			}
			else if (!strcmp(p,"video")) {
				show_video = 1;
			}
			else if (!strcmp(p,"no-video")) {
				show_video = 0;
			}
			else if (!strcmp(p,"eia608-state")) {
				eia608_debug_state = 1;
			}
			else if (!strcmp(p,"line21")) {
				is_line21 = 756;
			}
			else if (!strncmp(p,"line21=",7)) {
				is_line21 = atoi(p+7);
			}
			else if (!strcmp(p,"jumbled-backwards")) {
				mpeg_jumbled_CC1_CC2_backwards = 1;
			}
			else if (!strcmp(p,"v4l-vbi")) {
				is_v4l_vbi = 720;
			}
			else if (!strncmp(p,"v4l-vbi=",8)) {
				is_v4l_vbi = atoi(p+8);
			}
			else if (!strcmp(p,"mpg-dvd")) {
				mpeg_mode = MM_DVD;
			}
			else if (!strcmp(p,"p60")) {
				field_rate = 120;
			}
			else if (!strcmp(p,"mpg-atsc")) {
				mpeg_mode = MM_ATSC;
			}
			else if (!strncmp(p,"line21on=",9)) {
				line21_line = atoi(p+9);
				if (line21_line < 10 || line21_line > 32) line21_line = 21;
			}
			else if (!strcmp(p,"cc2")) {
				cc2 = 1;
			}
			else if (!strcmp(p,"scc")) {
				is_scc = 1;
			}
			else if (!strcmp(p,"v")) {
				verbose = 1;
			}
			else if (!strcmp(p,"fuzz")) {
				fuzz = 1;
			}
			else if (!strcmp(p,"webtv")) {
				do_webtv = 1;
			}
			else if (!strcmp(p,"text")) {
				do_text = 1;
			}
			else if (!strcmp(p,"raw")) {
				raw++;
			}
			else if (!strcmp(p,"xds")) {
				do_xds = 1;
			}
			else if (!strcmp(p,"dmuxdbg")) {
				dmuxdbg = 1;
			}
			else if (!strcmp(p,"rawtext")) {
				rawtext = 1;
			}
			else if (!strcmp(p,"jumbled-cc1-cc2")) {
				mpeg_jumbled_CC1_CC2 = 1;
			}
			else if (!strcmp(p,"no-jumbled-cc1-cc2")) {
				mpeg_jumbled_CC1_CC2 = 0;
			}
			else if (!strcmp(p,"id")) {
				rawinput = 1;
			}
			else {
				fprintf(stderr,"Unknown switch '%s'\n",p);
				help();
				return 1;
			}
		}
		else {
			if (sw == 0) {
				path = p;
			}
			else {
				fprintf(stderr,"Ignoring argv '%s'\n",p);
			}

			sw++;
		}
	}

	if (is_line21 > 3000) {
		fprintf(stderr,"Scanline too large\n");
		return 1;
	}

	if (path == NULL) {
		help();
		return 1;
	}

	if (!fuzz) {
		fd = open(path,O_RDONLY);
		if (fd < 0) {
			fprintf(stderr,"Cannot open file '%s'\n",path);
			return 1;
		}

		if (is_v4l_vbi <= 0 && !is_line21 && !is_scc && !rawinput) {
			struct stat st;

			if (fstat(fd,&st) == 0) {
				if (S_ISCHR(st.st_mode)) {
					if (!strncmp(path,"/dev/vbi",8)) {
						is_v4l_vbi = 720;
					}
				}
			}
			else {
				fprintf(stderr,"WARNING: Cannot stat the file/device I just opened\n");
			}
		}
	}
	else {
		fd = -1;
	}

	if (SDL_Init(SDL_INIT_EVERYTHING) < 0) {
		fprintf(stderr,"SDL_Init() failed\n");
		return -1;
	}

	/* "The screen is 15 rows high and 32 columns wide... within the
	 *  title safe area... 195 lines high"
	 *
	 *  that tells me the font used supposedly is 195/15 = 13 pixels high */
	video_surface_SDL = SDL_SetVideoMode((CC_COLUMNS+4+4)*RENDERED_FONT_WIDTH,(CC_ROWS+2+2)*CC_FONT_HEIGHT,16,0);
	if (video_surface_SDL == NULL) {
		fprintf(stderr,"SDL_SetVideoMode() failed\n");
		return -1;
	}

	SDL_WM_SetCaption("Castus EIA-608 Closed Caption decoder emulation",NULL);

	/* dammit SDL */
	signal(SIGINT,sigma);
	signal(SIGQUIT,sigma);
	signal(SIGTERM,sigma);

	if ((wnd = sliding_window_v4_create_buffer(1024*1024)) == NULL) {
		fprintf(stderr,"Cannot create sliding window\n");
		return 1;
	}
	if ((eia608_cc = eia608_decoder_create()) == NULL) {
		fprintf(stderr,"Cannot alloc CC decoder\n");
		return 1;
	}

	if (do_webtv && even) {
		fprintf(stderr,"Ignoring -webtv flag because you specified that you're listening to the even Line 21 field\n");
		do_webtv = 0;
	}

	/* tell the demux which channel we want */
	if (do_text)	eia608_demux_init(&eia608_dmux,demux_match = (EIA608_DM_TEXT1 + cc2 + (even ? 2 : 0)));
	else		eia608_demux_init(&eia608_dmux,demux_match = (EIA608_DM_CC1 + cc2 + (even ? 2 : 0)));
	if (do_xds)	eia608_demux_add_want(&eia608_dmux,EIA608_DM_XDS);
	if (do_webtv)	eia608_demux_add_want(&eia608_dmux,EIA608_DM_TEXT2); /* WARNING! we're assuming the user is giving us CC1/CC2 */

	/* debugging function */
	if (dmuxdbg) eia608_dmux.want_modes = ~0;

	eia608_webtv_reader_init(&webtv);
	eia608_cc->on_update_screen = on_cc_update_screen;
	eia608_cc->on_rollup_screen = on_cc_rollup_screen;
	eia608_cc->debug_show_chars = (!raw && !rawtext) ? 1 : 0;
	eia608_cc->debug_rollup_interrupt = verbose;
	if (do_xds) {
		if ((xds_asm = xds_data_assembly_create()) == NULL) {
			fprintf(stderr,"Cannot create XDS asm\n");
			return 1;
		}
		xds_asm->on_xds_packet = on_xds_packet;
	}

	if (is_scc) {
		if ((scc_reader=scc_eia608_reader_create()) == NULL) {
			fprintf(stderr,"Cannot alloc SCC reader\n");
			return 1;
		}
	}

	redraw_entire_screen();
	SDL_Flip(video_surface_SDL);

	if (fuzz) {
		show_video = 0;
		srand(time(NULL));
		eia608_decoder_roll_up(eia608_cc,8,0xFF);

		while (!DIE) {
			signed long long cc = (signed long long)((unsigned int)rand() & 0xFFFF);
			emit_cc((uint16_t)cc);
		}
	}
	else if (is_v4l_vbi > 0) {
		struct v4l2_mmap_tracking *vidmap = NULL;
		struct v4l2_buffer *vidqueue = NULL;
		int v4l_stream_queue_index = 0;
		struct v4l2_streamparm vidcprm;
		struct v4l2_capability vidcap;
		struct v4l2_capability cap;
		struct v4l2_format capfmt;
		size_t vidqueue_size = 0;
		int v4l_video_stream = 0;
		int v4l_video_index = -1;
		int v4l_video_fd = -1;
		size_t vbi_size;

		/* try to auto-detect corresponding video device */
		if (show_video) {
			{
				const char *x = strrchr(path,'/');
				if (x != NULL) {
					x++;
					if (!strncmp(x,"vbi",3) && isdigit(x[3])) {
						x += 3;
						v4l_video_index = atoi(x);
					}
				}
			}

			fprintf(stderr,"Guessed video device index %d\n",v4l_video_index);

			if (v4l_video_index >= 0) {
				char viddev[64];
				sprintf(viddev,"/dev/video%d",v4l_video_index);
				v4l_video_fd = open(viddev,O_RDWR|O_CLOEXEC);
				if (v4l_video_fd < 0) {
					fprintf(stderr,"Failed to open video dev %s, %s\n",viddev,strerror(errno));
					v4l_video_index = -1;
				}
			}

			if (v4l_video_fd >= 0) {
				v4l2_std_id std_x = V4L2_STD_NTSC_M;
				char tmp[6];

				memset(&vidcap,0,sizeof(vidcap));
				memset(&vidfmt,0,sizeof(vidfmt));
				memset(&vidcprm,0,sizeof(vidcprm));
				if (ioctl(v4l_video_fd,VIDIOC_QUERYCAP,&vidcap))
					fprintf(stderr,"Failed to query video cap, %s\n",strerror(errno));
				vidfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
				if (ioctl(v4l_video_fd,VIDIOC_G_FMT,&vidfmt))
					fprintf(stderr,"Failed to query current format, %s\n",strerror(errno));
				if (ioctl(v4l_video_fd,VIDIOC_S_STD,&std_x))
					fprintf(stderr,"Failed to set video standard, %s\n",strerror(errno));

				if (vidcap.capabilities & V4L2_CAP_STREAMING)
					v4l_video_stream = 1;
				else if (vidcap.capabilities & V4L2_CAP_READWRITE)
					v4l_video_stream = 0;
				else
					fprintf(stderr,"WARNING: Video capture card does not support any method I recognize\n");

				fprintf(stderr,"Reminder: To configure capture card inputs, etc. please use an external tool like MPlayer\n");
				memcpy(tmp,&vidfmt.fmt.pix.pixelformat,sizeof(vidfmt.fmt.pix.pixelformat));
				tmp[sizeof(vidfmt.fmt.pix.pixelformat)] = 0;
				fprintf(stderr,"Current format: %u x %u pixfmt='%s' field=%u bpl=%u\n",
					vidfmt.fmt.pix.width,
					vidfmt.fmt.pix.height,
					tmp,
					vidfmt.fmt.pix.field,
					vidfmt.fmt.pix.bytesperline);

				vidfmt.fmt.pix.width = 720;
				vidfmt.fmt.pix.height = 480;
				vidfmt.fmt.pix.bytesperline = 0;
				ioctl(v4l_video_fd,VIDIOC_S_FMT,&vidfmt);
				ioctl(v4l_video_fd,VIDIOC_G_FMT,&vidfmt);
				vidfmt.fmt.pix.bytesperline = 0;
				/* NTS: Try to use UYVY (widely supported by many capture cards) whereas BGR24 can get glitchy */
				vidfmt.fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;//BGR24;
				ioctl(v4l_video_fd,VIDIOC_S_FMT,&vidfmt);
				ioctl(v4l_video_fd,VIDIOC_G_FMT,&vidfmt);
				vidfmt.fmt.pix.bytesperline = 0;
				vidfmt.fmt.pix.field = V4L2_FIELD_ANY;
				ioctl(v4l_video_fd,VIDIOC_S_FMT,&vidfmt);
				ioctl(v4l_video_fd,VIDIOC_G_FMT,&vidfmt);

				fprintf(stderr,"Current format: %u x %u pixfmt='%s' field=%u bpl=%u\n",
					vidfmt.fmt.pix.width,
					vidfmt.fmt.pix.height,
					tmp,
					vidfmt.fmt.pix.field,
					vidfmt.fmt.pix.bytesperline);

				memset(&vidcprm,0,sizeof(vidcprm));
				vidcprm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
				if (ioctl(v4l_video_fd,VIDIOC_G_PARM,&vidcprm))
					fprintf(stderr,"Unable to read capture params, %s\n",strerror(errno));

				fprintf(stderr,"Current cap params: rbufs=%u time/frame=%u/%u\n",
					vidcprm.parm.capture.readbuffers,
					vidcprm.parm.capture.timeperframe.numerator,
					vidcprm.parm.capture.timeperframe.denominator);

				vidcprm.parm.capture.readbuffers = 8;
				vidcprm.parm.capture.extendedmode = 0;
				if (vidcprm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) {
					vidcprm.parm.capture.timeperframe.numerator = 1001;
					vidcprm.parm.capture.timeperframe.denominator = 30000;
					vidcprm.parm.capture.capturemode |= V4L2_CAP_TIMEPERFRAME;
				}
				vidcprm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
				if (ioctl(v4l_video_fd,VIDIOC_S_PARM,&vidcprm))
					fprintf(stderr,"Failed to set stream params, %s\n",strerror(errno));
				vidcprm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
				ioctl(v4l_video_fd,VIDIOC_G_PARM,&vidcprm);

				fprintf(stderr,"Current cap params: rbufs=%u time/frame=%u/%u\n",
					vidcprm.parm.capture.readbuffers,
					vidcprm.parm.capture.timeperframe.numerator,
					vidcprm.parm.capture.timeperframe.denominator);

				if (v4l_video_stream) {
					memset(&vidbufs,0,sizeof(vidbufs));
					vidbufs.count = 8;
					vidbufs.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
					vidbufs.memory = V4L2_MEMORY_MMAP;
					if (ioctl(v4l_video_fd,VIDIOC_REQBUFS,&vidbufs)) {
						fprintf(stderr,"Unable to request buffers, %s\n",strerror(errno));
						v4l_video_stream = 0;
					}

					vidqueue_size = vidbufs.count;
					assert(vidqueue_size != 0);
					vidqueue = (struct v4l2_buffer*)malloc(sizeof(struct v4l2_buffer) * vidqueue_size);
					assert(vidqueue != NULL);
					memset(vidqueue,0,sizeof(struct v4l2_buffer) * vidqueue_size);

					vidmap = (struct v4l2_mmap_tracking*)malloc(sizeof(struct v4l2_mmap_tracking) * vidqueue_size);
					assert(vidmap != NULL);
					memset(vidmap,0,sizeof(struct v4l2_mmap_tracking) * vidqueue_size);

					struct v4l2_buffer *qbuf;
					int i;

					for (i=0;(size_t)i < vidqueue_size;i++) {
						qbuf = vidqueue + i;
						qbuf->index = i;
						qbuf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
						qbuf->memory = V4L2_MEMORY_MMAP;
						if (ioctl(v4l_video_fd,VIDIOC_QUERYBUF,qbuf)) {
							fprintf(stderr,"WARNING: unable to query buf, %s\n",strerror(errno));
							continue;
						}

						vidmap[i].size = qbuf->length;
						vidmap[i].mmap = (unsigned char*)mmap(NULL,vidmap[i].size,PROT_READ|PROT_WRITE,MAP_SHARED,v4l_video_fd,qbuf->m.offset);
						if (vidmap[i].mmap == MAP_FAILED) {
							fprintf(stderr,"WARNING: failed to mmap QBUF, %s\n",strerror(errno));
							vidmap[i].mmap = NULL;
						}

						if (ioctl(v4l_video_fd,VIDIOC_QBUF,qbuf))
							fprintf(stderr,"WARNING: unable to QBUF, %s\n",strerror(errno));
					}

					int x = V4L2_BUF_TYPE_VIDEO_CAPTURE;
					if (ioctl(v4l_video_fd,VIDIOC_STREAMON,&x))
						fprintf(stderr,"WARNING: Unable to start streaming, %s\n",strerror(errno));
				}

				{
					int x = fcntl(v4l_video_fd,F_GETFL);
					fcntl(v4l_video_fd,F_SETFL,x|O_NONBLOCK);
				}

				video_width = vidfmt.fmt.pix.width;
				video_height = vidfmt.fmt.pix.height;
				video_stride = vidfmt.fmt.pix.bytesperline;
				if (video_stride < (video_width*2)) video_stride = video_width*2;
				if (video_stride > 0 && video_height > 0)
					video_frame = (uint16_t*)malloc(video_height * video_stride);
			}
		}

		eia608_decoder_analog_line21_decoder_state_init(&ast);
		if (ioctl(fd,VIDIOC_QUERYCAP,&cap)) {
			fprintf(stderr,"Failed to query caps, %s\n",strerror(errno));
			return 1;
		}

		fprintf(stderr,"caps: 0x%08x\n",cap.capabilities);
		fprintf(stderr,"devcaps: 0x%08x\n",cap.device_caps);

		if (!(cap.capabilities & V4L2_CAP_VBI_CAPTURE)) {
			fprintf(stderr,"This device doesn't do VBI capture\n");
			return 1;
		}

		capfmt.type = V4L2_BUF_TYPE_VBI_CAPTURE;
		if (ioctl(fd,VIDIOC_G_FMT,&capfmt)) {
			fprintf(stderr,"Failed to query format\n");
			return 1;
		}

		fprintf(stderr,"Initial VBI format:\n");
		fprintf(stderr,"    sampling_rate: %uHz\n",capfmt.fmt.vbi.sampling_rate);
		fprintf(stderr,"    offset:        %u\n",capfmt.fmt.vbi.offset);
		fprintf(stderr,"    samples/line:  %u\n",capfmt.fmt.vbi.samples_per_line);
		fprintf(stderr,"    sample_fmt:    0x%x\n",capfmt.fmt.vbi.sample_format);
		fprintf(stderr,"    start:         %u, %u\n",capfmt.fmt.vbi.start[0],capfmt.fmt.vbi.start[1]);
		fprintf(stderr,"    count:         %u, %u\n",capfmt.fmt.vbi.count[0],capfmt.fmt.vbi.count[1]);
		fprintf(stderr,"    flags:         0x%x\n",capfmt.fmt.vbi.flags);

		capfmt.fmt.vbi.start[0] = 21;
		capfmt.fmt.vbi.count[0] = 1;
		capfmt.fmt.vbi.start[0] = 284;
		capfmt.fmt.vbi.count[0] = 1;
		capfmt.fmt.vbi.sample_format = V4L2_PIX_FMT_GREY;
		if (ioctl(fd,VIDIOC_S_FMT,&capfmt))
			fprintf(stderr,"Warning, failed to set format\n");

		if (ioctl(fd,VIDIOC_G_FMT,&capfmt)) {
			fprintf(stderr,"Failed to query format\n");
			return 1;
		}

		fprintf(stderr,"Final VBI format:\n");
		fprintf(stderr,"    sampling_rate: %uHz\n",capfmt.fmt.vbi.sampling_rate);
		fprintf(stderr,"    offset:        %u\n",capfmt.fmt.vbi.offset);
		fprintf(stderr,"    samples/line:  %u\n",capfmt.fmt.vbi.samples_per_line);
		fprintf(stderr,"    sample_fmt:    0x%x\n",capfmt.fmt.vbi.sample_format);
		fprintf(stderr,"    start:         %u, %u\n",capfmt.fmt.vbi.start[0],capfmt.fmt.vbi.start[1]);
		fprintf(stderr,"    count:         %u, %u\n",capfmt.fmt.vbi.count[0],capfmt.fmt.vbi.count[1]);
		fprintf(stderr,"    flags:         0x%x\n",capfmt.fmt.vbi.flags);

		if (capfmt.fmt.vbi.sample_format != V4L2_PIX_FMT_GREY) {
			fprintf(stderr,"Unsupported sample format\n");
			return 1;
		}

		fprintf(stderr,"I will read Line 21 from VBI line %u\n",line21_line);
		if (capfmt.fmt.vbi.start[0] > line21_line) {
			fprintf(stderr,"Capture card won't return line 21 (start to high)\n");
			return 1;
		}
		if ((capfmt.fmt.vbi.start[0]+capfmt.fmt.vbi.count[0]-1) < (unsigned int)line21_line) {
			fprintf(stderr,"Capture card won't return line 21 (not enough coverage)\n");
		}
		if (capfmt.fmt.vbi.samples_per_line < 128)
			fprintf(stderr,"Warning, unusually low sample count for VBI\n");

		vbi_size = capfmt.fmt.vbi.samples_per_line * (capfmt.fmt.vbi.count[0] + capfmt.fmt.vbi.count[1]);
		is_v4l_vbi = capfmt.fmt.vbi.samples_per_line;

		{
			/* SIGH unfortunately some of my capture cards report 2048 samples/line but then fill in only the
			 * first 1560 or so. */
			int rsa = (int)(capfmt.fmt.vbi.sampling_rate / 15750); /* sample rate vs. 15.750KHz */
			rsa -= capfmt.fmt.vbi.offset;
			fprintf(stderr,"According to sample rate there should be %d per line\n",rsa);
			if (is_v4l_vbi > rsa && rsa > 800 && rsa < (int)capfmt.fmt.vbi.samples_per_line) is_v4l_vbi = rsa;
		}

		{
			int x = fcntl(fd,F_GETFL);
			fcntl(fd,F_SETFL,x|O_NONBLOCK);
		}

		do {
			if (DIE) break;

			assert((wnd->buffer+(vbi_size*8)) <= wnd->fence);
			assert(sliding_window_v4_is_sane(wnd));
			sliding_window_v4_empty(wnd);
			rd = read(fd,wnd->data,vbi_size*8);
			if (rd < 0) rd = 0;
			wnd->end = wnd->data + rd;
			assert(sliding_window_v4_is_sane(wnd));

			while (sliding_window_v4_data_available(wnd) >= (size_t)vbi_size) {
				unsigned char *line21;
				signed long long cc;
				int sofs;

				if (v4l_video_fd >= 0 && video_frame != NULL) {
					int rd;

					if (v4l_video_stream) {
						struct v4l2_buffer *qbuf;

						qbuf = vidqueue + v4l_stream_queue_index;
						memset(qbuf,0,sizeof(*qbuf));
						qbuf->index = v4l_stream_queue_index;
						qbuf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
						qbuf->memory = V4L2_MEMORY_MMAP;
						if (ioctl(v4l_video_fd,VIDIOC_QUERYBUF,qbuf))
							fprintf(stderr,"WARNING: Failed to query buf %s\n",strerror(errno));

						if (qbuf->flags & V4L2_BUF_FLAG_DONE) {
							unsigned char *src;

							src = vidmap[v4l_stream_queue_index].mmap;
							if (src != NULL) v4l_video_take(src);

							if (ioctl(v4l_video_fd,VIDIOC_DQBUF,qbuf))
								fprintf(stderr,"WARNING: Failed to dq buf %s\n",strerror(errno));

							ioctl(v4l_video_fd,VIDIOC_QUERYBUF,qbuf);

							qbuf->index = v4l_stream_queue_index;
							qbuf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
							qbuf->memory = V4L2_MEMORY_MMAP;
							if (ioctl(v4l_video_fd,VIDIOC_QBUF,qbuf))
								fprintf(stderr,"WARNING: Failed to queue buf %s\n",strerror(errno));

							if ((size_t)(++v4l_stream_queue_index) >= vidqueue_size)
								v4l_stream_queue_index = 0;
						}
					}
					else {
						rd = read(v4l_video_fd,video_frame,video_stride * video_height);
						if (rd > 0) {
							if (rd < (int)(vidfmt.fmt.pix.bytesperline*video_height))
								fprintf(stderr,"WARNING: Not enough data rd=%d/%d\n",
									(int)rd,(int)(vidfmt.fmt.pix.bytesperline*video_height));

							v4l_video_take(NULL);
						}
					}
				}

				if (show_video) redraw_entire_screen();

				/* given the range provided by the card, locate line 21 */
				line21 = wnd->data;
				if (even) /* even (second) field line 21. NTS: Second field counts from line 263 */
					sofs = (int)(line21_line + 263 - capfmt.fmt.vbi.start[1]) + capfmt.fmt.vbi.count[0];
				else /* odd (first) field line 21 */
					sofs = (int)(line21_line - capfmt.fmt.vbi.start[0]);

				if (sofs >= 0 && sofs < ((int)(capfmt.fmt.vbi.count[0] + capfmt.fmt.vbi.count[1]))) {
					line21 += (size_t)sofs * capfmt.fmt.vbi.samples_per_line;
					assert(line21 >= wnd->data && (line21+capfmt.fmt.vbi.samples_per_line) <= wnd->end);
					render_line21(line21,(size_t)is_v4l_vbi);
					cc = eia608_decoder_parse_analog_line21(line21,(size_t)is_v4l_vbi,&ast);
					if (cc >= 0) emit_cc((uint16_t)cc);
					else emit_cc(0x8080);
				}
				else {
					fprintf(stderr,"Warning: target scanline %d out of range\n",line21_line);
					emit_cc(0x8080);
				}

				if (show_video) SDL_Flip(video_surface_SDL);

				wnd->data += (size_t)vbi_size;
				if (DIE) break;
			}
		} while (!DIE);

		if (v4l_video_stream) {
			int x = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			if (ioctl(v4l_video_fd,VIDIOC_STREAMOFF,&x))
				fprintf(stderr,"WARNING: Unable to stop streaming, %s\n",strerror(errno));
		}

		if (vidmap) {
			int i;

			for (i=0;i < (int)vidqueue_size;i++) {
				ioctl(v4l_video_fd,VIDIOC_DQBUF,vidqueue+i);
				if (vidmap[i].mmap != NULL) {
					munmap(vidmap[i].mmap,vidmap[i].size);
					vidmap[i].mmap = NULL;
				}
			}

			free(vidmap);
			vidmap = NULL;
		}
		if (vidqueue) free(vidqueue);
		vidqueue = NULL;
		if (video_frame) free(video_frame);
		video_frame = NULL;
		if (v4l_video_fd >= 0) {
			close(v4l_video_fd);
			v4l_video_fd = -1;
		}
	}
	else if (is_line21) {
		unsigned long long fc=0;

		show_video = 0;

		eia608_decoder_analog_line21_decoder_state_init(&ast);
		do {
			if (DIE) break;

			assert(sliding_window_v4_is_sane(wnd));
			file_offset += (unsigned long long)sliding_window_v4_lazy_flush(wnd);
			eof = (sliding_window_v4_can_write(wnd) > 0 && sliding_window_v4_refill_from_fd(wnd,fd,0) <= 0);

			assert(sliding_window_v4_is_sane(wnd));
			while (sliding_window_v4_data_available(wnd) >= (size_t)is_line21) {
				signed long long cc;

				if (eia608_debug_state) eia608_decoder_analog_line21_decoder_state_dump(stderr,&ast);
				if (ast.last_thr >= 8 && eia608_debug_thr) render_line21_thr(wnd->data,(size_t)is_line21,ast.last_thr);
				else render_line21(wnd->data,(size_t)is_line21);
				cc = eia608_decoder_parse_analog_line21(wnd->data,(size_t)is_line21,&ast);
				wnd->data += (size_t)is_line21;
				if (cc >= 0) {
					emit_cc((uint16_t)cc);
				}
				else {
					if (raw) fprintf(stderr,"No data (ent %llu)\n",fc);
					emit_cc(0x8080);
				}

				fc++;
				if (DIE) break;
			}

			file_offset += (unsigned long long)sliding_window_v4_lazy_flush(wnd);
			eof = (sliding_window_v4_can_write(wnd) > 0 && sliding_window_v4_refill_from_fd(wnd,fd,0) <= 0);
		} while (sliding_window_v4_data_available(wnd) >= (size_t)is_line21 || !eof);
	}
	else if (is_scc) {
		show_video = 0;

		do {
			if (DIE) break;

			assert(sliding_window_v4_is_sane(wnd));
			file_offset += (unsigned long long)sliding_window_v4_lazy_flush(wnd);
			eof = (sliding_window_v4_can_write(wnd) > 0 && sliding_window_v4_refill_from_fd(wnd,fd,0) <= 0);

			assert(sliding_window_v4_is_sane(wnd));
			while (sliding_window_v4_data_available(wnd) >= (eof ? 1 : 4096)) {
				signed long c = scc_eia608_reader_get_word(scc_reader,wnd,eof);
				if (c >= 0) {
					while (cc_framecounter < scc_reader->current_frame && !DIE) emit_cc(0x8080);
					emit_cc((uint16_t)c);
				}
				else break;
				if (DIE) break;
			}

			file_offset += (unsigned long long)sliding_window_v4_lazy_flush(wnd);
			eof = (sliding_window_v4_can_write(wnd) > 0 && sliding_window_v4_refill_from_fd(wnd,fd,0) <= 0);
		} while (sliding_window_v4_data_available(wnd) >= 2 || !eof);
	}
	else if (rawinput) {
		show_video = 0;

		do {
			if (DIE) break;

			assert(sliding_window_v4_is_sane(wnd));
			file_offset += (unsigned long long)sliding_window_v4_lazy_flush(wnd);
			eof = (sliding_window_v4_can_write(wnd) > 0 && sliding_window_v4_refill_from_fd(wnd,fd,0) <= 0);

			assert(sliding_window_v4_is_sane(wnd));
			while (sliding_window_v4_data_available(wnd) >= 2) {
				emit_cc(__be_u16(wnd->data));
				wnd->data += 2;
				if (DIE) break;
			}

			file_offset += (unsigned long long)sliding_window_v4_lazy_flush(wnd);
			eof = (sliding_window_v4_can_write(wnd) > 0 && sliding_window_v4_refill_from_fd(wnd,fd,0) <= 0);
		} while (sliding_window_v4_data_available(wnd) >= 2 || !eof);
	}
	else {
		fprintf(stderr,"Unsupported mode\n");
	}

	video_surface_SDL=NULL;
	SDL_Quit();

	scc_reader = scc_eia608_reader_destroy(scc_reader);
	eia608_cc = eia608_decoder_destroy(eia608_cc);
	xds_asm = xds_data_assembly_destroy(xds_asm);
	wnd = sliding_window_v4_destroy(wnd);
	if (fd >= 0) close(fd);
	return 0;
}

