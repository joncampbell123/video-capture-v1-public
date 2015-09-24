
#include "avi_reader.h"
#include "avi_writer.h"
#include "rawint.h"
#include "minmax.h"
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <ctype.h>

/* large enough static buffer for any frame in a reasonable AVI file */
static unsigned char framedata[16*1024*1024];

/* DIE flag and signal handler */
static volatile int DIE = 0;
static void sigma(int __attribute__((unused)) x) {
	if (++DIE >= 20) abort();
}

int main(int argc,char **argv) {
	riff_avih_AVIMAINHEADER *mheader;
	unsigned int rate[64],scale[64];
	int savi_count = 0;
	avi_reader *savi[256];
	avi_writer *davi;
	int sfd,j,i;

	if (argc < 3) {
		fprintf(stderr,"c4_riff_datamosh_assist <in avi> [in avi] <out avi>\n");
		fprintf(stderr,"Combine AVI files by copying the video bitstream, minus the keyframes in secondary files.\n");
		fprintf(stderr,"For best results, encode all input AVI files with the same video codec and format and\n");
		fprintf(stderr,"use settings that encode with NO B-frames, and infinite GOP length.\n");
		return 1;
	}

	if ((davi=avi_writer_create()) == NULL) {
		fprintf(stderr,"cannot create AVI writer\n");
		return 1;
	}
	if (!avi_writer_open_file(davi,argv[argc-1])) {
		fprintf(stderr,"Cannot create AVI file\n");
		return 1;
	}

	for (j=1;j < (argc-1) && savi_count < 256;j++) {
		fprintf(stderr,"Opening AVI file %s\n",argv[j]);
		if ((sfd=open(argv[j],O_RDONLY)) < 0) {
			fprintf(stderr,"Cannot open\n");
			return 1;
		}
		fprintf(stderr,"   %s fd=%d\n",argv[j],sfd);
		if ((savi[savi_count]=avi_reader_create()) == NULL) {
			fprintf(stderr,"cannot create AVI reader\n");
			return 1;
		}
		avi_reader_fd(savi[savi_count],sfd);
		riff_stack_assign_fd_ownership(savi[savi_count]->stack);

		if (!avi_reader_scan(savi[savi_count])) {
			avi_reader_destroy(savi[savi_count]);
			continue;
		}
		avi_reader_scan_odml_index(savi[savi_count]);
		avi_reader_scan_index1(savi[savi_count]);
		if (savi[savi_count]->avi_streams == 0) {
			avi_reader_destroy(savi[savi_count]);
			continue;
		}

		if (savi_count == 0) {
			/* copy the source AVI main header to the destination */
			mheader = avi_writer_main_header(davi);
			assert(mheader != NULL);
			*mheader = savi[savi_count]->avi_main_header;

			/* scan each stream in the source AVI and create a matching stream in the target AVI */
			for (i=0;i < savi[savi_count]->avi_streams;i++) {
				riff_strh_AVISTREAMHEADER *sh;
				avi_reader_stream *is;
				avi_writer_stream *os;

				is = savi[savi_count]->avi_stream + i;
				os = avi_writer_new_stream(davi);
				assert(os != NULL);
				assert(os->index == i);

				/* copy the stream header */
				sh = avi_writer_stream_header(os);
				assert(sh != NULL);
				*sh = is->strh;

				rate[i] = sh->dwRate;
				scale[i] = sh->dwScale;
				fprintf(stderr,"[%u] %u/%u = %.3f\n",
						i,
						rate[i],scale[i],
						((double)rate[i]) / scale[i]);

				/* copy the format */
				if (is->strf_chunk.absolute_data_offset != 0) {
					unsigned char buffer[16384];
					if (is->strf_chunk.data_length > sizeof(buffer)) {
						fprintf(stderr,"Error: stream %u format chunk is too large\n",i);
						return 1;
					}

					if (lseek(sfd,is->strf_chunk.absolute_data_offset,SEEK_SET) != (signed long long)is->strf_chunk.absolute_data_offset) {
						fprintf(stderr,"Cannot lseek to format chunk\n");
						return 1;
					}
					if (read(sfd,buffer,is->strf_chunk.data_length) != (int)is->strf_chunk.data_length) {
						fprintf(stderr,"Cannot read format chunk\n");
						return 1;
					}
					if (!avi_writer_stream_set_format(os,buffer,is->strf_chunk.data_length)) {
						fprintf(stderr,"Cannot set format for output stream %u\n",i);
						return 1;
					}

					if (sh->fccType == avi_fccType_audio) {
						windows_WAVEFORMAT *fmt = (windows_WAVEFORMAT*)buffer;
						fprintf(stdout,"   WAVEFORMAT\n");
						fprintf(stdout,"      wFormatTag =       0x%04X\n",__le_u16(&(fmt->wFormatTag)));
						fprintf(stdout,"      nChannels =        %u\n",__le_u16(&(fmt->nChannels)));
						fprintf(stdout,"      nSamplesPerSec =   %u\n",__le_u32(&(fmt->nSamplesPerSec)));
						fprintf(stdout,"      nAvgBytesPerSec =  %u\n",__le_u32(&(fmt->nAvgBytesPerSec)));
						fprintf(stdout,"      nBlockAlign =      %u\n",__le_u16(&(fmt->nBlockAlign)));
						fprintf(stdout,"      wBitsPerSample =   %u\n",__le_u16(&(fmt->wBitsPerSample)));
					}
				}
			}
		}

		savi_count++;
	}

	/* start writing the target AVI */
	if (!avi_writer_begin_header(davi)) {
		fprintf(stderr,"Cannot begin header\n");
		return 1;
	}
	if (!avi_writer_begin_data(davi)) {
		fprintf(stderr,"Cannot begin data\n");
		return 1;
	}

	signal(SIGINT, sigma);
	signal(SIGQUIT,sigma);
	signal(SIGTERM,sigma);

	for (i=0;i < savi_count;i++) {
		avi_reader_stream_odml_index *index;
		avi_reader_stream_index1 *idx1;
		avi_writer_stream *os;
		avi_reader *ravi;

		ravi = savi[i];
		sfd = ravi->stack->fd;
		fprintf(stderr,"Copying from avi #%d/%d fd=%d\n",i+1,savi_count,sfd);
		for (int s=0;s < ravi->avi_streams;s++) {
			riff_strh_AVISTREAMHEADER *sh;
			int skip_key;

			index = avi_reader_get_stream_odml_index(ravi,s);
			idx1 = avi_reader_get_stream_index1(ravi,s);
			assert(s < davi->avi_stream_alloc);
			os = davi->avi_stream + s;

			sh = avi_writer_stream_header(os);
			assert(sh != NULL);
			if (sh->fccType == avi_fccType_video)
				skip_key = (i > 0);
			else
				skip_key = 0;

			fprintf(stderr,"Copying stream %d/%d\n",s+1,ravi->avi_streams);
			if (index != NULL && index->map != NULL && index->count != 0) {
				fprintf(stderr,"Using ODML index %u samples\n",index->count);
				for (int sample=0;sample < index->count;sample++) {
					avi_reader_stream_odml_index_entry *ic = index->map + sample;

					if (!AVI_ODML_INDX_NONKEY(ic->size) && skip_key) {
						avi_writer_stream_repeat_last_chunk(davi,os);
						continue;
					}

					if (lseek(sfd,ic->offset,SEEK_SET) != ic->offset) {
						fprintf(stderr,"Cannot seek to %lld\n",(long long)ic->offset);
						avi_writer_stream_repeat_last_chunk(davi,os);
						continue;
					}

					if (AVI_ODML_INDX_SIZE(ic->size) > sizeof(framedata)) {
						fprintf(stderr,"Video frame too large\n");
						abort();
					}

					if (AVI_ODML_INDX_SIZE(ic->size) > 0) {
						read(sfd,framedata,AVI_ODML_INDX_SIZE(ic->size));
						avi_writer_stream_write(davi,os,framedata,AVI_ODML_INDX_SIZE(ic->size),
							AVI_ODML_INDX_NONKEY(ic->size) ? 0 : riff_idx1_AVIOLDINDEX_flags_KEYFRAME);
					}
					else {
						avi_writer_stream_repeat_last_chunk(davi,os);
					}

					if (sh->fccType == avi_fccType_video)
						skip_key = 1;
				}
			}
			else if (idx1 != NULL && idx1->map != NULL && idx1->count != 0) {
				fprintf(stderr,"Using old index %u samples\n",idx1->count);
				for (int sample=0;sample < idx1->count;sample++) {
					riff_idx1_AVIOLDINDEX *ic = idx1->map + sample;

					if ((ic->dwFlags & riff_idx1_AVIOLDINDEX_flags_KEYFRAME) && skip_key) {
						avi_writer_stream_repeat_last_chunk(davi,os);
						continue;
					}

					if (lseek(sfd,ic->dwOffset,SEEK_SET) != ic->dwOffset) {
						fprintf(stderr,"Cannot seek to %lld\n",(long long)ic->dwOffset);
						avi_writer_stream_repeat_last_chunk(davi,os);
						continue;
					}

					if (ic->dwSize > sizeof(framedata)) {
						fprintf(stderr,"Video frame too large\n");
						abort();
					}

					if (ic->dwSize > 0) {
						read(sfd,framedata,ic->dwSize);
						avi_writer_stream_write(davi,os,framedata,ic->dwSize,ic->dwFlags);
					}
					else {
						avi_writer_stream_repeat_last_chunk(davi,os);
					}

					if (sh->fccType == avi_fccType_video)
						skip_key = 1;
				}
			}
		}
	}

	if (!avi_writer_end_data(davi)) {
		fprintf(stderr,"Cannot end data\n");
		return 1;
	}
	if (!avi_writer_finish(davi)) {
		fprintf(stderr,"Cannot finish AVI\n");
		return 1;
	}

	avi_writer_close_file(davi);
	avi_writer_destroy(davi);
	for (i=0;i < savi_count;i++) {
		avi_reader_destroy(savi[i]);
		savi[i] = NULL;
	}

	return 0;
}

