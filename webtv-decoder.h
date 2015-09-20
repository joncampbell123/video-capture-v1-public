
#ifndef __ISP_UTILS_V4_EIA608_WEBTV_DECODER_H
#define __ISP_UTILS_V4_EIA608_WEBTV_DECODER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct eia608_webtv_reader {
	unsigned char		assembly[512];
	unsigned int		write;
} eia608_webtv_reader;

void eia608_webtv_reader_init(eia608_webtv_reader *i);
void eia608_webtv_reader_reset(eia608_webtv_reader *i);
int eia608_webtv_checksum(eia608_webtv_reader *i);
int eia608_webtv_on_complete_line(eia608_webtv_reader *i);
int eia608_webtv_take_word(eia608_webtv_reader *i,uint16_t word);

#ifdef __cplusplus
}
#endif

#endif /* __ISP_UTILS_V4_EIA608_WEBTV_DECODER_H */

