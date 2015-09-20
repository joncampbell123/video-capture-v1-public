/* AVI reader library.
 * (C) 2008-2015 Jonathan Campbell.
 * Alternate copy for open source videocap project.
 */

#include "rawint.h"
#include "avi_reader.h"
#include "avi_rw_iobuf.h"
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>

avi_reader_stream_odml_index_entry *avi_reader_stream_odml_index_add(avi_reader_stream_odml_index *a) {
	int iindex = 0;

	if (!a)
		return NULL;

	if (a->map == NULL) {
		a->alloc = 512;
		a->count = 0;
		a->map = (avi_reader_stream_odml_index_entry*)malloc(sizeof(avi_reader_stream_odml_index_entry)*a->alloc);
		if (a->map == NULL) {
			a->alloc = 0;
			return NULL;
		}
	}
	else if (a->count >= a->alloc) {
		avi_reader_stream_odml_index_entry *n;
		a->alloc += 512;
		n = (avi_reader_stream_odml_index_entry*)
			realloc(a->map,sizeof(avi_reader_stream_odml_index_entry)*a->alloc);

		if (!n) {
			a->alloc -= 512;
			return NULL;
		}

		a->map = n;
	}

	iindex = a->count++;
	return a->map+iindex;
}

riff_idx1_AVIOLDINDEX *avi_reader_stream_index1_add(avi_reader_stream_index1 *a) {
	int iindex = 0;

	if (!a)
		return NULL;

	if (a->map == NULL) {
		a->alloc = 512;
		a->count = 0;
		a->map = (riff_idx1_AVIOLDINDEX*)malloc(sizeof(riff_idx1_AVIOLDINDEX)*a->alloc);
		if (a->map == NULL) {
			a->alloc = 0;
			return NULL;
		}
	}
	else if (a->count >= a->alloc) {
		riff_idx1_AVIOLDINDEX *n;
		a->alloc += 512;
		n = (riff_idx1_AVIOLDINDEX*)
			realloc(a->map,sizeof(riff_idx1_AVIOLDINDEX)*a->alloc);

		if (!n) {
			a->alloc -= 512;
			return NULL;
		}

		a->map = n;
	}

	iindex = a->count++;
	return a->map+iindex;
}

void avi_reader_stream_odml_index_clear(avi_reader_stream_odml_index *a) {
	if (!a) return;
	if (!a->map) return;
	free(a->map);
	a->map = NULL;
	a->alloc = 0;
	a->count = 0;
}

void avi_reader_stream_index1_clear(avi_reader_stream_index1 *a) {
	if (!a) return;
	if (!a->map) return;
	free(a->map);
	a->map = NULL;
	a->alloc = 0;
	a->count = 0;
}

int avi_reader_external_stack(avi_reader *r,riff_stack *es) {
	if (!r || !es) return 0;
	if (es->fd < 0) return 0;
	if (r->stack && !r->stack_is_ext) r->stack = riff_stack_destroy(r->stack);
	r->stack_is_ext = 1;
	r->stack = es;
	return 1;
}

int avi_reader_internal_stack(avi_reader *r) {
	if (!r) return 0;
	if (r->stack_is_ext) {
		r->stack_is_ext = 0;
		r->stack = riff_stack_create(16);
	}
	return (r->stack != NULL);
}

void avi_reader_fd(avi_reader *r,int fd) {
	riff_stack_assign_fd(r->stack,fd);
}

avi_reader *avi_reader_create() {
	avi_reader *a = (avi_reader*)malloc(sizeof(avi_reader));
	if (!a) return NULL;
	memset(a,0,sizeof(*a));
	if (!(a->stack = riff_stack_create(16))) {
		free(a);
		return NULL;
	}
	return a;
}

avi_reader *avi_reader_destroy(avi_reader *a) {
	if (a) {
		if (a->avi_stream_index1 != NULL) {
			int i;
			for (i=0;i < a->avi_streams;i++)
				avi_reader_stream_index1_clear(a->avi_stream_index1+i);
			free(a->avi_stream_index1);
		}
		if (a->avi_stream_odml_index != NULL) {
			int i;
			for (i=0;i < a->avi_streams;i++)
				avi_reader_stream_odml_index_clear(a->avi_stream_odml_index+i);
			free(a->avi_stream_odml_index);
		}

		if (a->avi_stream) free(a->avi_stream);
		if (a->stack && !a->stack_is_ext) a->stack = riff_stack_destroy(a->stack);
		free(a);
	}
	return NULL;
}

int avi_reader_has_odml_index(avi_reader *a) {
	int i;

	if (!a) return 0;

	for (i=0;i < a->avi_streams;i++) {
		avi_reader_stream *s = a->avi_stream + i;
		if (s->indx_chunk.absolute_data_offset != 0LL) return 1;
	}

	return 0;
}

int avi_reader_has_old_index(avi_reader *a) {
	if (!a) return 0;
	return (a->idx1_chunk.absolute_data_offset != 0LL)?1:0;
}

int avi_reader_has_an_index(avi_reader *a) {
	if (!a) return 0;
	if (avi_reader_has_odml_index(a)) return 1;
	if (avi_reader_has_old_index(a)) return 1;
	return 0;
}

int avi_reader_scan_odml_index(avi_reader *a) {
/* WARNING WARNING: This works perfectly as far as I know... BUT if you see AVI read errors or
 *                  bugs show up suddenly, this code is the FIRST to blame and check against! */
/* 2012/11/20: This is a much-needed optimization: If we continued using the old code that
 *             read 16 bytes at a time, we would get very poor performance loading AVI files.
 *             We increase performance by reading bigger chunks. */
	riff_stack *riff;
	int i,e;

	if (a == NULL) return 0;
	if (a->avi_streams == 0) return 0;
	if (a->avi_stream_odml_index == NULL) return 0;
	if (avi_io_buffer_init(sizeof(riff_indx_AVISTDINDEX_entry)) == NULL) return 0;

	riff = a->stack;
	for (i=0;i < a->avi_streams;i++) {
		riff_chunk ichk;
		riff_indx_AVISUPERINDEX asi;
		riff_indx_AVISUPERINDEX_entry ent;
		unsigned int max_ent,giv_ent;

		avi_reader_stream_odml_index *index = a->avi_stream_odml_index+i;
		avi_reader_stream_odml_index_clear(index);
		ichk = a->avi_stream[i].indx_chunk;
		if (ichk.absolute_data_offset == 0LL) continue;
		if (riff_stack_seek(riff,&ichk,0) != 0LL) continue;
		if ((int)riff_stack_read(riff,&ichk,&asi,sizeof(asi)) < (int)sizeof(asi)) continue;

		if (__le_u16(&asi.wLongsPerEntry) != 4) continue;
		if (asi.bIndexType != riff_indx_type_AVI_INDEX_OF_INDEXES) continue;
		if (asi.bIndexSubType != 0) continue;

		max_ent = (unsigned int)((ichk.data_length - sizeof(asi)) / sizeof(ent));
		giv_ent = __le_u32(&asi.nEntriesInUse);
		if (giv_ent > max_ent) giv_ent = max_ent;
		if (giv_ent == 0U) continue;

		for (e=0;(unsigned int)e < giv_ent;e++) {
			avi_reader_stream_odml_index_entry *ient;
			riff_indx_AVISTDINDEX_entry *stdent;
			riff_indx_AVISTDINDEX stdi;
			size_t index_entries,j;
			uint64_t ent_offset;
			uint32_t ent_size;
			riff_chunk ixchk;
			uint64_t offset;
			uint32_t size;

			if (riff_stack_read(riff,&ichk,&ent,sizeof(ent)) < (int)sizeof(ent)) continue;
			if (ent.qwOffset == 0ULL) continue;
			offset = __le_u64(&ent.qwOffset);
			size = __le_u32(&ent.dwSize);

			riff_chunk_improvise(&ixchk,offset+8,size-8);

			riff_stack_seek(riff,&ixchk,0LL);
			if ((int)riff_stack_read(riff,&ixchk,&stdi,sizeof(stdi)) < (int)sizeof(stdi)) continue;

			if (__le_u16(&stdi.wLongsPerEntry) != 2) continue;
			if (stdi.bIndexType != riff_indx_type_AVI_INDEX_OF_CHUNKS) continue;
			if (stdi.bIndexSubType != 0) continue;

			/* NTS: It was a naive assumption to think we could go by dwDuration for number of
			 *      entries. That happens to work for video.. but fails miserably for audio
			 *      in some AVIs. So we have to go instead by size of the chunk divided by
			 *      sizeof an entry. */
			index->dwChunkId = stdi.dwChunkId;
			avi_io_read = avi_io_readfence = avi_io_buf;
			index_entries = (size_t)(ixchk.data_length - sizeof(stdi)) / sizeof(*stdent);

			/* but: to properly read some other AVIs we must use the nEntriesInUse field of the ODML index header */
			{
				size_t count = __le_u32(&stdi.nEntriesInUse);

				if (count != 0 && index_entries > count)
					index_entries = count;
			}

			for (j=0;j < index_entries;j++) {
				if (avi_io_read >= avi_io_readfence) {
					size_t cnt = index_entries - j;
					size_t crd;

					/* FIXME: any case where dwDuration is large enough for overflow? */
					/* TODO: On an incomplete read, consider using what you DID obtain rather than giving up */
					avi_io_read = avi_io_buf;
					if (cnt > avi_io_elemcount) cnt = avi_io_elemcount;
					crd = (size_t)riff_stack_read(riff,&ixchk,avi_io_read,cnt*sizeof(*stdent));
					if ((ssize_t)crd <= 0) {
						fprintf(stderr,"Read error during AVI index [stdindex stream=%u]\n",(unsigned int)i);
						break;
					}
					crd /= sizeof(*stdent);
					if (crd < cnt) fprintf(stderr,"Warning: Read error during AVI index [stdindex stream=%u want=%u got=%u]\n",
						(unsigned int)i,(unsigned int)cnt,(unsigned int)crd);
					avi_io_readfence = avi_io_read + (crd*sizeof(*stdent));
				}

				stdent = (riff_indx_AVISTDINDEX_entry*)avi_io_read;
				avi_io_read += sizeof(riff_indx_AVISTDINDEX_entry);
				ent_offset = __le_u64(&stdi.qwBaseOffset) + __le_u32(&stdent->dwOffset);
				ent_size = __le_u32(&stdent->dwSize);

				ient = avi_reader_stream_odml_index_add(index);
				if (ient == NULL)
					break;

				ient->offset = ent_offset;
				ient->size = ent_size;
			}
		}
	}

	avi_io_buffer_free();
	return 1;
}

int avi_reader_scan_index1(avi_reader *a) {
	riff_idx1_AVIOLDINDEX *ent;
	riff_stack *riff;
	int i,tabents;
	int count=0;
	char x[2];

	/* unfortunately how exactly the offset is encoded is ambiguous,
	 * Micrsoft didn't clearly document whether the offset is absolute
	 * or relative to movi chunk. Worse, the offset de-facto seems to
	 * be relative to the 'movi' chunk as if the movi chunk never had
	 * the LIST/RIFF DWORD before it, so it's still off.
	 *
	 * Not to mention people on FFMPEG/MPlayer devlists mentioning
	 * +/- 1 byte errors in some AVIs anyway...
	 *
	 * So to make life easier for the calling app, we figure out what
	 * the meaning is and then translate the offset on behalf of the
	 * caller. */
	uint32_t bias=0;

	if (a == NULL) return 0;
	if (a->avi_streams == 0) return 0;
	if (a->avi_stream_index1 == NULL) return 0;
	if (a->idx1_chunk.absolute_data_offset == 0) return 0;
	if (avi_io_buffer_init(16) == NULL) return 0;

	riff = a->stack;
	for (i=0;i < a->avi_streams;i++)
		avi_reader_stream_index1_clear(a->avi_stream_index1+i);

	if (riff_stack_seek(riff,&a->idx1_chunk,0) != 0)
		return 0;

	tabents = a->idx1_chunk.data_length / 16UL;
	while (tabents > 0) {
		if (avi_io_read >= avi_io_readfence) {
			size_t cnt = (size_t)tabents;

			avi_io_read = avi_io_buf;
			if (cnt > avi_io_elemcount) cnt = avi_io_elemcount;
			if ((int)riff_stack_read(riff,&a->idx1_chunk,avi_io_read,cnt*16UL) < (int)(cnt*16UL)) {
				fprintf(stderr,"Read error during AVI index [idx1]\n");
				break;
			}

			avi_io_readfence = avi_io_read + (cnt*16UL);
		}

		ent = (riff_idx1_AVIOLDINDEX*)avi_io_read;
		avi_io_read += 16UL;
		tabents--;

		if (!memcmp(&ent->dwChunkId,"rec ",4)) {
			/* 'rec ' chunks are worthless.
			 * I have no idea why some AVIs have these (Microsoft Video For Windows samples CD-ROM) */
			continue;
		}
		else if (!memcmp(&ent->dwChunkId,"7Fxx",4)) {
			/* what the hell are these entries??? ("MTV AMP" test: AVIOLDINDEX starts with 7Fxx and the end has several of them) */
			continue;
		}

		/* FIXME: use rawint macros to ensure little Endian reading */
		/* avi files have xxyy where xx is a decimal number (stream index) and yy is the type */
		x[0] = (char)(ent->dwChunkId&0xFF);
		x[1] = (char)((ent->dwChunkId>>8)&0xFF);

		/* only note AVI index entries that refer to a stream */
		if (!isdigit(x[0]) && !isdigit(x[1])) {
			/* probably not a valid chunk anyway */
			continue;
		}

		/* figure out offset by looking for the chunk ID */
		if (count == 0) {
			uint32_t ofs = __le_u32(&ent->dwOffset) & (~1U);
			unsigned char buft[16];
			riff_chunk x;

			riff_chunk_improvise(&x,0,2ULL << 30ULL);

			/* is it relative to the 'movi' chunk? */
			riff_stack_seek(riff,&x,ofs);
			riff_stack_read(riff,&x,buft,16);
			if (memcmp(buft,&ent->dwChunkId,4) == 0) {
				/* absolute */
				bias = 0UL;
			}
			else {
				/* hm, try relative to the movi chunk */
				riff_stack_seek(riff,&x,ofs + a->movi_chunk.absolute_header_offset + 8ULL);
				riff_stack_read(riff,&x,buft,16);
				if (memcmp(buft,&ent->dwChunkId,4) == 0) {
					bias = (uint32_t)(a->movi_chunk.absolute_header_offset + 8ULL);
				}
				else {
					/* hm, try relative to the movi chunk without header bias */
					riff_stack_seek(riff,&x,ofs + a->movi_chunk.absolute_data_offset);
					riff_stack_read(riff,&x,buft,16);
					if (memcmp(buft,&ent->dwChunkId,4) == 0) {
						bias = (uint32_t)(a->movi_chunk.absolute_data_offset);
					}
					else {
						fprintf(stderr,"WARNING: AVIOLDINDEX problem, cannot determine whether dwOffset is absolute or relative\n");
						bias = (uint32_t)(a->movi_chunk.absolute_header_offset);
					}
				}
			}
		}

		/* additional note: dwOffset points at the chunk header, not the data */
		__w_le_u32(&ent->dwOffset,(__le_u32(&ent->dwOffset) & (~1U)) + bias + 8UL);

		count++;
		{
			riff_idx1_AVIOLDINDEX *e;
			int stream_id = ((x[0]-'0')*10)+(x[1]-'0');
			if (stream_id >= a->avi_streams) continue;

			if (!(e=avi_reader_stream_index1_add(&a->avi_stream_index1[stream_id])))
				continue;

			memcpy(e,ent,sizeof(*e));
		}
	}

	avi_io_buffer_free();
	return 1;
}

int avi_reader_scan(avi_reader *a) {
	riff_chunk avih_chunk = RIFF_CHUNK_INIT,strl_chunk = RIFF_CHUNK_INIT;
	int avi_strf_index=0;
	riff_stack *riff;

	if (a->avi_stream_index1) {
		int i;
		for (i=0;i < a->avi_streams;i++)
			avi_reader_stream_index1_clear(a->avi_stream_index1+i);
		free(a->avi_stream_index1);
	}
	a->avi_stream_index1 = NULL;

	if (a->avi_stream_odml_index) {
		int i;
		for (i=0;i < a->avi_streams;i++)
			avi_reader_stream_odml_index_clear(a->avi_stream_odml_index+i);
		free(a->avi_stream_odml_index);
	}
	a->avi_stream_odml_index = NULL;

	if (a->avi_stream) free(a->avi_stream);
	a->avi_stream = NULL;
	a->avi_streams = 0;

	riff = a->stack;

	memset(&a->movi_chunk,0,sizeof(a->movi_chunk));
	memset(&a->idx1_chunk,0,sizeof(a->idx1_chunk));
	memset(&a->hdrl_chunk,0,sizeof(a->hdrl_chunk));
	memset(&a->riff_avi_chunk,0,sizeof(a->riff_avi_chunk));
	memset(&a->avi_main_header,0,sizeof(a->avi_main_header));
	(void)riff_stack_empty(a->stack);
	if (riff_stack_seek(riff,NULL,0) != 0) return 0;
	if (riff_stack_readchunk(riff,NULL,&a->chunk) == 0) return 0;
	if (riff_stack_chunk_contains_subchunks(&a->chunk) == 0) return 0;
	if (a->chunk.fourcc != avi_riff_AVI) return 0;
	a->riff_avi_chunk = a->chunk;
	if (riff_stack_seek(riff,&a->riff_avi_chunk,0) != 0) return 0;

	while (riff_stack_readchunk(riff,&a->riff_avi_chunk,&a->chunk)) {
		if (riff_stack_chunk_contains_subchunks(&a->chunk) != 0) {
			if (a->chunk.fourcc == avi_riff_movi) {
				memcpy(&a->movi_chunk,&a->chunk,sizeof(a->chunk));
				/* don't enter */
			}
			else if (a->chunk.fourcc == avi_riff_hdrl) {
				memcpy(&a->hdrl_chunk,&a->chunk,sizeof(a->chunk));
				/* don't enter */
			}
			else {
				/* unknown chunk. don't enter */
			}
		}
		else {
			if (a->chunk.fourcc == avi_riff_idx1) {
				/* AVI 'idx1' chunk. if the caller wants random-access we need this chunk for that */
				memcpy(&a->idx1_chunk,&a->chunk,sizeof(a->chunk));
			}
		}
	}

	if (a->hdrl_chunk.absolute_data_offset == 0 || !riff_stack_chunk_contains_subchunks(&a->hdrl_chunk))
		return 0;
	if (a->movi_chunk.absolute_data_offset == 0 || !riff_stack_chunk_contains_subchunks(&a->movi_chunk))
		return 0;
	if (riff_stack_seek(riff,&a->hdrl_chunk,0) != 0)
		return 0;

	while (riff_stack_readchunk(riff,&a->hdrl_chunk,&a->chunk))
		if (!riff_stack_chunk_contains_subchunks(&a->chunk) && a->chunk.fourcc == avi_riff_avih)
			avih_chunk = a->chunk;

	if (avih_chunk.absolute_data_offset == 0)
		return 0;
	if (riff_stack_seek(riff,&avih_chunk,0) != 0)
		return 0;
	{ /* read 'avih' chunk */
		unsigned char buf[sizeof(riff_avih_AVIMAINHEADER)];
		riff_stack_read(riff,&avih_chunk,buf,sizeof(riff_avih_AVIMAINHEADER));
		memcpy(&a->avi_main_header,buf,sizeof(riff_avih_AVIMAINHEADER));

		a->avi_streams = a->avi_main_header.dwStreams;
		if (a->avi_streams > 256 || a->avi_streams < 0) a->avi_streams = 256;

		a->avi_stream = (avi_reader_stream*)
			malloc(sizeof(avi_reader_stream)*a->avi_streams);
		if (!a->avi_stream) return 0;
		memset(a->avi_stream,0,sizeof(avi_reader_stream)*a->avi_streams);

		a->avi_stream_index1 = (avi_reader_stream_index1*)
			malloc(sizeof(avi_reader_stream_index1)*a->avi_streams);
		if (!a->avi_stream_index1) return 0;
		memset(a->avi_stream_index1,0,sizeof(avi_reader_stream_index1)*a->avi_streams);

		a->avi_stream_odml_index = (avi_reader_stream_odml_index*)
			malloc(sizeof(avi_reader_stream_odml_index)*a->avi_streams);
		if (!a->avi_stream_odml_index) return 0;
		memset(a->avi_stream_odml_index,0,sizeof(avi_reader_stream_odml_index)*a->avi_streams);
	}

	if (riff_stack_seek(riff,&a->hdrl_chunk,0) != 0)
		return 0;

	avi_strf_index=0;
	while (riff_stack_readchunk(riff,&a->hdrl_chunk,&a->chunk)) {
		if (riff_stack_chunk_contains_subchunks(&a->chunk) && a->chunk.fourcc == avi_riff_strl) {
			if (avi_strf_index < a->avi_streams) {
				avi_reader_stream *s;

				strl_chunk = a->chunk;
				s = a->avi_stream+(avi_strf_index++);
				while (riff_stack_readchunk(riff,&strl_chunk,&a->chunk)) {
					if (riff_stack_chunk_contains_subchunks(&a->chunk))
						continue;

					if (a->chunk.fourcc == avi_riff_strh) {
						unsigned char buf[sizeof(riff_strh_AVISTREAMHEADER)];
						riff_stack_read(riff,&a->chunk,buf,sizeof(riff_strh_AVISTREAMHEADER));
						memcpy(&s->strh,buf,sizeof(riff_strh_AVISTREAMHEADER));
						memcpy(&s->strh_chunk,&a->chunk,sizeof(a->chunk));
					}
					else if (a->chunk.fourcc == avi_riff_strf) {
						memcpy(&s->strf_chunk,&a->chunk,sizeof(a->chunk));
						/* it's up to the caller to interpret these */
					}
					else if (a->chunk.fourcc == avi_riff_indx) {
						/* OpenDML AVI 2.0 index */
						memcpy(&s->indx_chunk,&a->chunk,sizeof(a->chunk));
					}
					else if (a->chunk.fourcc == avi_riff_vprp) {
						/* OpenDML video properties */
						memcpy(&s->vprp_chunk,&a->chunk,sizeof(a->chunk));
					}
					else if (a->chunk.fourcc == avi_riff_odml) {
						unsigned char tmp[16];
						riff_chunk sc;

						if (riff_stack_chunk_contains_subchunks(&a->chunk)) {
							while (riff_stack_readchunk(riff,&a->chunk,&sc)) {
								if (sc.fourcc == avi_riff_dmlh) {
									/* OpenDML 2.0 AVI files contain the TRUE length of the stream in the LIST:odml dmlh chunk.
									 * The dwLength field is only there to list the portion visible to older apps */
									/* NTS: we count on the AVI file to have put 'strh' first, followed by 'strf' and this
									 *      chunk. If this chunk comes before 'strh' the true framecount here will be overwritten */
									if (riff_stack_read(riff,&sc,tmp,4) == 4) {
										assert(sizeof(s->strh.dwLength) == 4);
										memcpy(&s->strh.dwLength,tmp,4);
									}
								}
							}
						}
					}
				}
			}
		}
	}

	if (!a->avi_streams)
		return 0;

	return 1;
}

avi_reader_stream_index1 *avi_reader_get_stream_index1(avi_reader *r,int stream_id) {
	if (!r) return NULL;
	if (!r->avi_streams) return NULL;
	if (stream_id < 0 || stream_id >= r->avi_streams) return NULL;
	return r->avi_stream_index1 + stream_id;
}

avi_reader_stream_odml_index *avi_reader_get_stream_odml_index(avi_reader *r,int stream_id) {
	if (!r) return NULL;
	if (!r->avi_streams) return NULL;
	if (stream_id < 0 || stream_id >= r->avi_streams) return NULL;
	return r->avi_stream_odml_index + stream_id;
}

int avi_reader_stream_index1_max_entry(avi_reader_stream_index1 *i) {
	if (!i) return 0;
	if (!i->map) return 0;
	return i->count;
}

riff_idx1_AVIOLDINDEX *avi_reader_stream_index1_get_entry(avi_reader_stream_index1 *i,int iindex) {
	if (!i) return NULL;
	if (!i->map) return NULL;
	if (iindex < 0 || iindex >= i->count) return NULL;
	return i->map + iindex;
}

int64_t avi_reader_stream_index1_get_entry_offset(avi_reader *avi,riff_idx1_AVIOLDINDEX *e) {
	if (!e || !avi) return -1LL;
	return avi->movi_chunk.absolute_header_offset + 8 + e->dwOffset;
}

