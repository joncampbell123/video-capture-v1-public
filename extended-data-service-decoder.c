
#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <stdio.h>

#include "extended-data-service-decoder.h"

xds_data_assembly *xds_data_assembly_create() {
	xds_data_assembly *x = (xds_data_assembly*)malloc(sizeof(xds_data_assembly));
	if (x == NULL) return NULL;
	memset(x,0,sizeof(*x));
	x->debug_warn_overrun = 1;
	x->debug_unexpected_end = 1;
	x->debug_checksum_failure = 1;
	return x;
}

xds_data_assembly *xds_data_assembly_destroy(xds_data_assembly *x) {
	if (x) {
		memset(x,0,sizeof(*x));
		free(x);
	}
	return NULL;
}

xds_data_stream *xds_data_assembly_get_stream(xds_data_assembly *x,unsigned char classno,unsigned char typeno) {
	size_t i;

	for (i=0;i < XDS_MAX_SIMUL_STREAMS;i++) {
		if (x->assembly[i].classno == classno && x->assembly[i].typeno == typeno)
			return &x->assembly[i];
	}

	return NULL;
}

void xds_data_assembly_debug(xds_data_assembly *x,const char *fmt,...) {
	va_list va;

	fprintf(stderr,"xds_data_assembly(%p) debug: ",(void*)x);
	va_start(va,fmt);
	vfprintf(stderr,fmt,va);
	va_end(va);
	fprintf(stderr,"\n");
}

xds_data_stream *xds_data_assembly_new_stream(xds_data_assembly *x,unsigned char classno,unsigned char typeno) {
	size_t i,fn=~((size_t)0);

	for (i=0;i < XDS_MAX_SIMUL_STREAMS;i++) {
		if (x->assembly[i].classno == classno && x->assembly[i].typeno == typeno)
			return &x->assembly[i];
		else if (fn == ~((size_t)0) && x->assembly[i].classno == 0)
			fn = i;
	}

	/* throw away the oldest stream if need be */
	if (fn == ~((size_t)0)) {
		if (x->debug_warn_overrun) xds_data_assembly_debug(x,"Warning, XDS stream overrun, forgetting oldest");
		fn = (size_t)0;
	}

	x->assembly[fn].classno = classno;
	x->assembly[fn].typeno = typeno;
	x->assembly[fn].data_length = 0;
	return &x->assembly[fn];
}

xds_data_stream *xds_data_assembly_get_current_stream(xds_data_assembly *x) {
	if (x->current_class == 0 || x->current_type == 0) return NULL;
	return xds_data_assembly_get_stream(x,x->current_class,x->current_type);
}

void xds_data_assembly_stream_add_word(xds_data_stream *s,uint16_t cc) {
	if ((s->data_length+2) <= XDS_MAX_LENGTH) {
		s->data[s->data_length++] = (unsigned char)(cc>>8);
		s->data[s->data_length++] = (unsigned char)(cc);
	}
}

unsigned int xds_data_assembly_stream_checksum(xds_data_stream *s) {
	unsigned char sum=0;
	size_t i;

	for (i=0;i < s->data_length;i++) sum += s->data[i]&0x7F;
	return (sum&0x7F);
}

void xds_data_assembly_take_eia608_word(xds_data_assembly *x,uint16_t cc) {
	xds_data_stream *s;

	cc &= 0x7F7F;
	if (cc == 0) return;
	else if ((cc&0x7F00) == 0x0F00) { /* 0x8Fxx end of a packet with checksum */
		if (x->current_class != 0 && x->current_type != 0) {
			unsigned char csum;

			if ((s=xds_data_assembly_get_current_stream(x)) == NULL) {
				if (x->debug_unexpected_end) {
					xds_data_assembly_debug(x,"Unexpected end of XDS packet for (class=%u type=%u)",
						x->current_class,x->current_type);
				}

				return;
			}

			xds_data_assembly_stream_add_word(s,cc);
			if ((csum=xds_data_assembly_stream_checksum(s)) == 0) {
				assert(s->data_length >= 2); s->data_length -= 2;
				if (x->on_xds_packet != NULL) x->on_xds_packet(x,s);
			}
			else {
				if (x->debug_checksum_failure) {
					xds_data_assembly_debug(x,"  checksum failed = %02x",csum);
				}
			}

			s->typeno = 0;
			s->classno = 0;
			s->data_length = 0;
			x->current_type = 0;
			x->current_class = 0;
		}
	}
	else if ((cc&0x7070) == 0) { /* start of another XDS packet (possibly interrupting the previous one) */
		x->current_class = (unsigned char)((cc>>8)&0xF);
		x->current_type = (unsigned char)(cc&0xF);
		if (x->current_class == 0 || x->current_type == 0) {
			x->current_class = 0;
			x->current_type = 0;
			return;
		}

		if ((x->current_class&1) == 1) {
			/* it starts a new packet */
			if ((s=xds_data_assembly_new_stream(x,x->current_class,x->current_type)) == NULL) {
				x->current_class = 0;
				x->current_type = 0;
				return;
			}

			/* reset the data counter */
			s->data_length = 0;
			xds_data_assembly_stream_add_word(s,cc);
		}
		else {
			/* it continues an existing packet */
			x->current_class = ((x->current_class-1)&0xE)+1;
			if ((s=xds_data_assembly_get_current_stream(x)) == NULL) {
				x->current_class = 0;
				x->current_type = 0;
				return;
			}

			assert(s->typeno == x->current_type);
			assert(s->classno == x->current_class);
		}
	}
	else if ((cc&0x6000) != 0) {
		if ((s=xds_data_assembly_get_current_stream(x)) == NULL) return;
		xds_data_assembly_stream_add_word(s,cc);
	}
}

