
#ifndef __ISP_UTILS_V4_EIA608_EIA608_DECODER_H
#define __ISP_UTILS_V4_EIA608_EIA608_DECODER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EIA608_MAX_DELAY_BUFFER			64

/* NTS: We define 2 extra columns so that the l-r fill works */
#define CC_ROWS					(15+1)
#define CC_COLUMNS				(32+2)
#define CC_X_OFFSET				1
#define CC_Y_OFFSET				0
#define CC_DISPLAY_COLUMNS			32
#define CC_DISPLAY_ROWS				15

#define CC_TOTAL_SCANLINES			195

#define CC_FONT_WIDTH				8
#define CC_FONT_HEIGHT				13
/* NTS: The FCC document mentions that in NTSC the CC covers 195 scan lines, with 15 rows. 195/15 == 13 */

typedef struct CCCHAR { /* == 24 bits */
	uint8_t				ch;		/* character code (0=empty) */
							/* WARNING: character codes are NOT ASCII, even though MOST of
							 *          the EIA-608 charset happens to match up with ASCII.
							 *          To obtain ASCII text please use function
							 *          eia608_decoder_cchar_to_ascii() */
	/* { */
	unsigned int			charset:2;	/* character set (0=ASCII) */
	unsigned int			color:3;	/* color/style (0=white) */
	unsigned int			italic:1;
	unsigned int			underline:1;
	unsigned int			background_transparent:1;
	/* } */
	/* { */
	unsigned int			background_color:3; /* (0=black) */
	unsigned int			_reserved_:5;
	/* } */
} __attribute__((packed)) CCCHAR;

enum {
	CC_PRESENT_DEBUG=0,			/* DEBUGGING: We show the text on the last line to show receipt of data */
	CC_PRESENT_ROLLUP,
	CC_PRESENT_POPON,
	CC_PRESENT_PAINTON
};

typedef struct eia608_decoder {
	CCCHAR*					current_page;
	CCCHAR*					next_page;
	CCCHAR					current_attr;

	/* { */
	unsigned int				updated:1;
	unsigned int				debug_show_chars:1;		/* print missed chars in debug mode */
	unsigned int				debug_rollup_interrupt:1;	/* print debug message if incoming char interrupts roll-up scroll */
	unsigned int				xds_input:1;			/* set if an XDS start code is detected, cleared when non-XDS code is encountered */
	unsigned int				space_on_format_change:1;	/* 0x112x format codes automatically insert a space (recommended) */
	unsigned int				next_line_while_rollup:1;	/* if set, print on next line while rollup animation is in progress.
										   do not set unless your rendering code is prepared to handle that case.
										   off by default. will still block however if a second carriage return
										   occurs during rollup animation. */
	unsigned int				l_r_fill:1;			/* always fill in one space to the left & right of chars (recommended) */
	unsigned int				autowrap:1;
	/* } */

	/* { */
	unsigned int				_f_wait_for_rollup:1;		/* set by library if waiting for rollup to complete */
	unsigned int				allow_white_background:1;	/* if set, extended attributes parsing will allow white background */
	unsigned int				clip_overwrite_on_31st_col:1;	/* if set, extended characters cannot be printed on the 31st column */
	unsigned int				_reserved_:5;
	/* } */

	uint8_t					present_mode;
	uint8_t					rollup_row;
	uint8_t					rollup_rowcount;
	uint8_t					wait_for_scroll;
	uint8_t					rollup_scroll;			/* if nonzero, we're in the middle of a rollup scroll */
	uint8_t					delayed_char;
	uint8_t					cursor_x;
	uint8_t					cursor_y;
	uint8_t					rollup_fontheight;
	uint16_t				last_data;
	uint16_t				last_122x;
	uint16_t				timeout_data;			/* number of frames after which CC data is automatically cleared. */
										/* that way transitions from CC'd to non-CC'd video does not leave
										 * captions stuck on the screen */
	uint16_t				timeout_data_counter;
	/* callbacks for updating the screen. coordinates are in character rows/columns, and start/end are inclusive */
	void					(*on_update_screen)(void *ctx,uint8_t xstart,uint8_t ystart,uint8_t xend,uint8_t yend);
	/* callback function should move the region up one scanline and clear the bottom-most one. called in rollup mode on newline */
	void					(*on_rollup_screen)(void *ctx,uint8_t ystart,uint8_t yend);

	/* delay buffer for rollup captions, so that the scrolling animation can
	 * complete before decoding more data */
	uint16_t				delay[EIA608_MAX_DELAY_BUFFER];
	uint16_t				delay_in,delay_out;

	/* debug */
	unsigned char				rollup_slow_cnt,rollup_slow_div;
} eia608_decoder;

extern const uint8_t eia608_std_colors[8*3];
extern const uint16_t eia608_row_to_rowcode_val[16];

void eia608_decoder_take_char(eia608_decoder *eia,uint8_t c);
unsigned int eia608_decoder_can_accept_data(eia608_decoder *x,uint16_t w);
void eia608_decoder_delay_flush(eia608_decoder *x);
void eia608_decoder_step_field(eia608_decoder *x);
void eia608_decoder_step_frame(eia608_decoder *x);
void eia608_decoder_free(eia608_decoder *x);
void eia608_decoder_debug(eia608_decoder *eia,const char *fmt,...);
void eia608_decoder_end_of_caption(eia608_decoder *eia);
void eia608_decoder_erase_displayed_memory(eia608_decoder *eia);
void eia608_decoder_erase_non_displayed_memory(eia608_decoder *eia);
void eia608_decoder_switch_popon_presentation(eia608_decoder *eia);
void eia608_decoder_reset(eia608_decoder *x);
int eia608_decoder_is_nonspace_data(eia608_decoder __attribute__((unused)) *eia,uint16_t cc);
void eia608_decoder_take_char(eia608_decoder *eia,uint8_t c);
uint8_t *eia608_get_font_8x13_bmp(eia608_decoder __attribute__((unused)) *eia,CCCHAR *cch);
uint8_t eia608_decoder_midrow_code_to_row(eia608_decoder __attribute__((unused)) *eia,uint16_t word);
const uint8_t *eia608_decoder_get_color(eia608_decoder __attribute__((unused)) *eia,uint8_t color);
const uint8_t *eia608_decoder_get_background_color(eia608_decoder __attribute__((unused)) *eia,uint8_t color);
void eia608_decoder_take_ctrl_pac(eia608_decoder *eia,uint16_t cc);
void eia608_decoder_scrollup_immediate(eia608_decoder *eia,unsigned int start,unsigned int end);
void eia608_decoder_carriage_return(eia608_decoder *eia);
void eia608_decoder_resume_text_display(eia608_decoder *eia);
void eia608_decoder_roll_up(eia608_decoder *eia,uint8_t rows,uint8_t base/*0xFF for default*/);
void eia608_decoder_take_misc_ctrl(eia608_decoder *eia,uint16_t cc);
void eia608_decoder_take_controlword(eia608_decoder *eia,uint16_t cc);
void eia608_decoder_take_word(eia608_decoder *eia,uint16_t cc);
int eia608_decoder_init(eia608_decoder *x);
eia608_decoder *eia608_decoder_create();
eia608_decoder *eia608_decoder_destroy(eia608_decoder *x);
int eia608_decoder_set_display_mode(eia608_decoder *x,int mode);
void eia608_decoder_delay_input(eia608_decoder *x,uint16_t word);
void eia608_decoder_take_word_with_delay(eia608_decoder *x,uint16_t w);
int eia608_decoder_delay_overflow(eia608_decoder *x);
int eia608_decoder_delay_output(eia608_decoder *x);
int eia608_decoder_cchar_to_unicode(uint8_t cc);
int eia608_decoder_cchar_is_transparent(uint8_t cc);
uint16_t eia608_unicode_to_code(unsigned int uc/*unicode char*/);
uint16_t eia608_row_to_rowcode(int y);

#ifdef __cplusplus
}
#endif

#endif /* __ISP_UTILS_V4_EIA608_EIA608_DECODER_H */

