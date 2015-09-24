
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

/* DIE flag and signal handler */
static volatile int DIE = 0;
static void sigma(int __attribute__((unused)) x) {
	if (++DIE >= 20) abort();
}

static void help() {
	fprintf(stderr,"avicutter -i <source AVI> --start <time> --end <time> -o <dest AVI>\n");
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
		avi_reader_scan_odml_index(savi);
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
public:
	std::vector<fmtinfo*>	stream_format_data;
	avi_reader*		savi;
	int			savi_fd;
};

int main(int argc,char **argv) {
	AVISource savi;

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

	savi.close();
	return 0;
}

