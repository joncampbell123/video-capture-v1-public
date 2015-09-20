
#include "sliding_window.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct scc_eia608_reader {
	unsigned int				timebase_num;
	unsigned int				timebase_den;
	unsigned long long			current_frame;

	/* { */
	unsigned int				wait_for_newline:1;
	unsigned int				got_timecode:1;
	unsigned int				_reserved_:6;
	/* } */
} __attribute__ ((__packed__)) scc_eia608_reader;

scc_eia608_reader *scc_eia608_reader_create();
void scc_eia608_reader_free(scc_eia608_reader *x);
void scc_eia608_reader_init(scc_eia608_reader *x);
scc_eia608_reader *scc_eia608_reader_destroy(scc_eia608_reader *x);
signed long scc_eia608_reader_get_word(scc_eia608_reader *x,sliding_window_v4 *w,int eof);

#ifdef __cplusplus
}
#endif

