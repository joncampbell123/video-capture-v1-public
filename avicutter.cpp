
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
#include <math.h>

#include <vector>
#include <string>

using namespace std;

/* large enough static buffer for any frame in a reasonable AVI file */
static unsigned char framedata[16*1024*1024];

static string		in_avi;
static string		out_avi;
static double		start_time = -1;
static double		end_time = -1;
static bool		opt_no_avioldindex = false;
static bool		opt_no_aviodmlindex = false;

/* DIE flag and signal handler */
static volatile int DIE = 0;
static void sigma(int __attribute__((unused)) x) {
	if (++DIE >= 20) abort();
}

static void help() {
	fprintf(stderr,"avicutter [options] -i <source AVI> --start <time> --end <time> -o <dest AVI>\n");
	fprintf(stderr,"  --ignore-oldindex                Ignore AVIOLDINDEX\n");
	fprintf(stderr,"  --ignore-odmlindex               Ignore AVISUPERINDEX\n");
}

/* s => seconds
 * h:m:s = second including decimal */
static double parse_time(const char *s) {
	const char *orig_s = s;
	double ret = -1;
	double sec = -1;
	int hour = -1;
	int min = -1;

	while (*s) {
		if (*s == ' ') {
			s++;
		}
		else if (isdigit(*s)) {
			sec = strtof(s,(char**)(&s));
		}
		else if (*s == ':') {
			hour = min;
			min = (int)floor(sec + 0.5);
			sec = -1;
			s++;
		}
		else {
			return -1;
		}
	}

	if (hour >= 0 || min >= 0 || sec >= 0.0)
		ret = 0;

	if (sec >= 0)
		ret += sec;
	if (min >= 0)
		ret += min * 60;
	if (hour >= 0)
		ret += hour * 60 * 60;

	return ret;
}

static bool parse(int argc,char **argv) {
	char *a;
	int i;

	for (i=1;i < argc;) {
		a = argv[i++];

		if (*a == '-') {
			do { a++; } while (*a == '-');

			if (!strcmp(a,"h") || !strcmp(a,"help")) {
				help();
				return false;
			}
			else if (!strcmp(a,"i")) {
				in_avi = argv[i++];
			}
			else if (!strcmp(a,"o")) {
				out_avi = argv[i++];
			}
			else if (!strcmp(a,"start")) {
				start_time = parse_time(argv[i++]);
			}
			else if (!strcmp(a,"end")) {
				end_time = parse_time(argv[i++]);
			}
			else if (!strcmp(a,"ignore-oldindex")) {
				opt_no_avioldindex = true;
			}
			else if (!strcmp(a,"ignore-odmlindex")) {
				opt_no_aviodmlindex = true;
			}
			else {
				fprintf(stderr,"Unknown switch '%s'\n",a);
				return false;
			}
		}
		else {
			fprintf(stderr,"Unexpected arg '%s'\n",a);
			return false;
		}
	}

	if (in_avi.empty() || out_avi.empty())
		return false;

	return true;
}

class AVIDestination {
public:
	AVIDestination() : davi(NULL) {
	}
	~AVIDestination() {
		close();
	}
public:
	bool begin_header() {
		if (davi != NULL)
			return (avi_writer_begin_header(davi) != 0)?true:false;

		return false;
	}
	bool begin_data() {
		if (davi != NULL)
			return (avi_writer_begin_data(davi) != 0)?true:false;

		return false;
	}
	bool end_data() {
		if (davi != NULL) 
			return (avi_writer_end_data(davi) != 0)?true:false;

		return false;
	}
	void close() {
		if (davi != NULL) {
			avi_writer_end_data(davi);
			avi_writer_finish(davi);
			avi_writer_close_file(davi);
			davi = avi_writer_destroy(davi);
		}
	}
	bool open(const char *path) {
		close();

		if ((davi=avi_writer_create()) == NULL) {
			close();
			return false;
		}
		if (!avi_writer_open_file(davi,path)) {
			close();
			return false;
		}

		return true;
	}
	riff_avih_AVIMAINHEADER *mainheader() {
		if (davi == NULL) return NULL;
		return avi_writer_main_header(davi);
	}
	size_t stream_count() {
		if (davi == NULL) return size_t(0);
		return (size_t)(davi->avi_stream_max);
	}
	avi_writer_stream *get_stream(const size_t c) {
		if (davi == NULL) return NULL;
		if (davi->avi_stream == NULL) return NULL;
		if (c >= (size_t)(davi->avi_stream_max)) return NULL;
		return davi->avi_stream + c;
	}
	riff_strh_AVISTREAMHEADER *get_stream_header(const size_t c) {
		avi_writer_stream *s = get_stream(c);
		if (s == NULL) return NULL;
		return avi_writer_stream_header(s);
	}
	unsigned char *get_format_data(const size_t stream) {
		avi_writer_stream *s = get_stream(stream);
		if (s == NULL) return NULL;
		return (unsigned char*)(s->format);
	}
	size_t get_format_data_size(const size_t stream) {
		avi_writer_stream *s = get_stream(stream);
		if (s == NULL) return 0;
		return s->format_len;
	}
	bool set_format_data(const size_t stream,unsigned char *data,const size_t len) {
		avi_writer_stream *s = get_stream(stream);
		if (s == NULL) return false;
		if (!avi_writer_stream_set_format(s,(void*)data,len)) return false;
		return true;
	}
	avi_writer_stream *new_stream() {
		if (davi == NULL) return NULL;
		return avi_writer_new_stream(davi);
	}
public:
	avi_writer*		davi;
};

class AVISource {
public:
	AVISource() : savi(NULL), savi_fd(-1) {
	}
	~AVISource() {
		close();
	}
public:
	class fmtinfo {
public:
		fmtinfo() : data(NULL), datalen(0) {
		}
		~fmtinfo() {
			free_data();
		}
		bool alloc_data(const size_t len/*length of strf*/) {
			free_data();
			if (len == 0) return false;
			if (len > 65536) return false;

			data = new (std::nothrow) unsigned char[len];
			if (data == NULL) return false;
			datalen = len;
			return true;
		}
		void free_data() {
			if (data) {
				delete[] data;
				data = NULL;
			}
			datalen = 0;
		}
public:
		unsigned char*		data;
		size_t			datalen;
	};
public:
	bool open(const char *path) {
		close();

		savi_fd = ::open(path,O_RDONLY);
		if (savi_fd < 0) {
			close();
			return false;
		}

		savi = avi_reader_create();
		if (savi == NULL) {
			close();
			return false;
		}

		avi_reader_fd(savi,savi_fd);
		riff_stack_assign_fd_ownership(savi->stack);
		savi_fd = -1;

		if (!avi_reader_scan(savi)) {
			close();
			return false;
		}

		if (!opt_no_aviodmlindex)
			avi_reader_scan_odml_index(savi);

		if (!opt_no_avioldindex)
			avi_reader_scan_index1(savi);

		if (savi->avi_streams == 0) {
			close();
			return false;
		}

		return true;
	}
	void close() {
		free_all_format_data();
		savi = avi_reader_destroy(savi);
		if (savi_fd >= 0) {
			::close(savi_fd);
			savi_fd = -1;
		}
	}
	riff_avih_AVIMAINHEADER *mainheader() {
		if (savi == NULL) return NULL;
		return &(savi->avi_main_header);
	}
	size_t stream_count() {
		if (savi == NULL) return size_t(0);
		return (size_t)(savi->avi_streams);
	}
	avi_reader_stream *get_stream(const size_t c) {
		if (savi == NULL) return NULL;
		if (savi->avi_stream == NULL) return NULL;
		if (c >= (size_t)(savi->avi_streams)) return NULL;
		return savi->avi_stream + c;
	}
	riff_strh_AVISTREAMHEADER *get_stream_header(const size_t c) {
		avi_reader_stream *s = get_stream(c);
		if (s == NULL) return NULL;
		return &(s->strh);
	}
	unsigned char *get_format_data(const size_t stream) {
		if (!load_format_data(stream)) return NULL;
		if (stream >= stream_format_data.size()) return NULL;
		fmtinfo *fi = stream_format_data[stream];
		if (fi == NULL) return NULL;
		return fi->data;
	}
	size_t get_format_data_size(const size_t stream) {
		if (!load_format_data(stream)) return 0;
		if (stream >= stream_format_data.size()) return 0;
		fmtinfo *fi = stream_format_data[stream];
		if (fi == NULL) return 0;
		return fi->datalen;
	}
	void free_format_data(const size_t stream) {
		if (stream >= stream_format_data.size()) return;

		if (stream_format_data[stream] != NULL) {
			delete stream_format_data[stream];
			stream_format_data[stream] = NULL;
		}
	}
	void free_all_format_data() {
		if (savi == NULL) return;

		for (size_t i=0;i < (size_t)(savi->avi_streams);i++)
			free_format_data(i);

		stream_format_data.clear();
	}
	bool load_format_data(const size_t stream) {
		int i;

		avi_reader_stream *s = get_stream(stream);
		if (s == NULL) return false;

		if (s->strf_chunk.absolute_data_offset == 0 || s->strf_chunk.data_length == 0 || s->strf_chunk.data_length > 65536)
			return false;

		while (stream_format_data.size() <= stream)
			stream_format_data.push_back((fmtinfo*)NULL);

		if (stream_format_data[stream] == NULL) {
			fmtinfo *fi = new (std::nothrow) fmtinfo();
			if (fi == NULL) return false;

			if (!fi->alloc_data((size_t)(s->strf_chunk.data_length))) {
				delete fi;
				return false;
			}
			assert(fi->data != NULL);
			assert(fi->datalen >= (size_t)(s->strf_chunk.data_length));

			riff_stack_push(savi->stack,&(s->strf_chunk));
			i = riff_stack_read(savi->stack,riff_stack_top(savi->stack),fi->data,fi->datalen);
			riff_stack_pop(savi->stack);

			if (i != (int)fi->datalen) {
				delete fi;
				return false;
			}

//			fprintf(stderr,"Stream #%zu: fmtlen=%zu loaded\n",stream,fi->datalen);
			stream_format_data[stream] = fi;
		}

		return true;
	}
	void load_all_format_data() {
		for (size_t i=0;i < (size_t)(savi->avi_streams);i++)
			load_format_data(i);
	}
	avi_reader_stream_index1 *get_old_index(const size_t stream) {
		if (savi == NULL) return NULL;
		if (savi->avi_streams == 0) return NULL;
		if (savi->avi_stream_index1 == NULL) return NULL;
		
		avi_reader_stream_index1 *r = savi->avi_stream_index1 + stream;
		if (r->count == 0 || r->map == NULL) return NULL;

		return r;
	}
	avi_reader_stream_odml_index *get_odml_index(const size_t stream) {
		if (savi == NULL) return NULL;
		if (savi->avi_streams == 0) return NULL;
		if (savi->avi_stream_odml_index == NULL) return NULL;
		
		avi_reader_stream_odml_index *r = savi->avi_stream_odml_index + stream;
		if (r->count == 0 || r->map == NULL) return NULL;

		return r;
	}
	size_t read_stream_max_index(const size_t stream) {
		avi_reader_stream_odml_index *odml = get_odml_index(stream);
		if (odml != NULL) return odml->count;

		avi_reader_stream_index1 *idx1 = get_old_index(stream);
		if (idx1 != NULL) return idx1->count;

		return 0;
	}
	bool read_stream_index(const size_t stream,const size_t index,off_t &file_offset,uint32_t &size,uint32_t &dwFlags) {
		avi_reader_stream_odml_index *odml = get_odml_index(stream);
		if (odml != NULL) {
			if (index >= odml->count) return false;
			assert(index < odml->alloc);

			avi_reader_stream_odml_index_entry *entry = odml->map + index;

			file_offset = (off_t)entry->offset;
			size = (uint32_t)AVI_ODML_INDX_SIZE(entry->size);
			dwFlags = AVI_ODML_INDX_NONKEY(entry->size) ? 0 : riff_idx1_AVIOLDINDEX_flags_KEYFRAME;
			return true;
		}

		avi_reader_stream_index1 *idx1 = get_old_index(stream);
		if (idx1 != NULL) {
			if (index >= idx1->count) return false;
			assert(index < idx1->alloc);

			riff_idx1_AVIOLDINDEX *entry = idx1->map + index;

			file_offset = (off_t)entry->dwOffset;
			size = (uint32_t)entry->dwSize;
			dwFlags = (uint32_t)entry->dwFlags;
			return true;
		}

		return false;
	}
	size_t seconds2frames(const size_t stream,const double t) {
		avi_reader_stream *strm = get_stream(stream);
		if (strm == NULL) return 0;

		riff_strh_AVISTREAMHEADER *strh = get_stream_header(stream);
		if (strh == NULL) return 0;

		uint64_t tmp = (uint64_t)floor(t * strh->dwRate);
		return (size_t)(tmp / strh->dwScale);
	}
	uint64_t seconds2blocks(const size_t stream,const double t,size_t &samples_per_block,size_t &bytes_per_block,size_t &block_sample_offset,size_t &sample_rate) {
		avi_reader_stream *strm = get_stream(stream);
		if (strm == NULL) return 0;

		riff_strh_AVISTREAMHEADER *strh = get_stream_header(stream);
		if (strh == NULL) return 0;

		unsigned char *fmt = get_format_data(stream);
		size_t fmtlen = get_format_data_size(stream);

		if (fmtlen < sizeof(windows_WAVEFORMAT)) return 0;
		windows_WAVEFORMAT *wfx = (windows_WAVEFORMAT*)fmt;

		if (wfx->wFormatTag == windows_WAVE_FORMAT_PCM) {
			uint64_t samp = (uint64_t)floor(t * wfx->nSamplesPerSec);
			samples_per_block = 1;
			bytes_per_block = wfx->nBlockAlign;
			sample_rate = wfx->nSamplesPerSec;
			block_sample_offset = 0;
			return samp;
		}

		return 0;
	}
public:
	std::vector<fmtinfo*>	stream_format_data;
	avi_reader*		savi;
	int			savi_fd;
};

int main(int argc,char **argv) {
	AVISource savi;
	AVIDestination davi;
	std::vector<std::pair<size_t,size_t> > video_crop;
	std::vector<std::pair<uint64_t,uint64_t> > audio_crop; /* in audio blocks */
	std::vector<size_t> audio_samples_per_block;
	std::vector<size_t> audio_bytes_per_block;
	std::vector<size_t> audio_sample_rate;
	std::vector<size_t> video_start;

	struct audio_scan {
public:
		audio_scan() : avi_index(0), avi_index_offset(0), avi_index_max(0), byte_offset(0), avi_index_fileoffset(0) {
		}
public:
		size_t		avi_index;
		size_t		avi_index_offset;
		size_t		avi_index_max;
		uint64_t	byte_offset;
		off_t		avi_index_fileoffset;
	};
	std::vector<audio_scan> audio_stream_scan;

	struct video_scan {
public:
		video_scan() : avi_index(0), wait_for_keyframe(true) {
		}
public:
		size_t		avi_index;
		bool		wait_for_keyframe;
	};
	std::vector<video_scan> video_stream_scan;

	if (!parse(argc,argv)) {
		help();
		return 1;
	}

	/* source AVI? */
	if (!savi.open(in_avi.c_str())) {
		fprintf(stderr,"Failed to open source avi '%s'\n",in_avi.c_str());
		return 1;
	}
	savi.load_all_format_data();

	/* dest AVI? */
	if (!davi.open(out_avi.c_str())) {
		fprintf(stderr,"Failed to open dest avi '%s'\n",out_avi.c_str());
		return 1;
	}

	/* whatever AVI header is in the source, copy to the test */
	{
		riff_avih_AVIMAINHEADER *sh,*dh;

		sh = savi.mainheader();
		dh = davi.mainheader();
		if (sh == NULL || dh == NULL) {
			fprintf(stderr,"Failed to access AVI main header, sh=%p dh=%p\n",
				(void*)sh,(void*)dh);
			return 1;
		}

		/* copy */
		*dh = *sh;
		dh->dwTotalFrames = 0;
	}

	/* copy streams as well */
	{
		size_t stream;

		for (stream=0;stream < savi.stream_count();stream++) {
			riff_strh_AVISTREAMHEADER *is,*os;
			avi_writer_stream *ow;

			is = savi.get_stream_header(stream);
			if (is == NULL) continue;
			ow = davi.new_stream();
			if (ow == NULL) continue;
			os = davi.get_stream_header(ow->index);
			if (os == NULL) continue;

			/* copy stream header, but zero dwLength so AVI writer can update it */
			*os = *is;
			os->dwLength = 0;

			/* copy format data */
			unsigned char *fmtdata = savi.get_format_data(stream);
			size_t fmtlen = savi.get_format_data_size(stream);

			if (fmtdata != NULL && fmtlen != 0)
				davi.set_format_data(stream,fmtdata,fmtlen);
		}
	}

#if 0 /*DEBUG*/
	for (size_t stream=0;stream < savi.stream_count();stream++) {
		fprintf(stderr,"savi index=%zu max=%zu\n",stream,savi.read_stream_max_index(stream));
		for (size_t i=0;i < savi.read_stream_max_index(stream);i++) {
			off_t file_offset;
			uint32_t dwFlags;
			uint32_t size;

			if (savi.read_stream_index(stream,i,/*&*/file_offset,/*&*/size,/*&*/dwFlags)) {
				fprintf(stderr,"  [%zu] = offset=%llu size=%lu dwFlags=0x%lx\n",
					i,(unsigned long long)file_offset,(unsigned long)size,(unsigned long)dwFlags);
			}
			else {
				fprintf(stderr,"  [%zu] = n/a\n",i);
			}
		}
	}
#endif

	/* for each stream, figure out how to crop */
	audio_crop.clear();
	video_crop.clear();
	for (size_t stream=0;stream < savi.stream_count();stream++) {
		audio_crop.push_back(std::pair<uint64_t,uint64_t>(0,0));
		video_crop.push_back(std::pair<size_t,size_t>(0,0));
		audio_samples_per_block.push_back(0);
		audio_bytes_per_block.push_back(0);
		audio_sample_rate.push_back(0);
		video_start.push_back(0);
	}
	audio_stream_scan.resize(savi.stream_count());
	video_stream_scan.resize(savi.stream_count());

	for (size_t stream=0;stream < savi.stream_count();stream++) {
		avi_reader_stream *strm = savi.get_stream(stream);
		if (strm == NULL) continue;

		riff_strh_AVISTREAMHEADER *strh = savi.get_stream_header(stream);
		if (strh == NULL) continue;

		if (strh->fccType == avi_fccType_audio) {
			size_t samples_per_block=0,bytes_per_block=0,block_sample_offset=0,sample_rate=0;
			uint64_t start,end;

			start = savi.seconds2blocks(stream,start_time,/*&*/samples_per_block,/*&*/bytes_per_block,/*&*/block_sample_offset,/*&*/sample_rate);
			audio_samples_per_block[stream] = samples_per_block;
			audio_bytes_per_block[stream] = bytes_per_block;
			audio_sample_rate[stream] = sample_rate;
			audio_crop[stream].first = start;

			end = savi.seconds2blocks(stream,end_time,/*&*/samples_per_block,/*&*/bytes_per_block,/*&*/block_sample_offset,/*&*/sample_rate);
			if (block_sample_offset != 0) end++;
			audio_crop[stream].second = end;

			fprintf(stderr,"Audio stream #%zu: blocks=%llu-%llu (samp/block=%zu byte/block=%zu)\n",
				stream,start,end,samples_per_block,bytes_per_block);

			/* and then prepare AVI scan */
			assert(stream < audio_stream_scan.size());
			audio_scan &ascan = audio_stream_scan[stream];

			uint64_t startb = start * (uint64_t)bytes_per_block;
			uint32_t dwFlags,size;
			off_t file_offset;

			while (ascan.byte_offset < startb) {
				if (ascan.avi_index >= savi.read_stream_max_index(stream))
					break;
				if (!savi.read_stream_index(stream,ascan.avi_index,/*&*/file_offset,/*&*/size,/*&*/dwFlags))
					break;

				uint64_t howmuch = startb - ascan.byte_offset;
				if (howmuch >= (uint64_t)size) {
					ascan.avi_index++;
					ascan.avi_index_max = 0;
					ascan.avi_index_offset = 0;
					ascan.avi_index_fileoffset = 0;
					ascan.byte_offset += (uint64_t)size;
					continue;
				}

				ascan.avi_index_max = (size_t)size;
				ascan.avi_index_offset = (size_t)howmuch;
				ascan.byte_offset += (uint64_t)howmuch;
				ascan.avi_index_fileoffset = file_offset;
				break;
			}

			fprintf(stderr,"AVI index #%zu scan: index=%zu offset=%zu/%zu byteoffset=%llu\n",
				stream,ascan.avi_index,ascan.avi_index_offset,ascan.avi_index_max,(unsigned long long)ascan.byte_offset);
		}
		else if (strh->fccType == avi_fccType_video) {
			size_t start,end;

			start = savi.seconds2frames(stream,start_time);
			end = savi.seconds2frames(stream,end_time);
			video_crop[stream].first = start;
			video_crop[stream].second = end;
			video_start[stream] = start;

			fprintf(stderr,"Video stream #%zu: frames=%zu-%zu\n",
				stream,start,end);

			/* and then prepare AVI scan */
			assert(stream < video_stream_scan.size());
			video_scan &vscan = video_stream_scan[stream];

			uint32_t dwFlags,size;
			off_t file_offset;

			vscan.avi_index = (size_t)start;

			/* we need to scan until keyframe */
			while (1) {
				if (vscan.avi_index >= savi.read_stream_max_index(stream))
					break;
				if (!savi.read_stream_index(stream,vscan.avi_index,/*&*/file_offset,/*&*/size,/*&*/dwFlags))
					break;
				if (dwFlags & riff_idx1_AVIOLDINDEX_flags_KEYFRAME)
					break;

				vscan.avi_index++;
			}

			fprintf(stderr,"AVI index #%zu scan: index=%zu starting_from=%zu\n",
				stream,vscan.avi_index,start);

		}
	}

	/* begin dest AVI */
	if (!davi.begin_header()) {
		fprintf(stderr,"dest avi failed to begin header\n");
		return 1;
	}
	if (!davi.begin_data()) {
		fprintf(stderr,"dest avi failed to begin data\n");
		return 1;
	}

	/* DO IT */
	{
		bool keep_going;
		double least_t = -1,tt;
		int do_stream = -1;

		keep_going = true;
		do {
			keep_going = false;
			do_stream = -1;
			least_t = -1;

			for (size_t stream=0;stream < savi.stream_count();stream++) {
				avi_reader_stream *strm = savi.get_stream(stream);
				if (strm == NULL) continue;

				riff_strh_AVISTREAMHEADER *strh = savi.get_stream_header(stream);
				if (strh == NULL) continue;

				if (strh->fccType == avi_fccType_audio) {
					if (audio_crop[stream].first < audio_crop[stream].second) {
						tt = (double)audio_crop[stream].first / audio_sample_rate[stream];
						if (least_t < 0 || least_t > tt) {
							least_t = tt;
							do_stream = (int)stream;
							keep_going = true;
						}
					}
				}
				else if (strh->fccType == avi_fccType_video) {
					if (video_crop[stream].first < video_crop[stream].second) {
						uint64_t tmp = video_crop[stream].first * strh->dwRate;
						tt = (double)tmp / strh->dwScale;
						if (least_t < 0 || least_t > tt) {
							least_t = tt;
							do_stream = (int)stream;
							keep_going = true;
						}
					}
				}
			}

			if (do_stream >= 0) {
				assert(do_stream < savi.stream_count());

				riff_strh_AVISTREAMHEADER *strh = savi.get_stream_header((size_t)do_stream);
				if (strh == NULL) continue;

				if (strh->fccType == avi_fccType_audio) {
					assert(audio_crop[do_stream].first < audio_crop[do_stream].second);
					audio_scan &ascan = audio_stream_scan[do_stream];
					size_t max_blocks = audio_sample_rate[do_stream] / 5;
					assert(max_blocks != 0);
					size_t do_blocks = max_blocks;

					uint64_t boff = (uint64_t)audio_crop[do_stream].first * audio_bytes_per_block[do_stream];
					if (boff != ascan.byte_offset) fprintf(stderr,"Audio byte offset mismatch\n");

					if ((audio_crop[do_stream].first + do_blocks) > audio_crop[do_stream].second)
						do_blocks = audio_crop[do_stream].second - audio_crop[do_stream].first;

					size_t do_bytes = do_blocks * audio_bytes_per_block[do_stream];
					int rd,do_rd;

					audio_crop[do_stream].first += do_blocks;
					while (do_bytes > 0) {
						off_t file_offset;
						uint32_t size,dwFlags;
						size_t bsz = do_bytes;

						while (ascan.avi_index_offset >= ascan.avi_index_max) {
							if (ascan.avi_index >= savi.read_stream_max_index((size_t)do_stream)) break;

							ascan.avi_index++;
							ascan.avi_index_max = 0;
							ascan.avi_index_offset = 0;
							if (!savi.read_stream_index((size_t)do_stream,ascan.avi_index,/*&*/file_offset,/*&*/size,/*&*/dwFlags)) continue;
							ascan.avi_index_fileoffset = file_offset;
							ascan.avi_index_max = (size_t)size;
						}

						if (ascan.avi_index_offset < ascan.avi_index_max) {
							if (bsz > (ascan.avi_index_max-ascan.avi_index_offset))
								bsz = (ascan.avi_index_max-ascan.avi_index_offset);

							if (riff_stack_seek(savi.savi->stack,NULL,ascan.avi_index_fileoffset+ascan.avi_index_offset) == (ascan.avi_index_fileoffset+ascan.avi_index_offset)) {
								if (riff_stack_read(savi.savi->stack,NULL,framedata,bsz) == bsz) {
									if (!avi_writer_stream_write(davi.davi,davi.get_stream(do_stream),framedata,bsz,dwFlags))
										fprintf(stderr,"AVI audio failed to write\n");
								}
								else {
									fprintf(stderr,"AVI frame cannot read\n");
								}
							}
							else {
								fprintf(stderr,"AVI frame cannot seek\n");
							}

							assert(do_bytes >= bsz);
							do_bytes -= bsz;

							ascan.byte_offset += bsz;
							ascan.avi_index_offset += bsz;
							assert(ascan.avi_index_offset <= ascan.avi_index_max);
						}
						else {
							break;
						}
					}
				}
				else if (strh->fccType == avi_fccType_video) {
					assert(video_crop[do_stream].first < video_crop[do_stream].second);
					video_scan &vscan = video_stream_scan[do_stream];
					bool do_copy = false;

					if (vscan.wait_for_keyframe) { /* AVI scan above left avi_index at keyframe. copy keyframe NOW to start stream properly */
						vscan.wait_for_keyframe = false;
						do_copy = true;
					}
					else if (video_crop[do_stream].first >= vscan.avi_index) {
						do_copy = true;
					}

					if (do_copy) {
						off_t file_offset;
						uint32_t size,dwFlags;

						if (savi.read_stream_index((size_t)do_stream,vscan.avi_index,/*&*/file_offset,/*&*/size,/*&*/dwFlags)) {
							{
								int patience = 1000;
								assert(davi.get_stream(do_stream) != NULL);
								while (davi.get_stream(do_stream)->sample_write_chunk < (video_crop[do_stream].first - video_start[do_stream]) && patience-- > 0)
									avi_writer_stream_write(davi.davi,davi.get_stream(do_stream),NULL,0,0);
							}

							if (size == 0) {
								/* do nothing */
							}
							else if (size <= sizeof(framedata)) {
								if (riff_stack_seek(savi.savi->stack,NULL,file_offset) == file_offset) {
									if (riff_stack_read(savi.savi->stack,NULL,framedata,size) == size) {
										if (!avi_writer_stream_write(davi.davi,davi.get_stream(do_stream),framedata,size,dwFlags))
											fprintf(stderr,"AVI frame failed to write\n");
									}
									else {
										fprintf(stderr,"AVI frame cannot read\n");
									}
								}
								else {
									fprintf(stderr,"AVI frame cannot seek\n");
								}
							}
							else {
								fprintf(stderr,"AVI frame too large!\n");
							}
						}

						vscan.avi_index++;
					}

					video_crop[do_stream].first++;
				}
			}
		} while (keep_going);
	}

	/* done */
	davi.end_data();
	davi.close();
	savi.close();
	return 0;
}

