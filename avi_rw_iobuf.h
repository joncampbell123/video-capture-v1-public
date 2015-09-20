/* AVI index handling, buffer optimization.
 * (C) 2012-2015 Jonathan Campbell.
 * Alternate copy for videocap project.
 *
 * Optimizes reading/writing AVI indexes by reading/writing through a buffer
 * instead of incurring a read/write call per index entry. This boosts AVI
 * closure or AVI loading quite a lot since lseek+read/write calls in Linux
 * are a performance killer compared to using read/write without lseek. */

extern unsigned char*		avi_io_buf;
extern unsigned char*		avi_io_read;
extern unsigned char*		avi_io_write;
extern unsigned char*		avi_io_fence;
extern size_t			avi_io_elemsize;
extern size_t			avi_io_next_adv;
extern size_t			avi_io_elemcount;
extern unsigned char*		avi_io_readfence;

unsigned char *avi_io_buffer_init(size_t structsize);
void avi_io_buffer_free();

