#ifndef ___AVIREADER_H
#define ___AVIREADER_H

#include <isp-utils-v4/avi/avi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct avi_reader_stream {
	riff_chunk			strh_chunk,strf_chunk;
	riff_chunk			vprp_chunk,indx_chunk;
	riff_strh_AVISTREAMHEADER	strh;
} avi_reader_stream;

typedef struct avi_reader_stream_index1 {
	int				alloc;
	int				count;
	riff_idx1_AVIOLDINDEX*		map;
} avi_reader_stream_index1;

typedef struct avi_reader_stream_odml_index_entry {
	int64_t				offset;
	uint32_t			size;		/* WARNING: bit 31 is used to indicate "non-keyframe" status */
} avi_reader_stream_odml_index_entry;

typedef struct avi_reader_stream_odml_index {
	int					alloc;
	int					count;
	uint32_t				dwChunkId;
	avi_reader_stream_odml_index_entry*	map;
} avi_reader_stream_odml_index;

#define AVI_ODML_INDX_SIZE(x)		((x) & 0x7FFFFFFFUL)
#define AVI_ODML_INDX_NONKEY(x)		((x) >> 31U)

typedef struct avi_reader {
	riff_stack*			stack;
	int				stack_is_ext;
	riff_chunk			chunk;
	riff_chunk			riff_avi_chunk;
	riff_chunk			movi_chunk;
	riff_chunk			idx1_chunk;
	riff_chunk			hdrl_chunk;
	riff_avih_AVIMAINHEADER 	avi_main_header;
	int				avi_streams;
	avi_reader_stream*		avi_stream;
	avi_reader_stream_index1*	avi_stream_index1;
	avi_reader_stream_odml_index*	avi_stream_odml_index;
} avi_reader;

riff_idx1_AVIOLDINDEX *avi_reader_stream_index1_add(avi_reader_stream_index1 *a);
void avi_reader_stream_odml_index_clear(avi_reader_stream_odml_index *a);
void avi_reader_stream_index1_clear(avi_reader_stream_index1 *a);
int avi_reader_external_stack(avi_reader *r,riff_stack *es);
int avi_reader_internal_stack(avi_reader *r);
void avi_reader_fd(avi_reader *r,int fd);
avi_reader *avi_reader_create();
avi_reader *avi_reader_destroy(avi_reader *a);
int avi_reader_scan_odml_index(avi_reader *a);
int avi_reader_has_odml_index(avi_reader *a);
int avi_reader_has_old_index(avi_reader *a);
int avi_reader_has_an_index(avi_reader *a);
int avi_reader_scan_index1(avi_reader *a);
int avi_reader_scan(avi_reader *a);
avi_reader_stream_index1 *avi_reader_get_stream_index1(avi_reader *r,int stream_id);
int avi_reader_stream_index1_max_entry(avi_reader_stream_index1 *i);
riff_idx1_AVIOLDINDEX *avi_reader_stream_index1_get_entry(avi_reader_stream_index1 *i,int index);
int64_t avi_reader_stream_index1_get_entry_offset(avi_reader *avi,riff_idx1_AVIOLDINDEX *e);
avi_reader_stream_odml_index *avi_reader_get_stream_odml_index(avi_reader *r,int stream_id);

#ifdef __cplusplus
}
#endif

#endif
