
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

typedef struct {
	unsigned int		avi_index;
	unsigned long		start,end; /* inclusive */
	unsigned int		repeat;
	int			step;
} scentry;

#define MAX_SCENT  2048

int main(int argc,char **argv) {
	int scent_max=0;
	scentry scent[MAX_SCENT];
	const char *script = NULL;
	riff_avih_AVIMAINHEADER *mheader;
	unsigned int rate[64],scale[64];
	avi_reader *savi[256];
	int savi_count = 0;
	avi_writer *davi;
	int sfd,j,i;

	if (argc < 4) {
		fprintf(stderr,"c4_riff_datamosh_assist <script> <in avi> [in avi] <out avi>\n");
		fprintf(stderr,"Combine AVI files by copying the video bitstream, minus the keyframes in secondary files.\n");
		fprintf(stderr,"For best results, encode all input AVI files with the same video codec and format and\n");
		fprintf(stderr,"use settings that encode with NO B-frames, and infinite GOP length.\n");
		fprintf(stderr,"Script takes the form range,range,range where 'range' is:\n");
		fprintf(stderr,"     index:start-end\n");
		fprintf(stderr,"Each start/end is given in frames\n");
		fprintf(stderr,"each range can have /repeat on it as well to indicate how often to repeat that frame\n");
		fprintf(stderr,"\n");
		fprintf(stderr,"   Example:   0:0-15             First AVI, copy frames 0-15 once\n");
		fprintf(stderr,"              0:0-30,0:60-75     Copy from first avi frames 0-30, then 60-75\n");
		fprintf(stderr,"              0:0-15,0:16/30     Copy frames 0-15, then repeat frame 16 30 times\n");
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

	script = argv[1];
	while (*script) {
		scentry *sc = scent+scent_max;
		if (scent_max >= MAX_SCENT) abort();

		sc->step = 1;
		sc->repeat = 1;
		sc->avi_index = strtol(script,(char**)(&script),10);
		if (*script++ != ':') {
			fprintf(stderr,"Syntax error in script\n");
			return 1;
		}
		sc->start = sc->end = strtol(script,(char**)(&script),10);
		if (*script == '-') {
			script++;
			sc->end = strtol(script,(char**)(&script),10);
		}
		if (*script == 's') {
			script++;
			sc->step = strtol(script,(char**)(&script),10);
		}
		if (*script == '/') {
			script++;
			sc->repeat = strtol(script,(char**)(&script),10);
		}

		if (sc->step == 0) sc->step = 1;

		if (sc->end < sc->start) {
			unsigned long t = sc->start;
			sc->start = sc->end;
			sc->end = t;
			sc->step = -sc->step;
		}

		fprintf(stderr,"Script: %u:%lu-%lus%d/%u\n",
			sc->avi_index,
			sc->start,
			sc->end,
			sc->step,
			sc->repeat);

		while (*script && *script != ',') script++;
		if (*script == ',') script++;

		scent_max++;
	}

	for (j=2;j < (argc-1) && savi_count < 256;j++) {
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

	for (int sc=0;sc < scent_max;sc++) {
		scentry *sc_entry = &scent[sc];

		if (sc_entry->avi_index >= (unsigned int)savi_count) {
			fprintf(stderr,"ERROR: AVI index out of range\n");
			continue;
		}

		avi_reader_stream_odml_index *index;
		avi_reader_stream_index1 *idx1;
		avi_writer_stream *os;
		avi_reader *ravi;

		ravi = savi[sc_entry->avi_index];
		sfd = ravi->stack->fd;
		for (int s=0;s < ravi->avi_streams;s++) {
			riff_strh_AVISTREAMHEADER *sh;

			index = avi_reader_get_stream_odml_index(ravi,s);
			idx1 = avi_reader_get_stream_index1(ravi,s);
			assert(s < davi->avi_stream_alloc);
			os = davi->avi_stream + s;

			sh = avi_writer_stream_header(os);
			assert(sh != NULL);
			if (sh->fccType != avi_fccType_video)
				continue;

			assert(sc_entry->step != 0);
			for (int rep=0;rep < (int)sc_entry->repeat;rep++) {
				for (unsigned long sample = sc_entry->step > 0L ? sc_entry->start : sc_entry->end;(signed long)sample >= 0L && (
					(sc_entry->step > 0 && sample <= sc_entry->end) ||
					(sc_entry->step < 0 && sample >= sc_entry->start));sample += sc_entry->step) {
				unsigned long long offset;
				unsigned char keyframe;
				unsigned long size;

				if (index != NULL && index->map != NULL && index->count != 0) {
					if (sample >= (unsigned long)index->count) continue;
					avi_reader_stream_odml_index_entry *ic = index->map + sample;
					keyframe = !AVI_ODML_INDX_NONKEY(ic->size);
					offset = ic->offset;
					size = AVI_ODML_INDX_SIZE(ic->size);
				}
				else if (idx1 != NULL && idx1->map != NULL && idx1->count != 0) {
					if (sample >= (unsigned long)idx1->count) continue;
					riff_idx1_AVIOLDINDEX *ic = idx1->map + sample;
					keyframe = !!(ic->dwFlags & riff_idx1_AVIOLDINDEX_flags_KEYFRAME);
					offset = ic->dwOffset;
					size = ic->dwSize;
				}
				else {
					continue;
				}

				if (lseek(sfd,(off_t)offset,SEEK_SET) != (off_t)offset)
					continue;
				if (size > sizeof(framedata))
					continue;

				if (size > 0) {
					read(sfd,framedata,size);
					avi_writer_stream_write(davi,os,framedata,size,keyframe ? riff_idx1_AVIOLDINDEX_flags_KEYFRAME : 0);
				}
				else {
					avi_writer_stream_repeat_last_chunk(davi,os);
				}
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

