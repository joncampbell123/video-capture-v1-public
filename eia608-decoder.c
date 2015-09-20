
/* NTS:
 *
 * How the EIA-608 font is laid out is very important:
 *
 * 0x00-0x1F      undefined
 * 0x20-0x7F      EIA-608 "ASCII" (some chars replaced with accented chars)
 * 0x80-0xAF      undefined
 * 0xB0-0xCF      Portuguese/German
 * 0xD0-0xEF      Spanish/French
 * 0xF0-0xFF      special character set
 */

/* FIXME: If you slow the rollup WAY down and enable the "scrollup while writing the next line"
 *        function (next_line_while_rollup), text on the edges of the lines can get a bit garbled. */

#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#include <fcntl.h>
#include <stdio.h>

#include <isp-utils-v4/eia-608/eia608-font-8x14.h>
#include <isp-utils-v4/eia-608/eia608-decoder.h>
#include <isp-utils-v4/misc/vgafont_8x14.h>
#include <isp-utils-v4/misc/vgafont_8x8.h>
#include <isp-utils-v4/misc/rawint.h>

const uint16_t eia608_row_to_rowcode_val[16] = {
	0x1060,	/* 0 invalid row */
	0x1140,	/* 1 */
	0x1160,	/* 2 */
	0x1240,	/* 3 */

	0x1260, /* 4 */
	0x1540,	/* 5 */
	0x1560, /* 6 */
	0x1640, /* 7 */

	0x1660, /* 8 */
	0x1740, /* 9 */
	0x1760, /* 10 */
	0x1040, /* 11 */

	0x1340, /* 12 */
	0x1360, /* 13 */
	0x1440, /* 14 */
	0x1460  /* 15 */
};

/* the decoder might be in a scrolling state and not ready to accept data */
unsigned int eia608_decoder_can_accept_data(eia608_decoder *x,uint16_t w) {
	/* if we're in roll-up mode, and doing the animation, and the char is a carriage return code,
	 * then DON'T accept the char */
	if (x->_f_wait_for_rollup == 0 && x->present_mode == CC_PRESENT_ROLLUP && x->rollup_scroll != 0 && (w&(~0x800)) == 0x142D)
		return 0;

	return (x->_f_wait_for_rollup == 0) && (x->delayed_char == 0);
}

void eia608_decoder_step_field(eia608_decoder *x) {
	if (x->rollup_scroll != 0) {
		if ((++x->rollup_slow_cnt) >= x->rollup_slow_div) {
			x->rollup_slow_cnt = 0;
			(x->rollup_scroll)--;
			x->updated = 1;
			if (x->on_rollup_screen && x->rollup_rowcount != 0 &&
				x->rollup_row < CC_DISPLAY_ROWS && (x->rollup_row+x->rollup_rowcount) <= CC_DISPLAY_ROWS) {
				x->on_rollup_screen(x,x->rollup_row,x->rollup_row+x->rollup_rowcount-1);
			}

			/* 2013/03/18: we don't actually move the row up UNTIL scrolling completes */
			if (x->rollup_scroll == 0) {
				eia608_decoder_scrollup_immediate(x,x->rollup_row,x->rollup_row+x->rollup_rowcount);
				x->_f_wait_for_rollup = 0;
			}
		}
	}

	if (x->_f_wait_for_rollup == 0 && x->delayed_char != 0) {
		eia608_decoder_take_char(x,x->delayed_char);
		x->delayed_char = 0;
	}
}

void eia608_decoder_step_frame(eia608_decoder *x) {
	eia608_decoder_step_field(x);
	eia608_decoder_step_field(x);
}

void eia608_decoder_free(eia608_decoder *x) {
	if (x->current_page) {
		free(x->current_page);
		x->current_page = NULL;
	}
	if (x->next_page) {
		free(x->next_page);
		x->next_page = NULL;
	}
}

void eia608_decoder_debug(eia608_decoder *eia,const char *fmt,...) {
	va_list va;

	fprintf(stderr,"eia608_decoder(%p) debug: ",(void*)eia);
	va_start(va,fmt);
	vfprintf(stderr,fmt,va);
	va_end(va);
	fprintf(stderr,"\n");
}

void eia608_decoder_end_of_caption(eia608_decoder *eia) {
	CCCHAR *tmp;

	if (eia->present_mode != CC_PRESENT_POPON) return;

	/* make next page current, current page next, to bring text onscreen */
	tmp = eia->current_page;
	eia->current_page = eia->next_page;
	eia->next_page = tmp;
	eia->updated = 1;

	/* update the display */
	if (eia->on_update_screen != NULL) eia->on_update_screen(eia,0,0,CC_COLUMNS-1,CC_ROWS-1);
}

void eia608_decoder_erase_displayed_memory(eia608_decoder *eia) {
	eia->updated = 1;
	memset(eia->current_page,0,CC_ROWS * CC_COLUMNS * sizeof(CCCHAR));
	if (eia->on_update_screen != NULL) eia->on_update_screen(eia,0,0,CC_COLUMNS-1,CC_ROWS-1);
}

void eia608_decoder_erase_non_displayed_memory(eia608_decoder *eia) {
	if (eia->present_mode != CC_PRESENT_POPON) return;
	memset(eia->next_page,0,CC_ROWS * CC_COLUMNS * sizeof(CCCHAR));
}

void eia608_decoder_switch_popon_presentation(eia608_decoder *eia) {
	if (eia->present_mode != CC_PRESENT_POPON) {
		eia608_decoder_debug(eia,"Entering pop-on mode");
		eia->present_mode = CC_PRESENT_POPON;
	}
}

void eia608_decoder_reset(eia608_decoder *x) {
	memset(x->current_page,0,CC_ROWS * CC_COLUMNS * sizeof(CCCHAR));
	memset(x->next_page,0,CC_ROWS * CC_COLUMNS * sizeof(CCCHAR));
	/* defaults for CC CAPTION "DEBUG" MODE */
	x->present_mode = CC_PRESENT_DEBUG;
	x->rollup_row = CC_DISPLAY_ROWS - 2;
	x->_f_wait_for_rollup = 0;
	x->rollup_slow_cnt = 0;
	x->rollup_slow_div = 0;
	x->rollup_rowcount = 2;
	x->autowrap = 0;
	x->rollup_fontheight = CC_FONT_HEIGHT;
	x->rollup_scroll = 0;
	x->xds_input = 0;
	x->updated = 0;
	x->cursor_x = 0;
	x->last_data = 0;
	x->last_122x = 0;
	x->delayed_char = 0;
	x->delay_in = x->delay_out = 0;
	x->cursor_y = CC_DISPLAY_ROWS - 1;
	memset(&x->current_attr,0,sizeof(x->current_attr));
	x->current_attr.background_color = 7; /* black */
	x->timeout_data_counter = x->timeout_data;
}

int eia608_decoder_is_nonspace_data(eia608_decoder __attribute__((unused)) *eia,uint16_t cc) {
	if ((cc&0x7F7F) == 0) return 0;
	return 1;
}

/* extended charset writing (overwrite character cell prior to cursor) */
void eia608_decoder_take_char_overwrite_prev(eia608_decoder *eia,uint8_t c) {
	unsigned int x,y;
	CCCHAR cc,*page;

	/* if input interrupted scrolling, well then... stop scrolling */
	if (eia->rollup_scroll != 0 && eia->next_line_while_rollup == 0) {
		eia->rollup_scroll = 0;
		eia->_f_wait_for_rollup = 0;
		if (eia->debug_rollup_interrupt) eia608_decoder_debug(eia,"Next char '%c' too soon, interrupting rollup scroll",c);
		eia608_decoder_scrollup_immediate(eia,eia->rollup_row,eia->rollup_row+eia->rollup_rowcount);
		if (eia->on_update_screen != NULL) eia->on_update_screen(eia,0,0,CC_COLUMNS-1,CC_ROWS-1);
	}

	if (eia->present_mode == CC_PRESENT_DEBUG) {
		if (eia->debug_show_chars)
			eia608_decoder_debug(eia,"CC decoder in debug state, got char (overwrite+prev) '%c'",c);

		return;
	}

	/* NTS: If told to, clip the cursor such that writing an extended char on the 31st column
	 *      is impossible (wrapping it over the 30th column) to mimic what most TV sets seem
	 *      to do */
	page = eia->current_page;
	cc = eia->current_attr;
	x = eia->cursor_x;
	if (x >= 31 && eia->clip_overwrite_on_31st_col) x = 30;
	else if (x > 0) x--;
	else eia608_decoder_debug(eia,"CC decoder received overwrite+prev char when cursor at x=0");
	y = eia->cursor_y;
	cc.ch = c;

	/* roll-up mode: if in the middle of roll-up animation write to the NEXT scan line */
	if (eia->present_mode == CC_PRESENT_ROLLUP && eia->rollup_scroll != 0)
		y++;

	/* pop-on mode: render to offscreen page */
	if (eia->present_mode == CC_PRESENT_POPON)
		page = eia->next_page;

	/* place the char */
	assert(y < CC_ROWS);
	assert(x < CC_DISPLAY_COLUMNS);
	page[(y*CC_COLUMNS)+x+CC_X_OFFSET] = cc;
	if (page == eia->current_page) {
		eia->updated = 1;
		if (eia->on_update_screen != NULL) eia->on_update_screen(eia,x+CC_X_OFFSET,y,x+CC_X_OFFSET,y);
	}
}

/* straight char entry */
void eia608_decoder_take_char(eia608_decoder *eia,uint8_t c) {
	unsigned int x,y;
	CCCHAR cc,*page;

	/* if input interrupted scrolling, well then... stop scrolling */
	if (eia->rollup_scroll != 0 && eia->next_line_while_rollup == 0) {
		eia->rollup_scroll = 0;
		eia->_f_wait_for_rollup = 0;
		if (eia->debug_rollup_interrupt) eia608_decoder_debug(eia,"Next char '%c' too soon, interrupting rollup scroll",c);
		eia608_decoder_scrollup_immediate(eia,eia->rollup_row,eia->rollup_row+eia->rollup_rowcount);
		if (eia->on_update_screen != NULL) eia->on_update_screen(eia,0,0,CC_COLUMNS-1,CC_ROWS-1);
	}

	if (eia->present_mode == CC_PRESENT_DEBUG) {
		if (eia->debug_show_chars)
			eia608_decoder_debug(eia,"CC decoder in debug state, got char '%c'",c);

		return;
	}

	if (eia->cursor_x == CC_DISPLAY_COLUMNS) {
		if (eia->autowrap)
			eia608_decoder_carriage_return(eia);
		else
			eia608_decoder_debug(eia,"Warning, unexpected column overrun");
	}
	if (eia->cursor_x == CC_DISPLAY_COLUMNS)
		eia->cursor_x = CC_DISPLAY_COLUMNS - 1;

	page = eia->current_page;
	cc = eia->current_attr;
	x = eia->cursor_x;
	y = eia->cursor_y;
	cc.ch = c;

	/* roll-up mode: if in the middle of roll-up animation write to the NEXT scan line */
	if (eia->present_mode == CC_PRESENT_ROLLUP && eia->rollup_scroll != 0)
		y++;

	/* pop-on mode: render to offscreen page */
	if (eia->present_mode == CC_PRESENT_POPON)
		page = eia->next_page;

	/* place the char */
	assert(y < CC_ROWS);
	assert(x < CC_DISPLAY_COLUMNS);
	page[(y*CC_COLUMNS)+x+CC_X_OFFSET] = cc;
	if (page == eia->current_page) {
		eia->updated = 1;
		if (eia->on_update_screen != NULL) eia->on_update_screen(eia,x+CC_X_OFFSET,y,x+CC_X_OFFSET,y);
	}

	/* if the left & right cells to this char are empty, then fill them in */
	/* BUGFIX: If we're writing a transparent space, do NOT L/R fill. This fixes
	 *         bug #651 where, given certain files such as Who Framed Roger Rabbit,
	 *         the caption would show up as a lone black block followed by 2-3 spaces
	 *         and then the main caption. */
	if (eia->l_r_fill && cc.ch != 0xF9/*Special char TSP Transparent space*/) {
		if (page[(y*CC_COLUMNS)+x+CC_X_OFFSET-1].ch == 0 || page[(y*CC_COLUMNS)+x+CC_X_OFFSET-1].ch == 0xF9) {
			cc.ch = ' ';
			page[(y*CC_COLUMNS)+x+CC_X_OFFSET-1] = cc;
			if (page == eia->current_page) {
				eia->updated = 1;
				if (eia->on_update_screen != NULL) eia->on_update_screen(eia,x+CC_X_OFFSET-1,y,x+CC_X_OFFSET-1,y);
			}
		}
		if (page[(y*CC_COLUMNS)+x+CC_X_OFFSET+1].ch == 0 || page[(y*CC_COLUMNS)+x+CC_X_OFFSET-1].ch == 0xF9) {
			cc.ch = ' ';
			page[(y*CC_COLUMNS)+x+CC_X_OFFSET+1] = cc;
			if (page == eia->current_page) {
				eia->updated = 1;
				if (eia->on_update_screen != NULL) eia->on_update_screen(eia,x+CC_X_OFFSET+1,y,x+CC_X_OFFSET+1,y);
			}
		}
	}

	if (eia->cursor_x < CC_DISPLAY_COLUMNS)
		eia->cursor_x++;
}

/* take CCCHAR value and convert to unicode charset */
int eia608_decoder_cchar_to_unicode(uint8_t cc) {
	if (cc < 0x20) return -1; /* not yet defined*/

	/* EIA-608 replacements of some ASCII characters */
	switch (cc) {
		case 0x2A:	return 0x00E1;	/* á */
		case 0x5C:	return 0x00E9;	/* é */
		case 0x5E:	return 0x00ED;	/* í */
		case 0x5F:	return 0x00F3;	/* ó */
		case 0x60:	return 0x00FA;	/* ú */
		case 0x7B:	return 0x00C7;	/* Ç */
		case 0x7C:	return 0x00F7;	/* ÷ */
		case 0x7D:	return 0x00D1;	/* Ñ */
		case 0x7E:	return 0x00F1;	/* ñ */
		case 0x7F:	return 0x25A0;	/* ■ */

		/* extended charset, according to our translation (spanish/french) */
		case 0xB0:	return 0x00C3;	/* Ã */
		case 0xB1:	return 0x00E3;	/* ã */
		case 0xB2:	return 0x00CD;	/* Í */
		case 0xB3:	return 0x00CC;	/* Ì */
		case 0xB4:	return 0x00EC;	/* ì */
		case 0xB5:	return 0x00D2;	/* Ò */
		case 0xB6:	return 0x00F2;	/* ò */
		case 0xB7:	return 0x00D5;	/* Õ */
		case 0xB8:	return 0x00F5;	/* õ */
		case 0xB9:	return '{';	/* { */
		case 0xBA:	return '}';	/* } */
		case 0xBB:	return '\\';	/* \ */
		case 0xBC:	return '^';	/* ^ */
		case 0xBD:	return '_';	/* _ */
		case 0xBE:	return '|';	/* | */
		case 0xBF:	return '~';	/* ~ */
		case 0xC0:	return 0x00C4;	/* Ä */
		case 0xC1:	return 0x00E4;	/* ä */
		case 0xC2:	return 0x00D6;	/* Ö */
		case 0xC3:	return 0x00F6;	/* ö */
		case 0xC4:	return 0x00DF;	/* ß */
		case 0xC5:	return 0x00A5;	/* ¥ */
		case 0xC6:	return 0x00A4;	/* ¤ */
		case 0xC7:	return 0x00A6;	/* ¦ */
		case 0xC8:	return 0x00C5;	/* Å */
		case 0xC9:	return 0x00E5;	/* å */
		case 0xCA:	return 0x00D8;	/* Ø */
		case 0xCB:	return 0x00F8;	/* ø */
		case 0xCC:	return 0x250C;	/* ┌ */
		case 0xCD:	return 0x2510;	/* ┐ */
		case 0xCE:	return 0x2514;	/* └ */
		case 0xCF:	return 0x2518;	/* ┘ */

		/* extended charset, according to our translation (spanish/french) */
		case 0xD0:	return 0x00C1;	/* Á */
		case 0xD1:	return 0x00C9;	/* É */
		case 0xD2:	return 0x00D3;	/* Ó */
		case 0xD3:	return 0x00DA;	/* Ú */
		case 0xD4:	return 0x00DC;	/* Ü */
		case 0xD5:	return 0x00FC;	/* ü */
		case 0xD6:	return 0x00B4;	/* ´ */
		case 0xD7:	return 0x00A1;	/* ¡ */
		case 0xD8:	return '*';	/* * */
		case 0xD9:	return '\'';	/* ' */
		case 0xDA:	return 0x2014;	/* — */
		case 0xDB:	return 0x00A9;	/* © */
		case 0xDC:	return 0x2120;	/* ℠ */
		case 0xDD:	return 0x00B7;	/* · */
		case 0xDE:	return 0x201C;	/* “ */
		case 0xDF:	return 0x201D;	/* ” */
		case 0xE0:	return 0x00C0;	/* À */
		case 0xE1:	return 0x00C2;	/* Â */
		case 0xE2:	return 0x00C7;	/* Ç */
		case 0xE3:	return 0x00C8;	/* È */
		case 0xE4:	return 0x00CA;	/* Ê */
		case 0xE5:	return 0x00CB;	/* Ë */
		case 0xE6:	return 0x00EB;	/* ë */
		case 0xE7:	return 0x00CE;	/* Î */
		case 0xE8:	return 0x00CF;	/* Ï */
		case 0xE9:	return 0x00EF;	/* ï */
		case 0xEA:	return 0x00D4;	/* Ô */
		case 0xEB:	return 0x00D9;	/* Ù */
		case 0xEC:	return 0x00F9;	/* ù */
		case 0xED:	return 0x00DB;	/* Û */
		case 0xEE:	return 0x00AB;	/* « */
		case 0xEF:	return 0x00BB;	/* » */

		/* extended charset, according to our translation (special) */
		case 0xF0:	return 0x00AE;	/* ® */
		case 0xF1:	return 0x00B0;	/* ° */
		case 0xF2:	return 0x00BD;	/* ½ */
		case 0xF3:	return 0x00BF;	/* ¿ */
		case 0xF4:	return 0x2122;	/* ™ */
		case 0xF5:	return 0x00A2;	/* ¢ */
		case 0xF6:	return 0x00A3;	/* £ */
		case 0xF7:	return 0x266A;	/* ♪ */
		case 0xF8:	return 0x00E0;	/* à */
		case 0xF9:	return ' ';	/* (TS) */
		case 0xFA:	return 0x00E8;	/* è */
		case 0xFB:	return 0x00E2;	/* â */
		case 0xFC:	return 0x00EA;	/* ê */
		case 0xFD:	return 0x00EE;	/* î */
		case 0xFE:	return 0x00F4;	/* ô */
		case 0xFF:	return 0x00FB;	/* û */
	};

	if (cc > 0x7F) return -1; /* not yet defined*/
	return (int)cc;
}

uint8_t *eia608_get_font_8x13_bmp(eia608_decoder __attribute__((unused)) *eia,CCCHAR *cch) {
	return eia608_8x14_font+((unsigned int)cch->ch * 14)+1;
}

uint8_t eia608_decoder_midrow_code_to_row(eia608_decoder __attribute__((unused)) *eia,uint16_t word) {
	/* NTS: The table is typed out in this manner because the FCC doc lists the rows as 1-based */
	static uint8_t map[16] = {
		/* 0x1040 */ 11,
		/* 0x1060 */ 0, /* invalid row */
		/* 0x1140 */ 1,
		/* 0x1160 */ 2,

		/* 0x1240 */ 3,
		/* 0x1260 */ 4,
		/* 0x1340 */ 12,
		/* 0x1360 */ 13,

		/* 0x1440 */ 14,
		/* 0x1460 */ 15,
		/* 0x1540 */ 5,
		/* 0x1560 */ 6,

		/* 0x1640 */ 7,
		/* 0x1660 */ 8,
		/* 0x1740 */ 9,
		/* 0x1760 */ 10
	};
	return map[((word>>7)&0xE)+((word>>5)&1)];
}

const uint8_t eia608_std_colors[8*3] = {
	192,192,192,		/* white */
	0,  192,0,              /* green */
	0,  0,  192,		/* blue */
	0,  192,192,            /* cyan */

	192,0,  0,		/* red */
	192,192,0,		/* yellow */
	192,0,  192,		/* magenta */
	192,192,192		/* white [italic] */
};

const uint8_t eia608_std_black[3] = {
	0,  0,  0
};

const uint8_t *eia608_decoder_get_color(eia608_decoder __attribute__((unused)) *eia,uint8_t color) {
	return eia608_std_colors+((color&7)*3);
}

const uint8_t *eia608_decoder_get_background_color(eia608_decoder __attribute__((unused)) *eia,uint8_t color) {
	if ((color&7) == 7) return eia608_std_black;
	return eia608_std_colors+((color&7)*3);
}

void eia608_decoder_take_ctrl_pac(eia608_decoder *eia,uint16_t cc) {
	if ((cc&0xFF00) == 0x1000 && (cc&0xFF) > 0x5F)
		return;

	if (eia->present_mode == CC_PRESENT_DEBUG)
		return;

	if (eia->present_mode == CC_PRESENT_ROLLUP) {
		uint8_t nrow = eia608_decoder_midrow_code_to_row(eia,cc);

		if (nrow > 0) nrow--; /* row is 1-based */
		if (nrow < (eia->rollup_rowcount-1)) nrow = eia->rollup_rowcount-1;

		if (nrow != eia->cursor_y) {
			eia608_decoder_debug(eia,"Roll-up mode: base row changed %u to %u with %u row",eia->cursor_y,nrow,eia->rollup_rowcount);
			eia608_decoder_erase_displayed_memory(eia); /* FIXME: We're supposed to preserve the roll-up if it merely moved */
			eia608_decoder_roll_up(eia,eia->rollup_rowcount,nrow);
		}
	}
	else {
		eia->cursor_y = eia608_decoder_midrow_code_to_row(eia,cc); /* <- return value is 1-based */
		if (eia->cursor_y > 0) eia->cursor_y--; /* convert to 0-based */
	}

	eia->current_attr.background_transparent = 0;
	eia->current_attr.background_color = 7; /* black */
	eia->current_attr.underline = cc & 1;
	eia->current_attr.italic = 0;
	eia->current_attr.color = 0; /* white */

	if ((cc&0x10) != 0) {
		uint8_t nx;

		nx = ((cc >> 1) & 7) * 4;

		/* HACK: Some CC test sources do not use the "Carriage return" code,
		 *       but simply start the line again. If we don't take these into
		 *       consideration we'll end up with a 1-line rollup overwriting
		 *       itself again and again */
		if (eia->present_mode == CC_PRESENT_ROLLUP && nx < eia->cursor_x) {
			eia608_decoder_debug(eia,"Roll-up mode: cursor moved backwards. It's possible the captioneer did not insert Carriage Return commands.");
			eia608_decoder_carriage_return(eia);
		}
		eia->cursor_x = nx;
	}
	else {
		if ((cc & 0xE) == 0xE)
			eia->current_attr.italic = 1;
		else
			eia->current_attr.color = (cc & 0xE) >> 1;

		eia->cursor_x = 0;
	}
}

/* move the console up one, and clear the lowest line. caller must initiate redraw */
void eia608_decoder_scrollup_immediate(eia608_decoder *eia,unsigned int start,unsigned int end) {
	if (start > end) return;
	assert(start < CC_ROWS);
	assert(end < CC_ROWS);

	eia->rollup_slow_cnt = eia->rollup_slow_div;
	memmove(eia->current_page+(start*CC_COLUMNS),eia->current_page+((start+1)*CC_COLUMNS),(end - start) * CC_COLUMNS * sizeof(CCCHAR));
	memset(eia->current_page+(end*CC_COLUMNS),0,CC_COLUMNS * sizeof(CCCHAR));
}

void eia608_decoder_carriage_return(eia608_decoder *eia) {
	if (eia->present_mode == CC_PRESENT_ROLLUP) {
		if (eia->rollup_scroll != 0) {
			eia->updated = 1;
			eia->rollup_scroll = 0;
			eia->_f_wait_for_rollup = 1;
			eia608_decoder_scrollup_immediate(eia,eia->rollup_row,eia->rollup_row+eia->rollup_rowcount);
			if (eia->debug_rollup_interrupt) eia608_decoder_debug(eia,"Carriage return too soon, interrupting rollup scroll");
			if (eia->on_update_screen != NULL) eia->on_update_screen(eia,0,0,CC_COLUMNS-1,CC_ROWS-1);
		}

		/* NTS: we don't actually move the row up until scrolling stops */
		eia->rollup_scroll = eia->rollup_fontheight;
		eia->rollup_slow_cnt = eia->rollup_slow_div;
		if (!eia->next_line_while_rollup) eia->_f_wait_for_rollup = 1;
		eia->cursor_x = 0;
	}
}

void eia608_decoder_resume_direct_captioning(eia608_decoder *eia) {
	if (eia->present_mode != CC_PRESENT_PAINTON) {
		eia608_decoder_erase_displayed_memory(eia);
		eia->present_mode = CC_PRESENT_PAINTON;
		eia->rollup_rowcount = 0;
		eia->rollup_row = 14;
		eia->cursor_x = 0;
		eia->cursor_y = 0;
		eia->autowrap = 0;
		eia608_decoder_debug(eia,"Entering paint-on mode");
	}
}

void eia608_decoder_resume_text_display(eia608_decoder *eia) {
	if (eia->present_mode == CC_PRESENT_DEBUG || eia->present_mode == CC_PRESENT_POPON) {
		eia->present_mode = CC_PRESENT_ROLLUP;
		eia->rollup_rowcount = 15;
		eia->rollup_row = 14 + 1 - 15;
		eia->cursor_x = 0;
		eia->cursor_y = 14;
		eia->autowrap = 1;
		eia608_decoder_debug(eia,"Entering text channel mode");
	}
}

void eia608_decoder_delete_to_end_of_row(eia608_decoder *eia) {
	CCCHAR *buf = eia->current_page;
	unsigned char y;

	if (eia->present_mode == CC_PRESENT_POPON)
		buf = eia->next_page;
	if (eia->cursor_x >= CC_DISPLAY_COLUMNS)
		return;

	/* roll-up mode: if in the middle of roll-up animation write to the NEXT scan line */
	y = eia->cursor_y;
	if (eia->present_mode == CC_PRESENT_ROLLUP && eia->rollup_scroll != 0)
		y++;

	assert(y < CC_ROWS);
	buf += (y * CC_COLUMNS);
	memset(buf+eia->cursor_x+CC_X_OFFSET,0,sizeof(CCCHAR)*(CC_COLUMNS-(eia->cursor_x+CC_X_OFFSET)));

	eia->updated = 1;
	if (eia->on_update_screen != NULL && buf == eia->current_page)
		eia->on_update_screen(eia,eia->cursor_x+CC_X_OFFSET,
			eia->cursor_y,CC_COLUMNS-1,eia->cursor_y);
}

void eia608_decoder_backspace(eia608_decoder *eia) {
	if (eia->present_mode == CC_PRESENT_ROLLUP) {
		if (eia->cursor_x > 0) {
			CCCHAR *page = eia->current_page;
			unsigned char y = eia->cursor_y;

			/* roll-up mode: if in the middle of roll-up animation write to the NEXT scan line */
			if (eia->present_mode == CC_PRESENT_ROLLUP && eia->rollup_scroll != 0)
				y++;

			/* pop-on mode: render to offscreen page */
			if (eia->present_mode == CC_PRESENT_POPON)
				page = eia->next_page;

			assert(y < CC_ROWS);
			if (eia->cursor_x < CC_DISPLAY_COLUMNS) {
				if (page[(y*CC_COLUMNS)+eia->cursor_x+CC_X_OFFSET].ch != 0) {
					eia->current_attr.ch = 0;
					page[(y*CC_COLUMNS)+eia->cursor_x+CC_X_OFFSET] = eia->current_attr;
				}
			}

			eia->cursor_x--;
			if (eia->cursor_x < CC_DISPLAY_COLUMNS) {
				if (page[(y*CC_COLUMNS)+eia->cursor_x+CC_X_OFFSET].ch != 0) {
					eia->current_attr.ch = 0;
					page[(y*CC_COLUMNS)+eia->cursor_x+CC_X_OFFSET] = eia->current_attr;
				}
			}

			if (eia->cursor_x < CC_DISPLAY_COLUMNS) {
				eia->updated = 1;
				if (eia->on_update_screen != NULL && page == eia->current_page)
					eia->on_update_screen(eia,
						eia->cursor_x+CC_X_OFFSET,eia->cursor_y,
						eia->cursor_x+CC_X_OFFSET+1,eia->cursor_y);
			}
		}
	}
}

void eia608_decoder_roll_up(eia608_decoder *eia,uint8_t rows,uint8_t base) {
	if (rows == 0 || eia == NULL) return;

	if (base == 0xFF) base = eia->rollup_row + eia->rollup_rowcount - 1;
	if (base < (rows - 1)) base = rows - 1;
	if (eia->present_mode != CC_PRESENT_ROLLUP ||
		(eia->present_mode == CC_PRESENT_ROLLUP && (eia->rollup_rowcount != rows || eia->cursor_y != base))) {
		if (eia->present_mode != CC_PRESENT_ROLLUP)
			eia608_decoder_erase_displayed_memory(eia);

		eia->present_mode = CC_PRESENT_ROLLUP;
		eia->rollup_rowcount = rows;
		eia->rollup_row = base + 1 - rows;
		eia->cursor_x = 0;
		eia->cursor_y = base;
		eia->autowrap = 0; /* FIXME: Is this right? */
		eia608_decoder_debug(eia,"Entering roll-up mode (%u rows at %u)",rows,base);
	}
}

void eia608_decoder_take_misc_ctrl(eia608_decoder *eia,uint16_t cc) {
	switch (cc&0xFF) {
		case 0x20: /* RCL Resume caption loading */
			eia608_decoder_switch_popon_presentation(eia);
			break;

		case 0x21: /* BS Backspace */
			eia608_decoder_backspace(eia);
			break;

		case 0x22: /* AOF Alarm off (??), aka reserved. */
			break;

		case 0x24: /* DER Delete to End of Row */
			eia608_decoder_delete_to_end_of_row(eia);
			break;

		case 0x25: /* RU2 Roll-up 2 rows */
		case 0x26: /* RU3 Roll-up 3 rows */
		case 0x27: /* RU4 Roll-up 4 rows */
			eia608_decoder_roll_up(eia,((cc&0xFF)-0x25)+2,0xFF/*default*/);
			break;

		case 0x29: /* RDC Resume Direct Captioning */
			eia608_decoder_resume_direct_captioning(eia);
			break;

		case 0x2B: /* RTD Resume Text Display */
			eia608_decoder_resume_text_display(eia);
			break;

		case 0x2C: /* EDM Erase Displayed Memory */
			eia608_decoder_erase_displayed_memory(eia);
			break;

		case 0x2D: /* CR Carriage Return */
			eia608_decoder_carriage_return(eia);
			break;

		case 0x2E: /* ENM Erase Non-Displayed Memory */
			eia608_decoder_erase_non_displayed_memory(eia);
			break;

		case 0x2F: /* EOC End of Caption */
			eia608_decoder_end_of_caption(eia);
			break;

		default:
			fprintf(stderr,"Unknown misc control %04x\n",cc);
			break;
	};
}

void eia608_decoder_take_misc_ctrl15(eia608_decoder *eia,uint16_t cc) {
	switch (cc&0xFF) {
		case 0x2C: /* EDM Erase Displayed Memory */
			eia608_decoder_erase_displayed_memory(eia);
			break;

		default:
			fprintf(stderr,"Unknown misc15 control %04x\n",cc);
			break;
	};
}

void eia608_decoder_take_controlword(eia608_decoder *eia,uint16_t cc) {
	if ((cc&0x40) != 0) {
		eia608_decoder_take_ctrl_pac(eia,cc);
		return;
	}
	else if ((cc&0xFFF0) == 0x1020) { /* background attribute */
		eia->current_attr.background_transparent = cc & 1;
		eia->current_attr.background_color = (cc >> 1) & 7;

		/* emulate observed behavior on an LG TV set that disallows a white background */
		if (!eia->allow_white_background && eia->current_attr.background_color == 0)
			eia->current_attr.background_color = 7; /* change to black */
		return;
	}

	switch (cc>>8) {
		case 0x14: /* misc */
			eia608_decoder_take_misc_ctrl(eia,cc);
			break;

		case 0x15: /* misc?? */
			eia608_decoder_take_misc_ctrl15(eia,cc);
			break;

		case 0x17: /* tab offsets */
			if (eia->present_mode != CC_PRESENT_DEBUG && eia->present_mode != CC_PRESENT_ROLLUP) {
				if ((cc&0xFF) >= 0x21 && (cc&0xFF) <= 0x23) eia->cursor_x += cc&3;
				if (eia->cursor_x > CC_DISPLAY_COLUMNS) eia->cursor_x = CC_DISPLAY_COLUMNS;
			}
			break;

		default:
			fprintf(stderr,"Unhandled midrow code %04x\n",cc);
			break;
	}
}

void eia608_decoder_take_word(eia608_decoder *eia,uint16_t cc) {
	cc &= 0x7F7F; /* remove parity bits */
	if (cc == 0) {
		if (eia->timeout_data_counter > 0) {
			if (--eia->timeout_data_counter == 0) {
				/* data timeout. clear the CC memory */
				eia608_decoder_debug(eia,"data timeout. clearing CC window");
				eia608_decoder_erase_displayed_memory(eia);
			}
		}

		return;
	}

	/* reset timeout counter, we got data */
	eia->timeout_data_counter = eia->timeout_data;

	if ((cc&0x6000) != 0) { /* plain text */
		if (!eia->xds_input) { /* ignore XDS data */
			if (eia->delayed_char != 0) {
				eia608_decoder_take_char(eia,eia->delayed_char);
				eia->delayed_char = 0;
			}

			eia608_decoder_take_char(eia,cc>>8);
			if ((cc&0xFF) != 0) {
				if (eia608_decoder_can_accept_data(eia,0))
					eia608_decoder_take_char(eia,cc&0xFF);
				else
					eia->delayed_char = cc&0xFF;
			}

		}

		eia->last_122x = cc;
		eia->last_data = cc;
		return;
	}

	if (eia->xds_input && (cc&0x7F00) == 0x0F00) { /* end of XDS data with checksum */
		eia->xds_input = 0;
		return;
	}
	else if ((cc&0x7070) == 0 && (cc&0x0F0F) != 0) { /* [P000 CLASS] [P000 TYPE] eXtended Data Services and class and type nonzero */
		/* NTS: We don't decode XDS. We only pay attention to XDS data
		 *      so that we do not confuse it with CC or TEXT data.
		 *      If the caller wants to read data from XDS it must use
		 *      the extended-data-decoder.c library functions to assemble
		 *      and read the packets. */
		eia->xds_input = 1;
		return;
	}

	if ((cc&0x7F00) == 0x0F00)
		return;

	cc &= ~0x0800; /* filter out channel bit */
	if ((cc&0xFF60) == 0x1220) { /* extended western charset */
		eia->xds_input = 0;

		/* ewww... this convention is even more bizarre:
		 * When you use this charset you're expected to first transmit using
		 * the basic charset a facimile of the char, THEN you transmit the
		 * extended charset code, which overwrites the char cell just before
		 * the cursor instead of typing it out as normal. */
		/* NTS: The EIA-608 bitmap font maps 0x1220-0x123F to 0xD0-0xEF */
		eia608_decoder_take_char_overwrite_prev(eia,(cc&0x1F)+0xD0);
	}
	else if ((cc&0xFF60) == 0x1320) { /* extended western charset */
		eia->xds_input = 0;

		/* ewww... this convention is even more bizarre:
		 * When you use this charset you're expected to first transmit using
		 * the basic charset a facimile of the char, THEN you transmit the
		 * extended charset code, which overwrites the char cell just before
		 * the cursor instead of typing it out as normal. */
		/* NTS: The EIA-608 bitmap font maps 0x1220-0x123F to 0xB0-0xCF */
		eia608_decoder_take_char_overwrite_prev(eia,(cc&0x1F)+0xB0);
	}
	else if ((cc&0xFFF0) == 0x1120) { /* color/italics */
		eia->xds_input = 0;

		if (cc != eia->last_data) {
			/* NTS: The convention (according to CC test data I have) is that
			 *      a color/italics change automatically causes the insertion
			 *      of a space. Without this, normal-italic-normal changes in
			 *      closed captioning show up as words jumbled together */
			/* NTS: Barney and friends VOB -- Apparently according to reference
			 *      you can set a color and then set italics */
			/* NTS: M*A*S*H FOX special -- we have to reset italics on color
			 *      change, or else the per-word emphasis they use makes the
			 *      whole line italicized */
			if (eia->space_on_format_change) {
				/* NTS: Advance the cursor, do not write block. */
				if (eia->cursor_x < CC_DISPLAY_COLUMNS)
					eia->cursor_x++;
			}

			if ((cc & 0xE) == 0xE)
				eia->current_attr.italic = 1;
			else {
				eia->current_attr.color = (cc >> 1) & 7;
				eia->current_attr.italic = 0;
			}

			eia->current_attr.underline = cc & 1;
			eia->last_122x = 0;
		}
	}
	else if ((cc&0xFFF0) == 0x1130) { /* special chars */
		/* NTS: The EIA-608 bitmap font maps special chars to 0xF0-0xFF */
		eia->xds_input = 0;
		if (cc != eia->last_data)
			eia608_decoder_take_char(eia,(cc&0xF)+0xF0);
		else
			cc = 0;
	}
	else if ((cc&0x1000) != 0) {
		eia->xds_input = 0;
		if (cc != eia->last_data || cc == 0x1421/*Backspace*/)
			eia608_decoder_take_controlword(eia,cc);
		else
			cc = 0;
	}
	else {
		fprintf(stderr,"Unhandled code %04x\n",cc);
	}

	eia->last_data = cc;
}

int eia608_decoder_init(eia608_decoder *x) {
	x->current_page = malloc(CC_ROWS * CC_COLUMNS * sizeof(CCCHAR));
	if (x->current_page == NULL) return 0;

	x->next_page = malloc(CC_ROWS * CC_COLUMNS * sizeof(CCCHAR));
	if (x->next_page == NULL) {
		eia608_decoder_free(x);
		return 0;
	}

	x->next_line_while_rollup = 0;
	x->timeout_data = 30*10;	/* ~10 seconds at NTSC 29.97 */
	x->timeout_data_counter = 0;
	x->on_update_screen = NULL;
	x->on_rollup_screen = NULL;
	x->debug_rollup_interrupt = 1;
	x->space_on_format_change = 1;
	x->allow_white_background = 0;
	x->clip_overwrite_on_31st_col = 1;
	x->debug_show_chars = 1;
	x->wait_for_scroll = 0;
	x->l_r_fill = 1;
	eia608_decoder_reset(x);
	return 1;
}

eia608_decoder *eia608_decoder_create() {
	eia608_decoder *x = (eia608_decoder*)malloc(sizeof(eia608_decoder));
	if (x == NULL) return NULL;
	if (!eia608_decoder_init(x)) {
		free(x);
		x=NULL;
	}
	return x;
}

eia608_decoder *eia608_decoder_destroy(eia608_decoder *x) {
	if (x) {
		eia608_decoder_free(x);
		free(x);
	}
	return NULL;
}

void eia608_decoder_delay_flush(eia608_decoder *x) {
	x->delay_in = x->delay_out = 0;
}

int eia608_decoder_delay_output(eia608_decoder *x) {
	uint16_t r;

	if (x->delay_in == x->delay_out)
		return -1;

	assert(x->delay_out < EIA608_MAX_DELAY_BUFFER);
	r = (int)x->delay[x->delay_out++];
	if (x->delay_out >= EIA608_MAX_DELAY_BUFFER)
		x->delay_out = 0;

	return (int)r;
}

void eia608_decoder_delay_input(eia608_decoder *x,uint16_t word) {
	int val;

	if (((x->delay_in+1)%EIA608_MAX_DELAY_BUFFER) == x->delay_out) {
		eia608_decoder_debug(x,"warning, delay overflow");

		/* at this point, feed data into the decoder whether it's ready or not.
		 * at worst, this will interrupt the rollup animation */
		if ((val=eia608_decoder_delay_output(x)) >= 0)
			eia608_decoder_take_word(x,(uint16_t)val);
		else {
			eia608_decoder_debug(x,"...delay overflow compensation: unable to get delayed output??");
			return;
		}
	}

	assert(x->delay_in < EIA608_MAX_DELAY_BUFFER);
	x->delay[x->delay_in++] = word;
	if (x->delay_in >= EIA608_MAX_DELAY_BUFFER)
		x->delay_in = 0;
}

int eia608_decoder_delay_overflow(eia608_decoder *x) {
	return (((x->delay_in+1)%EIA608_MAX_DELAY_BUFFER) == x->delay_out);
}

void eia608_decoder_take_word_with_delay(eia608_decoder *x,uint16_t w) {
	int val;

	eia608_decoder_delay_input(x,w);
	while ((eia608_decoder_can_accept_data(x,w) || eia608_decoder_delay_overflow(x)) && (val=eia608_decoder_delay_output(x)) >= 0)
		eia608_decoder_take_word(x,(uint16_t)val);
}

int eia608_decoder_cchar_is_transparent(uint8_t cc) {
	if (cc == 0x00/*nothing*/ || cc == 0xF9/*Transparent Space*/) return 1;
	return 0;
}

uint16_t eia608_unicode_to_code(unsigned int uc/*unicode char*/) {
	if (uc == 0x266A/* music note (♪) */)
		return 0x1130 + 7; /* special char 7 */
	/* TODO: special chars, latin, etc. */

	if (uc >= 0x20 && uc <= 0x7F)
		return (uint16_t)uc;

	return 0x0000; /* unknown */
}

uint16_t eia608_row_to_rowcode(int y) {
	if (y <= 0) y = 1;
	else if (y > 15) y = 15;
	return eia608_row_to_rowcode_val[y];
}

