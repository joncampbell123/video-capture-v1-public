
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINE21_SINE_LENGTH		512

extern unsigned char*			eia608_line21_sinewave;

int eia608_line21_sine_init();
void eia608_line21_sine_free();
void eia608_decoder_generate_analog_line21(unsigned char *out,size_t width,uint16_t word);

#ifdef __cplusplus
}
#endif

