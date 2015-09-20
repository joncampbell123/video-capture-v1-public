
#include <stdio.h>
#include <stdint.h>
#include "smpte_sdi_vanc.h"
#include "smpte_sdi_vanc_eia608_packet.h"

int smpte_sdi_vanc_decode_eia608_packet(struct smpte_sdi_vanc_eia608_packet *pkt,unsigned char *s,unsigned char *f) {
	if (pkt == NULL || (s+3) > f) return -1;
	pkt->field = (((s[0]>>7)&1)^1)+1;
	pkt->line = (pkt->field == 2 ? 272 : 9) + (s[0]&0x1F);
	pkt->data = (((uint16_t)(s[1]&0xFF)) << 8) + ((uint16_t)(s[2]&0xFF));
	return 0;
}

