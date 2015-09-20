#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

#include "parity.h"

int main(int argc,char **argv) {
	static const char *header = "Scenarist_SCC V1.0\n\n";
	unsigned long long counter=0;
	unsigned char tmp[128];
	int ifd,ofd,datac=0;
	unsigned int word;
	int H,M,S,F;

	if (argc < 3) {
		fprintf(stderr,"raw_eia608_to_scc <input raw> <output SCC>\n");
		return 1;
	}

	if ((ifd=open(argv[1],O_RDONLY)) < 0) {
		fprintf(stderr,"Cannot open input\n");
		return 1;
	}
	if ((ofd=open(argv[2],O_WRONLY|O_CREAT|O_TRUNC,0644)) < 0) {
		fprintf(stderr,"Cannot open output\n");
		return 1;
	}

	write(ofd,header,strlen(header));

	while (read(ifd,tmp,2) == 2) {
		word = ((unsigned int)tmp[0] << 8U) + (unsigned int)tmp[1];

		/* the raw capture might be missing parity. recompute it.
		 * if the raw value has even parity, then we must set bit 7 to make it odd. */
		word &= 0x7F7F;
		word |= u8_parity(word>>8) == 0   ? 0x8000 : 0x0000;
		word |= u8_parity(word&0xFF) == 0 ?   0x80 :   0x00;

		/* emit SCC output */
		if ((word&0x7F7F) == 0) {
			if (datac != 0) {
				write(ofd,"\n",1);
				datac = 0;
			}
		}
		else {
			if (datac == 0) {
				unsigned long long t = counter * 1001ULL;

				S = (unsigned int)(t / 30000ULL);
				M = (S / 60ULL) % 60ULL;
				H = S / 3600ULL;
				F = (unsigned int)((t / 1000ULL) % 30ULL);
				S %= 60;

				sprintf((char*)tmp,"%02u:%02u:%02u:%02u\t",H,M,S,F);
				write(ofd,tmp,strlen((char*)tmp));
			}

			sprintf((char*)tmp,"%04x ",word);
			write(ofd,tmp,strlen((char*)tmp));

			datac++;
		}

		counter++;
	}

	write(ofd,"\n",1);
	close(ofd);
	close(ifd);
	return 0;
}

