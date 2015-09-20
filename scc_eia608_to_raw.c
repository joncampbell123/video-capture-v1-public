#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

#include "scc-reader.h"
#include "eia608-decoder.h"

static int no_parity = 0;
static int dump_to_stdout = 0;
static int infd = -1,outfd = -1;
static const char *infile = NULL;
static const char *outfile = NULL;
static unsigned long long cc_framecounter=0;

static void help() {
	fprintf(stderr,"scc_eia608_to_raw.c -i <SCC file> -o <raw dump of 16-bit CC words>\n");
	fprintf(stderr,"  -d   also decode and dump text to STDOUT\n");
	fprintf(stderr,"  -np  strip parity bit\n");
}

static int parse_argv(int argc,char **argv) {
	char *a;
	int i;

	if (argc < 2) {
		help();
		return 1;
	}

	for (i=1;i < argc;) {
		a = argv[i++];

		if (*a == '-') {
			do { a++; } while (*a == '-');

			if (!strcmp(a,"i")) {
				infile = argv[i++];
			}
			else if (!strcmp(a,"o")) {
				outfile = argv[i++];
			}
			else if (!strcmp(a,"np")) {
				no_parity = 1;
			}
			else if (!strcmp(a,"d")) {
				dump_to_stdout = 1;
			}
			else {
				help();
				return 1;
			}
		}
		else {
			help();
			return 1;
		}
	}

	return 0;
}

int main(int argc,char **argv) {
	unsigned long long file_offset = 0;
	scc_eia608_reader *scc_reader;
	sliding_window_v4 *wnd = NULL;
	unsigned char tmp[2];
	char eof=0;

	if (parse_argv(argc,argv))
		return 1;

	if ((infd=open(infile,O_RDONLY)) < 0) {
		fprintf(stderr,"Cannot open %s, %s\n",infile,strerror(errno));
		return 1;
	}

	if ((outfd=open(outfile,O_WRONLY|O_CREAT|O_TRUNC,0644)) < 0) {
		fprintf(stderr,"Cannot open for writing %s, %s\n",outfile,strerror(errno));
		return 1;
	}

	if ((wnd=sliding_window_v4_create_buffer(1024*1024)) == NULL) {
		fprintf(stderr,"Cannot create sliding window\n");
		return 1;
	}

	if ((scc_reader=scc_eia608_reader_create()) == NULL) {
		fprintf(stderr,"Cannot alloc SCC reader\n");
		return 1;
	}

	do {
		assert(sliding_window_v4_is_sane(wnd));
		file_offset += (unsigned long long)sliding_window_v4_lazy_flush(wnd);
		eof = (sliding_window_v4_can_write(wnd) > 0 && sliding_window_v4_refill_from_fd(wnd,infd,0) <= 0);

		assert(sliding_window_v4_is_sane(wnd));
		while (sliding_window_v4_data_available(wnd) >= (eof ? 1 : 4096)) {
			signed long c = scc_eia608_reader_get_word(scc_reader,wnd,eof);
			if (c >= 0) {
				while (cc_framecounter < scc_reader->current_frame) {
					tmp[0] = tmp[1] = no_parity ? 0x00 : 0x80;
					write(outfd,tmp,2);
					cc_framecounter++;
				}

				tmp[0] = c >> 8;
				tmp[1] = c;
				if (no_parity) {
					tmp[0] &= 0x7F;
					tmp[1] &= 0x7F;
				}
				write(outfd,tmp,2);
				cc_framecounter++;

				if (dump_to_stdout) {
					if ((c&0x6000) != 0) {
						fputc((c>>8)&0x7F,stdout);
						if ((c&0x7F) != 0) fputc(c&0x7F,stdout);
					}
				}
			}
			else break;
		}

		file_offset += (unsigned long long)sliding_window_v4_lazy_flush(wnd);
		eof = (sliding_window_v4_can_write(wnd) > 0 && sliding_window_v4_refill_from_fd(wnd,infd,0) <= 0);
	} while (sliding_window_v4_data_available(wnd) >= 2 || !eof);

	scc_reader=scc_eia608_reader_destroy(scc_reader);
	wnd = sliding_window_v4_destroy(wnd);
	close(outfd);
	close(infd);
	return 0;
}

