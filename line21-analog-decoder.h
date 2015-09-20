
#ifdef __cplusplus
extern "C" {
#endif

/* stateful tracking information for decoding analog Line 21.
 * doing this allows the decoder to be more robust against noise and interference */
typedef struct eia608_analog_line21_decoder_state {
	int		offset;
	unsigned int	scale;
	unsigned char	failures;
	unsigned char	last_thr;
	unsigned char	minv,maxv;
	unsigned char	recursion;
} eia608_analog_line21_decoder_state;

void eia608_decoder_analog_line21_decoder_state_init(eia608_analog_line21_decoder_state *s);
long eia608_decoder_parse_analog_line21(unsigned char *scanline,size_t in_scan_width,eia608_analog_line21_decoder_state *s);
void eia608_decoder_analog_line21_decoder_state_dump(FILE *f,eia608_analog_line21_decoder_state *s);

#ifdef __cplusplus
}
#endif

