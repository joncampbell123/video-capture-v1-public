
#ifndef __ISP_UTILS_V4_EIA608_EIA608_DEMUX_H
#define __ISP_UTILS_V4_EIA608_EIA608_DEMUX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	EIA608_DM_NONE=0,		/* not paying attention to anything yet */
	EIA608_DM_CC1,
	EIA608_DM_CC2,
	EIA608_DM_CC3,
	EIA608_DM_CC4,
	EIA608_DM_TEXT1,
	EIA608_DM_TEXT2,
	EIA608_DM_TEXT3,
	EIA608_DM_TEXT4,
	EIA608_DM_XDS,

	EIA608_DM_MAX
};

typedef struct eia608_demux {
	unsigned char		current_mode;	/* EIA608_DM_* */
	unsigned char		prev_mode;	/* XDS: what mode to return to when packet terminates */
	unsigned short		want_modes;	/* (1 << EIA608_DM_*) bitfield */
} eia608_demux;

static const eia608_demux EIA608_DEMUX_INIT = {
	0,
	0,
	0
};

void eia608_demux_add_want(eia608_demux *d,unsigned char want);
void eia608_demux_init(eia608_demux *d,unsigned char want_mode);
void eia608_demux_remove_want(eia608_demux *d,unsigned char want);
int eia608_demux_take_word(eia608_demux *d,uint16_t cc,unsigned char evenfield/*0=odd 1=even*/);

#ifdef __cplusplus
}
#endif

#endif /* __ISP_UTILS_V4_EIA608_EIA608_DEMUX_H */

