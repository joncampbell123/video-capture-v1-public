#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

#include "parity.h"
#include "line21-analog-encoder.h"

int main(int argc,char **argv) {
	unsigned long long counter=0;
	unsigned char scan[4096];
	unsigned char tmp[2];
	unsigned int word;
	int width = 756;
	int ifd,ofd;

	if (argc < 3) {
		fprintf(stderr,"raw_eia608_to_line21 <input raw> <raw line12 scanlines> [scan width]\n");
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
	if (argc > 3) {
		width = atoi(argv[3]);
		if (width < 64 || width > 3000) return 1;
	}

	while (read(ifd,tmp,2) == 2) {
		word = ((unsigned int)tmp[0] << 8U) + (unsigned int)tmp[1];

		/* the raw capture might be missing parity. recompute it.
		 * if the raw value has even parity, then we must set bit 7 to make it odd. */
		word &= 0x7F7F;
		word |= u8_parity(word>>8) == 0   ? 0x8000 : 0x0000;
		word |= u8_parity(word&0xFF) == 0 ?   0x80 :   0x00;

		/* render the scanline */
		eia608_decoder_generate_analog_line21(scan,(size_t)width,word);

		/* emit the scanline */
		write(ofd,scan,width);
		counter++;
	}

	write(ofd,"\n",1);
	close(ofd);
	close(ifd);
	return 0;
}

