/* DEVELOPMENT WARNING: This API is brand new, and not yet solidified.
 * Expect it to morph and change over time, behavior may change slightly
 * or if absolutely necessary, may change abruptly and suddenly. New
 * functions may appear and some functions may disappear.
 *
 * In short, this API is not yet solid!
 *    -- Jonathan C */

#ifndef __SLIDING_WINDOW_V4_H
#define __SLIDING_WINDOW_V4_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <string.h>

/* we now allow sliding windows to be NULL, memory buffer, or memory-mapped file */
enum {
	SLIDING_WINDOW_V4_TYPE_NULL=0,
	SLIDING_WINDOW_V4_TYPE_BUFFER,
	SLIDING_WINDOW_V4_TYPE_MMAP
};

/* NTS: This is the new improved sliding window implementation.
 *      It works exactly like the isp-utils sliding_window, but also
 *      permits the sliding window to represent a memory-mapped file
 *      source, or a user-controlled buffer. The first 4 fields
 *      line up EXACTLY with the isp-utils version.
 *
 *      When isp-utils is deprecated, I will add a #define here to
 *      map sliding_window -> sliding_window_v4
 *
 *      Hopefully this can take the place of that horrible "mmapfile"
 *      C++ mess I made in isp-utils. */
typedef struct sliding_window_v4 {
	unsigned char		*buffer;
	unsigned char		*fence;
	unsigned char		*data;
	unsigned char		*end;
	unsigned char		type;	/* SLIDING_WINDOW_V4_TYPE_... */
	union {
		/* SLIDING_WINDOW_V4_TYPE_NULL */
		/* No declarations needed. NULL windows are valid so long as buffer == fence == data == end == NULL */
		/* SLIDING_WINDOW_V4_TYPE_BUFFER */
		struct {
			/* In this scenario, the return value of malloc() is assigned to "buffer" and never changes, unless
			 * the buffer is realloc'd to enlarge or shrink it. If not owned by this library, then "buffer" can
			 * change on the whim of the program that maintains it for us so long as window "sanity" is maintained.
			 *
			 * buffer = malloc(size)
			 * fence = buffer + size */
			void		(*owner_free_buffer)(void *buf);	/* callback to free it if not owned by this library */
			uint8_t		lib_owner;				/* 1=this library allocated the buffer and manages it 0=externally allocated */
			union {
				size_t		integer;
				void*		pointer;
			} user; /* the caller may assign custom data here, pointer or integer */
		} buffer;
		/* SLIDING_WINDOW_V4_TYPE_MMAP */
		struct {
			/* In this scenario, buffer and pointers may be set to NULL initially. Upon request for more data,
			 * or if the function is explicitly called to do so, the OS memory-mapping API is used to map
			 * file contents into memory, and "buffer" is set to the base of that mapping. If "flush" or
			 * other API functions are called, the "buffer" pointer value may change depending on
			 * what the OS mmap() function returns.
			 *
			 * buffer == data == end == fence == NULL if no mapping
			 *     or
			 * buffer = mmap(...)
			 * fence = buffer + mmap_size
			 *
			 * the "allocation" length in this case reflects how much was actually memory-mapped */
#if defined(WIN32)
			/* Variables for future Microsoft Windows port */
			HANDLE		handle;				/* Win32 "HANDLE" to the file */
			HANDLE		fmapping;			/* Win32 "File mapping" object */
#else
			int		fd;				/* file descriptor (TODO: Win32 port should declare as "HANDLE") */
#endif
			uint64_t	file_offset;			/* file offset that corresponds to contents seen at *buffer.
									   so if the data pointer is somewhere within the buffer, the file offset it
									   corresponds to is:
									   
									   (size_t)data - (size_t)buffer + file_offset
									   
									   if buffer == NULL (not yet mapped) then this file offset becomes what the
									   mapper will mmap() from next (rounded down to the nearest page boundary though) */
			size_t		last_lazy_sz;			/* last value passed to mmap_lazy() which is used in writing mode */
			size_t		mmap_limit;			/* caller specified mmap limit. library will not mmap() more than this at any time.
									   the caller will probably have thousands of reasons why we can't go wild and mmap
									   whatever the fuck we want, and for good reason. */
			unsigned int	rw:1;				/* 1=mmap read/write  0=mmap read-only */
			unsigned int	fd_owner:1;			/* 1=this library "owns" the file descriptor and
									   will close it upon destruction of the window */
			unsigned int	writing_mode:1;			/* 1=write mode: on mmap/flush set end == data  0=read mode: on mmap/flush set end == fence */
			unsigned int	malloc_fallback:1;		/* 1=memory mapping failed for whatever reason, so the library had to malloc()
									   the buffer and read in the contents instead. when mapping again, the library
									   will free() the buffer, then attempt another memory map.

									   this fallback will probably not be necessary for Linux, but is needed to deal
									   with deficiencies in Microsoft Windows where memory-mapping may not work, such
									   as when one process attempts to map the same "page" from the file to two different
									   virtual memory addresses (because the calling process has two instances of this
									   object pointed at a file perhaps) --- not a problem in Linux, but too much for
									   Windows to handle apparently.
									   
									   FIXME: This fallback mode currently offers no "synchronization" when in r/w mode */
			unsigned int	eof_flag:1;			/* 1=the mapping, or attempt at mapping, bumps up against the end of the file */
			unsigned int	extend_while_writing:1;		/* 1=if writing mode and mmap_lazy() detects any attempt to extend into EOF, the
									     library will automatically extend the file out up to "sz" bytes to satisfy the
									     request.
									    
									   NOTE: Because of the way this works, expect your file to have extra padding or
									         junk past what you have written when you finish with the window. When closing
										 the window and the file, you may want to ftruncate() the file back to contain
										 only the data you intended to write. */
		} mmap;
	} u;
} sliding_window_v4;
/* at all times in proper operation:
 *
 *  +----------------------- BUFFER ---------------------------------------------------+
 *  |        | valid data   |                                                          |
 *  +----------------------------------------------------------------------------------+
 *  ^        ^               ^                                                          ^
 *  |        |               |                                                          |
 *  buffer   data            end                                                        fence
 *
 *  where:
 *     buffer = first byte of allocated memory region
 *     data   = next byte of data to consume
 *     end    = last byte + 1 of data to consume (also: data == end means no data to consume)
 *     fence  = last byte + 1 of allocated memory region
 *
 *     and buffer <= data <= end <= fence and buffer != fence.
 *
 *  a reset buffer has:
 *     data = buffer
 *     end = buffer
 *     no data to consume
 *
 *  inserting data to consume:
 *     copy data to memory location "end" and advance "end" that many bytes.
 *     obviously the byte count must be restricted so that end + bytes <= fence.
 *
 *  consuming data:
 *     determine length of valid data (size_t)(end - data) and consume that or less.
 *     directly accesss bytes through data pointer. advance "data" pointer by exactly
 *     the number of bytes consumed. always maintain data <= end.
 *
 *  when data and/or end pointers are close to the fence:
 *     determine length of valid data (size_t)(end - data), move that data back to the
 *     beginning of the buffer, set data = buffer and end = buffer + length. then you
 *     can use the newly regained memory to load and parse more data.
 *
 *  isp v4.0 new: We now allow the caller to do his own allocation, then call
 *  sliding_window_v4_alloc() to create a window, instead of using up another pointer
 *  to allocate one. The caller must use sliding_window_v4_free() on it.
 *
 *  NEW: the sliding window can also be NULL, But then, the window must be
 *       buffer == data == end == NULL 
 *
 *  NEW: the sliding window can also represent a memory-mapped view of a file (if supported
 *       by the host OS). Note that when first created, buffer == data == end == NULL, the
 *       code will not mmap until you ask it to. Also note that when calling mmap functions
 *       buffer is either non-NULL and pointing directly to the mmap()'d region, or NULL.
 *       Unlike traditional windows, you must not assume that buffer != NULL. But you may
 *       assume that if buffer != NULL the other pointers are non-NULL. The "is_sane"
 *       function will flag any mmap window that does not fit those constraints.
 *
 *       In this mode, whether you intend to read or write with the window
 *       is important:
 *
 *       - in read mode, you are expected to read from the window using the
 *       data pointer (up to "end"). On "flush" the data pointer is moved back towards buffer
 *       by re-mapping the memory-map window. In almost all circumstances, end == fence
 *       (but don't assume that!).
 *
 *       - in write mode, you are expected to write data to the end pointer (just as you
 *         would a normal window). the flush functions however still go by the "data" pointer,
 *         so as you write you must make sure "data" follows the "end" pointer. In most
 *         circumstances, implementations will simply write to ->end, advance ->end, and
 *         then set data == end
 *
 *       - the "lazy mmap" function is written to remap if and only if your offset is not
 *         yet visible in the mapping, or if remapping would be required to ensure that
 *         the specified number of bytes are visible. The function is written to ensure
 *         that many bytes are available EXCEPT in cases where the range passes the end of
 *         the file. If the offset itself is past the EOF, the function will return failure
 *         and buffer == NULL */

/* TODO: Which of these functions should NOT be publicly visible? */
sliding_window_v4*	sliding_window_v4_create_null();
sliding_window_v4*	sliding_window_v4_create_buffer(size_t size);
sliding_window_v4*	sliding_window_v4_destroy(sliding_window_v4 *sw);
int			sliding_window_v4_resize(sliding_window_v4 *sw,size_t new_size);
size_t			sliding_window_v4_data_advance(sliding_window_v4 *sw,size_t bytes);
size_t			sliding_window_v4_wrote(sliding_window_v4 *sw,size_t bytes);
size_t			sliding_window_v4_flush(sliding_window_v4 *sw);
size_t			sliding_window_v4_lazy_flush(sliding_window_v4 *sw);
sliding_window_v4*	sliding_window_v4_alloc_buffer(sliding_window_v4 *sw,size_t size);
sliding_window_v4*	sliding_window_v4_set_custom_buffer(sliding_window_v4 *sw,size_t size,const unsigned char *data);
int			sliding_window_v4_is_sane(sliding_window_v4 *sw);
void			sliding_window_v4_free(sliding_window_v4 *sw);
uint64_t		sliding_window_v4_ptr_to_mmap_offset(sliding_window_v4 *sw,unsigned char *ptr);
int			sliding_window_v4_mmap_lseek(sliding_window_v4 *sw,uint64_t offset);
sliding_window_v4*	sliding_window_v4_alloc_mmap(sliding_window_v4 *sw,size_t max_size);
void			sliding_window_v4_do_munmap(sliding_window_v4 *sw);
int			sliding_window_v4_mmap_set_fd(sliding_window_v4 *sw,int fd,int lib_ownership/*whether the caller gives the lib ownership of the fd*/);
unsigned char*		sliding_window_v4_mmap_offset_to_ptr(sliding_window_v4 *sw,uint64_t ofs);
int			sliding_window_v4_mmap_lazy_lseek(sliding_window_v4 *sw,uint64_t ofs,size_t len);
void			sliding_window_v4_do_mmap_sync(sliding_window_v4 *sw);
int			sliding_window_v4_do_mmap(sliding_window_v4 *sw);
sliding_window_v4*	sliding_window_v4_create_mmap(size_t limit,int fd);
int			sliding_window_v4_mmap_data_advance(sliding_window_v4 *sw,unsigned char *to);
int			sliding_window_v4_do_mmap_autoextend(sliding_window_v4 *sw,uint64_t ofs,size_t len);

/* set whether or not the file is automatically extended upon writing */
static inline int sliding_window_v4_set_mmap_extend_while_writing(sliding_window_v4 *sw,int ex) {
	if (sw == NULL) return 0;
	if (sw->type != SLIDING_WINDOW_V4_TYPE_MMAP) return 0;
	if (sw->u.mmap.rw == 0) return 0;
	sw->u.mmap.extend_while_writing = ex;
	return 1;
}

/* set write mode for mmap objects */
static inline int sliding_window_v4_set_mmap_write_mode(sliding_window_v4 *sw,int write) {
	if (sw == NULL) return 0;
	if (sw->type != SLIDING_WINDOW_V4_TYPE_MMAP) return 0;
	if (sw->u.mmap.rw == 0) return 0;
	sw->u.mmap.writing_mode = write;
	return 1;
}

/* set read/write flag for mmap objects */
static inline int sliding_window_v4_set_mmap_rw(sliding_window_v4 *sw,int rw) {
	if (sw == NULL) return 0;
	if (sw->type != SLIDING_WINDOW_V4_TYPE_MMAP) return 0;
	sw->u.mmap.rw = rw;
	return 1;
}

static inline int sliding_window_v4_mmap_eof(sliding_window_v4 *sw) {
	if (sw == NULL) return 0;
	if (sw->type != SLIDING_WINDOW_V4_TYPE_MMAP) return 0;
	return sw->u.mmap.eof_flag;
}

static inline int sliding_window_v4_set_custom_buffer_free_cb(sliding_window_v4 *sw,void (*cb)(void *)) {
	if (sw->type == SLIDING_WINDOW_V4_TYPE_BUFFER && !sw->u.buffer.lib_owner) {
		sw->u.buffer.owner_free_buffer = cb;
		return 1;
	}

	return 0;
}

static inline unsigned char sliding_window_v4_is_readable(sliding_window_v4 *sw) {
	if (sw == NULL) return 0;

	/* if the buffer is not null, then you can read from it */
	return (sw->buffer != NULL)?1:0;
}

/* since mmap() windows are permitted to be read-only the calling program might want to
 * ask at any time if the window is writeable. programs that don't will segfault the
 * minute they try to write read-only mappings, and it serves them right! */
static inline unsigned char sliding_window_v4_is_writeable(sliding_window_v4 *sw) {
	if (sw == NULL) return 0;

	if (sw->type == SLIDING_WINDOW_V4_TYPE_MMAP) {
		/* if buffer is non-null and the caller wanted read/write mapping */
		return (sw->buffer != NULL && sw->u.mmap.rw)?1:0;
	}

	/* if the buffer is not null */
	return (sw->buffer != NULL)?1:0;
}

static inline unsigned char sliding_window_v4_type(sliding_window_v4 *sw) {
	return sw->type;
}

/* WARNING: this is intended for use on sliding_window_v4 structs you just allocated
 *          yourself from the stack, your data area, etc.
 *
 *          When to use it:
 *
 *          sliding_window_v4 window;  <-- declared on stack
 *          sliding_window_v4_zero(&window); <-- initialize struct to known state
 *          sliding_window_v4_alloc_buffer(&window); <-- allocate buffer within window
 *          <... typical code to use the window object ...>
 *          sliding_window_v4_free(&window);
 *
 *          When NOT to use it:
 *
 *          sliding_window_v4 *wnd;
 *          wnd = sliding_window_v4_create_buffer(4096); <-- allocated by create function
 *          sliding_window_v4_zero(wnd); <-- this causes memory leak, malloc()'d buffer is lost!!!
 *          sliding_window_v4_destroy(wnd); <-- and then this does nothing because the window was zeroed
 */
static inline void sliding_window_v4_zero(sliding_window_v4 *sw) {
	memset(sw,0,sizeof(*sw));
}

static inline int sliding_window_v4_is_buffer_alloc(sliding_window_v4 *sw) {
	return (sw->type == SLIDING_WINDOW_V4_TYPE_BUFFER) && (sw->buffer != NULL);
}

static inline size_t sliding_window_v4_alloc_length(sliding_window_v4 *sw) {
	return (size_t)(sw->fence - sw->buffer);
}

static inline size_t sliding_window_v4_data_offset(sliding_window_v4 *sw) {
	return (size_t)(sw->data - sw->buffer);
}

static inline size_t sliding_window_v4_data_available(sliding_window_v4 *sw) {
	return (size_t)(sw->end - sw->data);
}

static inline size_t sliding_window_v4_can_write(sliding_window_v4 *sw) {
	return (size_t)(sw->fence - sw->end);
}

static inline size_t sliding_window_v4_write_offset(sliding_window_v4 *sw) {
	return (size_t)(sw->end - sw->buffer);
}

static inline void sliding_window_v4_empty(sliding_window_v4 *sw) {
	/* poof */
	sw->data = sw->buffer;
	sw->end = sw->buffer;
}

static inline void discard_sliding_window_v4(sliding_window_v4 **pw) {
	sliding_window_v4 *w;
	if (!pw) return;
	w = *pw;
	if (w) *pw = sliding_window_v4_destroy(w);
}

static inline void __attribute__((unused)) sliding_window_v4_refill_from_ptr(sliding_window_v4 *sw,uint8_t **ptr,uint8_t *fence) {
	size_t crem = (size_t)(fence - *ptr);
	size_t brem = sliding_window_v4_can_write(sw);
	if (sw->type != SLIDING_WINDOW_V4_TYPE_BUFFER) return; /* <- TODO */
	if (crem > brem) {
		sliding_window_v4_lazy_flush(sw);
		brem = sliding_window_v4_can_write(sw);
		if (crem > brem) crem = brem;
	}
	if (crem > 0) {
		memcpy(sw->end,*ptr,crem); *ptr += crem;
		sliding_window_v4_wrote(sw,crem);
	}
}

static inline void __attribute__((unused)) sliding_window_v4_refill_from_ptr_noflush(sliding_window_v4 *sw,uint8_t **ptr,uint8_t *fence) {
	size_t crem = (size_t)(fence - *ptr);
	size_t brem = sliding_window_v4_can_write(sw);
	if (sw->type != SLIDING_WINDOW_V4_TYPE_BUFFER) return; /* <- TODO */
	if (crem > brem) crem = brem;
	if (crem > 0) {
		memcpy(sw->end,*ptr,crem); *ptr += crem;
		sliding_window_v4_wrote(sw,crem);
	}
}

/* socket I/O. returns -1 if disconnect, 0 if nothing read.
 * WARNING: code uses recv/send to carry out I/O, which works ONLY on sockets */
ssize_t			sliding_window_v4_refill_from_socket(sliding_window_v4 *sw,int fd,size_t max);
ssize_t			sliding_window_v4_empty_to_socket(sliding_window_v4 *sw,int fd,size_t max);

/* file I/O. returns -1 if disconnect, 0 if nothing read.
 * this may block, which is not a problem for file access, but may be a problem if the descriptor
 * is a character TTY device or FIFO. In which case, you should make the descriptor non-blocking. */
ssize_t			sliding_window_v4_refill_from_fd(sliding_window_v4 *sw,int fd,size_t max);
ssize_t			sliding_window_v4_empty_to_fd(sliding_window_v4 *sw,int fd,size_t max);

int sliding_window_v4_safe_strtoui(sliding_window_v4 *w,unsigned int *ret,int base);

#ifdef __cplusplus
}
#endif

#endif
