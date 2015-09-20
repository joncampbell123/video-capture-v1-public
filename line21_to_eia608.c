#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

#include "eia608-decoder.h"
#include "line21-analog-decoder.h"

static int no_parity = 0;
static int nonstateful = 0;
static int dump_to_stdout = 0;
static int infd = -1,outfd = -1;
static int in_scan_width = 756;
static const char *infile = NULL;
static const char *outfile = NULL;
static unsigned char scanline[4096];
static unsigned long long scan_counter=0;
static int scc_data = 0;

static int sccfd = -1;
static const char *scc_file = NULL;

static void help() {
	fprintf(stderr,"line21_to_eia608 -i <raw video> -o <raw dump of 16-bit CC words>\n");
	fprintf(stderr,"  -w   width of scan line in raw video (in test files, usually 756)\n");
	fprintf(stderr,"  -d   also decode and dump text to STDOUT\n");
	fprintf(stderr,"  -scc <file>   also create an SCC file\n");
	fprintf(stderr,"  -ns  disable stateful decoding\n");
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
			else if (!strcmp(a,"scc")) {
				scc_file = argv[i++];
			}
			else if (!strcmp(a,"o")) {
				outfile = argv[i++];
			}
			else if (!strcmp(a,"np")) {
				no_parity = 1;
			}
			else if (!strcmp(a,"ns")) {
				nonstateful = 1;
			}
			else if (!strcmp(a,"w")) {
				in_scan_width = atoi(argv[i++]);
				if (in_scan_width < 1 || in_scan_width > 4000) abort();
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
	eia608_analog_line21_decoder_state ast;
	unsigned char tmp[2];
	long ccword;
	int rd;

	eia608_decoder_analog_line21_decoder_state_init(&ast);

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

	if (scc_file != NULL) {
		static const char *header = "Scenarist_SCC V1.0\n\n";

		if ((sccfd=open(scc_file,O_WRONLY|O_CREAT|O_TRUNC,0644)) < 0) {
			fprintf(stderr,"Cannot open scc file for writing\n");
			return 1;
		}
		write(sccfd,header,strlen(header));
	}

	while ((rd=read(infd,scanline,in_scan_width)) == in_scan_width) {
		ccword = eia608_decoder_parse_analog_line21(scanline,in_scan_width,nonstateful?NULL:(&ast));
		if (ccword >= 0) {
			if (no_parity) ccword &= 0x7F7F;
			tmp[0] = ((unsigned int)ccword >> 8);
			tmp[1] = ((unsigned int)ccword & 0xFF);

			if (dump_to_stdout) {
				if ((ccword&0x6000) != 0) {
					fputc((ccword>>8)&0x7F,stdout);
					fputc(ccword&0x7F,stdout);
				}
			}
		}
		else {
			tmp[0] = tmp[1] = 0;
		}

		if (sccfd >= 0) {
			char tmp[64];

			if (ccword >= 0 && (ccword&0x7F7F) != 0) {
				if (scc_data == 0) {
					unsigned long long cnv;
					unsigned int H,M,S,F;

					cnv = (scan_counter * 1001ULL) / 30000ULL;
					S = (unsigned int)(cnv % 60ULL);
					M = (unsigned int)((cnv / 60ULL) % 60ULL);
					H = (unsigned int)((cnv / 60ULL) / 60ULL);
					F = (unsigned int)(((scan_counter * 1001ULL) % 30000ULL) / 1000ULL);

					sprintf(tmp,"%02u:%02u:%02u;%02u\t",H,M,S,F);
					write(sccfd,tmp,strlen(tmp));
				}

				sprintf(tmp,"%04x ",(unsigned int)ccword);
				write(sccfd,tmp,strlen(tmp));
				scc_data++;
			}
			else {
				if (scc_data != 0) {
					write(sccfd,"\n\n",2);
					scc_data = 0;
				}
			}
		}

		write(outfd,tmp,2);
		scan_counter++;
	}

	if (sccfd >= 0) {
		write(sccfd,"\n",1);
		close(sccfd);
	}

	close(outfd);
	close(infd);
	return 0;
}

