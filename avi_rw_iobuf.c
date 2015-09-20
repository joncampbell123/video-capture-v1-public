/* AVI index handling, buffer optimization.
 * (C) 2012-2015 Jonathan Campbell.
 * Alternate copy for videocap project.
 *
 * Optimizes reading/writing AVI indexes by reading/writing through a buffer
 * instead of incurring a read/write call per index entry. This boosts AVI
 * closure or AVI loading quite a lot since lseek+read/write calls in Linux
 * are a performance killer compared to using read/write without lseek. */

#include "rawint.h"
#include "avi_reader.h"
#include "avi_rw_iobuf.h"
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>

/* common index writing code.
 * This code originally wrote the index one struct at a type.
 * If you know anything about syscall overhead and disk I/O,
 * that's a VERY inefficent way to do it! So index read/write
 * code uses our buffer to batch the index entries into RAM
 * and write them to disk in one burst */
unsigned char*		avi_io_buf = NULL;
unsigned char*		avi_io_read = NULL;
unsigned char*		avi_io_write = NULL;
unsigned char*		avi_io_fence = NULL;
size_t			avi_io_elemsize = 0;
size_t			avi_io_next_adv = 0;
size_t			avi_io_elemcount = 0;
unsigned char*		avi_io_readfence = NULL;

unsigned char *avi_io_buffer_init(size_t structsize) {
#define GROUPSIZE ((size_t)(65536*2))
	if (avi_io_buf == NULL) {
		avi_io_buf = malloc(GROUPSIZE);
		if (avi_io_buf == NULL) return NULL;
	}

	avi_io_elemcount = GROUPSIZE / structsize;
	avi_io_fence = avi_io_buf + (avi_io_elemcount * structsize);
	avi_io_readfence = avi_io_buf;
	avi_io_elemsize = structsize;
	avi_io_write = avi_io_buf;
	avi_io_read = avi_io_buf;
	return avi_io_buf;
#undef GROUPSIZE
}

void avi_io_buffer_free() {
	if (avi_io_buf) {
		free(avi_io_buf);
		avi_io_buf = NULL;
	}
	avi_io_readfence = avi_io_buf;
	avi_io_write = avi_io_buf;
	avi_io_read = avi_io_buf;
}

