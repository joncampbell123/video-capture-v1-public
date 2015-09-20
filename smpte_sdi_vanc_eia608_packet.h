
#ifdef __cplusplus
extern "C" {
#endif

struct smpte_sdi_vanc_eia608_packet {
	unsigned char		field;
	unsigned short		line;	/* translated */
	uint16_t		data;
};

int smpte_sdi_vanc_decode_eia608_packet(struct smpte_sdi_vanc_eia608_packet *pkt,unsigned char *s,unsigned char *f);

#ifdef __cplusplus
}
#endif

