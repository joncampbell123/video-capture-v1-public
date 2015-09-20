
#include "eia608-demux.h"

void eia608_demux_init(eia608_demux *d,unsigned char want_mode) {
	d->current_mode = EIA608_DM_NONE;
	d->want_modes = 1U << want_mode;
	d->prev_mode = 0;
}

void eia608_demux_add_want(eia608_demux *d,unsigned char want) {
	d->want_modes |= 1U << want;
}

void eia608_demux_remove_want(eia608_demux *d,unsigned char want) {
	d->want_modes &= ~(1U << want);
}

int eia608_demux_take_word(eia608_demux *d,uint16_t cc,unsigned char evenfield/*0=odd 1=even*/) { /* returns mode, or -1 if not the mode you want */
	unsigned char retmode = d->current_mode;

	if (evenfield > 1) return -1;

	cc &= 0x7F7F;
	if ((cc&0x6000) != 0) { /* plain text/data non-control */
	}
	else if (d->current_mode == EIA608_DM_XDS && (cc&0x7F00) == 0x0F00) {
		/* change the current mode now, but return XDS output for this call */
		d->current_mode = d->prev_mode;
	}
	else if ((cc&0x7070) == 0 && (cc&0x0F0F) != 0 && evenfield) {
		/* [P000 CLASS] [P000 TYPE] eXtended Data Services and class and type nonzero. Even field (CC3/CC4) only. */
		/* XDS packet starting. Note that one XDS packet is allowed to interrupt another */
		if (d->current_mode != EIA608_DM_XDS) {
			if (d->prev_mode != EIA608_DM_XDS)
				d->prev_mode = d->current_mode; /* save the mode we return to when the XDS data finishes */

			d->current_mode = retmode = EIA608_DM_XDS;
		}
	}
	else if (d->current_mode != EIA608_DM_XDS) {
		/* a control word. bit 11 at this point determines which channel it belongs to */
		unsigned char cc2 = (cc & 0x800) ? 1 : 0;

		/* filter out CC1/CC2 channel bit */
		cc &= ~0x800;

		if (cc == 0x1420/*Resume Caption Loading*/ ||
			(cc >= 0x1425 && cc <= 0x1427)/*Roll-up caption mode*/ ||
			cc == 0x1429/*Resume Direct Captioning*/) {
			/* known CC channel command */
			d->current_mode = retmode = EIA608_DM_CC1 + cc2 + (evenfield * 2);
		}
		else if (cc == 0x142B/*Resume Text Display*/) {
			d->current_mode = retmode = EIA608_DM_TEXT1 + cc2 + (evenfield * 2);
		}
	}

	if (retmode == EIA608_DM_NONE) return -1;
	else if ((d->want_modes & (1U << retmode)) == 0) return -1;
	return retmode;
}

