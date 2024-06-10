/* Live feed definition.
 * (C) 2008-2015 Jonathan Campbell
 *
 * This structure defines the header of the shared memory segment
 * used between the capture program and the GUI to facilitate
 * the live feed, with or without capture active.
 */

#include <stdint.h>
#include <stddef.h>

/* communication structure in live feed.
 * the live feeds are temporary, don't ever expect to save the buffers long-term because
 * this structure *WILL CHANGE* over time. */
#define LIVE_SHM_SLOTS		64
struct live_shm_header_entry {
	uint32_t		offset;
	uint32_t		field_order; /* 0=none 1=top field first 2=bottom field first */
	uint32_t		generation;
	uint32_t		audio_max_level[4];
	uint32_t		__removed__[4]; /* audio_avg_level */
};

struct live_shm_header {
	uint32_t		header;
	uint32_t		slots;
	uint32_t		width,height;
	uint32_t		stride,frame_size,in;
	uint32_t		this_generation;
	uint32_t		audio_channels,audio_rate;
	uint32_t		color_fmt;
	struct live_shm_header_entry map[LIVE_SHM_SLOTS];
};
/* the feed is invalid the instant this header value disappears */
#define LIVE_SHM_HEADER			0xABCD1234
#define LIVE_SHM_HEADER_UPDATING	0xABCDFEFE

#define LIVE_COLOR_FMT_YUV420		1
#define LIVE_COLOR_FMT_YUV422		2

