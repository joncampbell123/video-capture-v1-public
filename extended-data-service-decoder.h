
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* XDS data assembly (yuck) */
#define XDS_MAX_LENGTH		256
/* maximum simultaneous streams we can assemble at once */
#define XDS_MAX_SIMUL_STREAMS	8

typedef struct xds_data_stream {
	unsigned char		typeno;
	unsigned char		classno;
	unsigned char		data[XDS_MAX_LENGTH];
	size_t			data_length;
} xds_data_stream;

typedef struct xds_data_assembly {
	xds_data_stream		assembly[XDS_MAX_SIMUL_STREAMS];
	unsigned char		current_class;
	unsigned char		current_type;
	/* debug */
	unsigned int		debug_warn_overrun:1;
	unsigned int		debug_unexpected_end:1;
	unsigned int		debug_checksum_failure:1;
	unsigned int		_debug_reserved_:5;
	/* callback */
	void			(*on_xds_packet)(void *ctx,xds_data_stream *s);
} __attribute__ ((__packed__)) xds_data_assembly;

xds_data_assembly *xds_data_assembly_create();
xds_data_assembly *xds_data_assembly_destroy(xds_data_assembly *x);
xds_data_stream *xds_data_assembly_get_stream(xds_data_assembly *x,unsigned char classno,unsigned char typeno);
void xds_data_assembly_debug(xds_data_assembly *x,const char *fmt,...);
xds_data_stream *xds_data_assembly_new_stream(xds_data_assembly *x,unsigned char classno,unsigned char typeno);
xds_data_stream *xds_data_assembly_get_current_stream(xds_data_assembly *x);
void xds_data_assembly_stream_add_word(xds_data_stream *s,uint16_t cc);
unsigned int xds_data_assembly_stream_checksum(xds_data_stream *s);
void xds_data_assembly_take_eia608_word(xds_data_assembly *x,uint16_t cc);

#ifdef __cplusplus
}
#endif

