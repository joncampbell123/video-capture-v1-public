
#define __STDC_LIMIT_MACROS
#define __STDC_CONSTANT_MACROS

#include <sys/un.h>
#include <sys/vfs.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <dirent.h>
#include <sys/sysmacros.h>	/* major(), minor() */
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <sched.h>

#include <pthread.h> /* portable thread library -- we're multi-threaded now! */

#include <alsa/asoundlib.h>	/* ALSA sound library */

#include <poll.h>
#include <linux/videodev2.h>	/* Linux Video4Linux 2 capture interface */

#include "now.h"
#include "live_feed.h"
#include "avi_writer.h"

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

/* comment out this define if your AVI files come out corrupt when stopping capture.
 * the intent is that AVI closure carry out AVI index writing away from the main
 * thread so this program is free to feed live video and capture more */
#define ASYNC_AVI_CLOSURE

#define AVI_FRAMES_PER_GROUP		15
#define AVI_B_FRAMES			0

using namespace std;

#include <list>
#include <vector>
#include <algorithm>
#include <string>

enum AVIPacketStream {
    AVI_STREAM_NONE=0,
    AVI_STREAM_AUDIO,
    AVI_STREAM_VIDEO,
    AVI_STREAM_VBI
};

class AVIPacket {
public:
    AVIPacket() : avi_flags(0), data(NULL), data_length(0), stream(AVI_STREAM_NONE), target_chunk(0), sequence(-1LL) {
    }
    ~AVIPacket() {
        free_data();
    }
public:
    void set_flags(const uint32_t flags) {
        avi_flags = flags;
    }
    bool set_data(const unsigned char *src,size_t len) {
        if (data != NULL) free_data();
        if (len == 0) return true;

        data = new unsigned char[len];
        if (data == NULL) return false;
        data_length = len;
        memcpy(data,src,len);

        return true;
    }
    void free_data(void) {
        if (data != NULL) {
            delete[] data;
            data = NULL;
        }
        data_length = 0;
    }
public:
    uint32_t                avi_flags;
    unsigned char*          data;
    size_t                  data_length;
    enum AVIPacketStream    stream;
    unsigned long long      target_chunk;
    signed long long        sequence;
};

/* WARNING: In async mode, this queue is used by main thread and disk I/O thread.
 *          Both must mediate access using a mutex!
 *
 *          Push data to the end, pull from the beginning */
std::list<AVIPacket*>       async_avi_queue;
pthread_mutex_t             async_avi_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_t                   async_avi_thread;
volatile bool               async_avi_thread_running = false;
volatile bool               async_avi_thread_die = false;
signed long long            async_avi_queue_counter = 0;

void async_avi_queue_lock(void) {
    if (pthread_mutex_lock(&async_avi_queue_mutex) != 0) {
        fprintf(stderr,"Mutex queue lock failed\n");
        abort();
    }
}

void async_avi_queue_unlock(void) {
    if (pthread_mutex_unlock(&async_avi_queue_mutex) != 0) {
        fprintf(stderr,"Mutex queue unlock failed\n");
        abort();
    }
}

bool async_avi_queue_trylock(void) {
    if (pthread_mutex_trylock(&async_avi_queue_mutex) != 0)
        return false;

    return true;
}

void *async_avi_thread_proc(void *arg);

void async_avi_thread_start(void) {
    if (!async_avi_thread_running) {
        assert(async_avi_thread_die == false);

        async_avi_queue_counter = 0;
        async_avi_thread_running = true;
        if (pthread_create(&async_avi_thread,NULL,async_avi_thread_proc,NULL)) {
            fprintf(stderr,"Async AVI thread start: failed\n");
            abort();
        }

        fprintf(stderr,"Asynchronous I/O thread started\n");
    }
}

void async_avi_thread_stop(void) {
    if (async_avi_thread_running) {
        async_avi_thread_die = true;
        fprintf(stderr,"Stopping async I/O thread\n");
        if (pthread_join(async_avi_thread,NULL) != 0) {
            fprintf(stderr,"pthread_join() failed stopping async AVI thread\n");
            abort();
        }

        /* thread should clear both on exit */
        assert(async_avi_thread_running == false);
        assert(async_avi_thread_die == false);
    }
}

/* WARNING: Do not call unless disk I/O thread is stopped */
void flush_async_queue(void) {
    if (async_avi_thread_running) {
        fprintf(stderr,"BUG: flush_async_queue() while AVI async I/O thread is running\n");
        abort();
    }

    if (!async_avi_queue.empty())
        fprintf(stderr,"Async I/O queue: Dropping %zu items\n",async_avi_queue.size());

    while (!async_avi_queue.empty()) {
        AVIPacket *pkt = async_avi_queue.front();
        async_avi_queue.pop_front();
        delete pkt;
    }
}

static unsigned int		audio_sum_count = 0;
static unsigned int		audio_sum_level[4] = {0,0,0,0};
static unsigned int		audio_max_level[4] = {0,0,0,0};

static struct v4l2_capability	v4l_vbi_cap;
static struct v4l2_format	v4l_vbi_capfmt;

static bool                 jpeg_yuv = false;
/* -watch-format: restart capture (and the AVI file) when the input format changes */
static bool			watch_format = false;
static bool			watch_format_events = false;	/* driver sends V4L2_EVENT_SOURCE_CHANGE */
static double			watch_format_next_check = 0;
static struct v4l2_pix_format	watch_format_pix;		/* format capture was started with */
static bool			avi_restart_announce = false;	/* tell the GUI about the new AVI */

static unsigned char*		vbi_buffer = NULL;		/* [vbi_width * vbi_height] */
static unsigned int		vbi_read_field = 0;

static bool			swap_fields = false;

static bool             async_io = false;

static double			avi_audio_err = 0;
static snd_pcm_hw_params_t*	alsa_hw_params = NULL;
static snd_pcm_t*		alsa_pcm = NULL;

/* audio monitor: play the captured audio out to a sound card while capturing */
static bool			audio_monitor = false;			/* user wants to listen */
static string			audio_monitor_device = "default";	/* ALSA playback device */
static double			alsa_play_retry = 0;			/* do not retry a failed open before this time */

static volatile int		DIE = 0;

static FILE*			AVI_logfile = NULL;

static int			vbi_fd = -1;
static int			vbi_width,vbi_height,vbi_stride;
/* height of the recorded VBI picture: 4:2:0 encoders need an even height (HD raw VBI has
 * an odd line count, e.g. 720p lines 7-25 = 19): one black line added below */
#define vbi_enc_height		(vbi_height + (vbi_height & 1))
static bool			capture_vbi = false;
static bool			capture_all_vbi = false;

static string			input_standard;
static string			input_device;
static string			audio_device;
static string			avi_file;
static avi_writer*		AVI = NULL;
static avi_writer_stream*	AVI_audio = NULL;
static avi_writer_stream*	AVI_video = NULL;
static avi_writer_stream*	AVI_vbi_video = NULL;
static windows_WAVEFORMATEX	AVI_audio_fmt;
static windows_BITMAPINFOHEADER	AVI_video_fmt;
static windows_BITMAPINFOHEADER	AVI_vbi_video_fmt;
static double			avi_file_start_time = -1;
static double			avi_file_start_monotime = -1;
static unsigned long long	avi_vbi_frame_counter = 0;
static unsigned long long	avi_frame_counter = 0;
static unsigned long long	avi_audio_samples = 0;
static double			preview_fps = 30.0;
static double			v4l_device_retry = 0;
static int			v4l_index = -2;
static bool			v4l_open_shut_up = 0;
static bool         v4l_vbi_via_sysfs = true;

static unsigned long long v4l_video_async_track = 0;

static unsigned long long v4l_last_frame = 0,v4l_last_vbi = 0;
static unsigned long long v4l_last_frame_delta = 0,v4l_last_vbi_delta = 0;

#define CROP_DEFAULT		-9999

static int			v4l_codec_yshr = 1;
static string			want_codec_sel;		// h264, h264-422
static bool			v4l_crop_bounds = false;
static bool			v4l_crop_defrect = false;
static int			v4l_open_vbi_first = -1;		// -1 = auto  0 = no  1 = yes
static int			v4l_crop_vcr_hack = 0;
static int			v4l_crop_x = CROP_DEFAULT;
static int			v4l_crop_y = CROP_DEFAULT;
static int			v4l_crop_w = CROP_DEFAULT;
static int			v4l_crop_h = CROP_DEFAULT;
static int			v4l_width = 720,v4l_height = 480;
static double			want_fps = -1;
static int			want_width = -1,want_height = -1;
static int			v4l_framerate_n = 30000,v4l_framerate_d = 1001;
static int			v4l_force_interlaced = -2;
static int			v4l_interlaced = 0;
static int			v4l_interlaced_woven = 0;
static bool			v4l_std_625 = false; /* current standard is 625/50 (V4L2_FIELD_INTERLACED = top field first) */
static bool			v4l_std_known = false; /* VIDIOC_G_STD worked (analog TV input) */
static int			v4l_framesize = 0;
static int			v4l_stride = 720;
static int			v4l_width_stride = 720;
static int			v4l_buffers = 0;
static int			v4l_bufptr = 0;
static char			v4l_devname[64];
static int			v4l_fd = -1;
static struct v4l2_buffer	v4l_buf[30];
static unsigned char*		v4l_ptr[30]={NULL};
static double			v4l_basetime = -1;
static int			v4l_brightness = 0; /* -10000...10000 */
static int			v4l_contrast = 0; /* -10000...10000 */

static int field_opposite(const int x) {
	if (x >= 0)
		return x ^ 1;
	else
		return x;
}

static int effective_v4l_interlaced(void) {
    if (v4l_force_interlaced >= -1)
        return swap_fields ? field_opposite(v4l_force_interlaced) : v4l_force_interlaced;
    else
        return swap_fields ? field_opposite(v4l_interlaced) : v4l_interlaced;
}

static struct v4l2_format	v4l_fmt;

/* Formats whose buffers hold a whole interlaced frame (both fields) */
static bool v4l2_field_is_frame(const unsigned int f) {
	return	f == V4L2_FIELD_INTERLACED || f == V4L2_FIELD_INTERLACED_TB || f == V4L2_FIELD_INTERLACED_BT ||
		f == V4L2_FIELD_SEQ_TB || f == V4L2_FIELD_SEQ_BT;
}

/* Set v4l_interlaced (0 = top field first, 1 = bottom field first, -1 = progressive)
 * and v4l_interlaced_woven (1 = fields interleaved line by line, 0 = one field after
 * the other) from a V4L2 field value (format or dequeued buffer). */
static void v4l_set_field_order(const unsigned int field) {
	if ((field == V4L2_FIELD_TOP || field == V4L2_FIELD_BOTTOM) && v4l2_field_is_frame(v4l_fmt.fmt.pix.field)) {
		/* A lone field in a frame format, marked by the driver (e.g. gvusb2 "Deliver Incomplete
		 * Frames"): the buffer is still a woven frame with the missing field's lines black.
		 * Keep the stream's field order instead of switching to progressive. */
		if (v4l_interlaced < 0) v4l_interlaced = v4l_std_625 ? 0 : 1;
		v4l_interlaced_woven = (v4l_fmt.fmt.pix.field == V4L2_FIELD_SEQ_TB || v4l_fmt.fmt.pix.field == V4L2_FIELD_SEQ_BT) ? 0 : 1;
		return;
	}

	v4l_interlaced = -1;
	v4l_interlaced_woven = 1;
	if (field == V4L2_FIELD_INTERLACED)
		v4l_interlaced = v4l_std_625 ? 0 : 1; /* temporal order depends on the standard */
	else if (field == V4L2_FIELD_INTERLACED_BT)
		v4l_interlaced = 1;
	else if (field == V4L2_FIELD_INTERLACED_TB)
		v4l_interlaced = 0;
	else if (field == V4L2_FIELD_SEQ_TB)
		{ v4l_interlaced = 0; v4l_interlaced_woven = 0; }
	else if (field == V4L2_FIELD_SEQ_BT)
		{ v4l_interlaced = 1; v4l_interlaced_woven = 0; }
	/* NONE, TOP, BOTTOM, ALTERNATE (single-field images): progressive */
}

static struct v4l2_capability	v4l_caps;

static int			audio_rate = 0;
static int			audio_channels = 0;

/* Live feed to capture program */
static int			live_shm_fd = -1;
static unsigned char*		live_shm = NULL;
static size_t			live_shm_size = 0;
static size_t			live_shm_size_req = 8 * 1024 * 1024;
static uint32_t			live_shm_gen = 11111;
static string			live_shm_name;

/* FFMPEG MPEG-4 encoder */
static const AVCodec*		fmp4_codec=NULL;
static AVCodecContext*		fmp4_context=NULL;
/* libx264/libx265 copy the picture into their own frames inside avcodec_send_frame():
 * hand them a reference-counted wrapper so libavcodec does not copy it first (a whole
 * frame of av_image_copy per frame: ~250 MB/s at 1080p60 4:2:2) */
static bool			fmp4_copies_input = false;
static void			no_free(void *,uint8_t *) { }
/* VAAPI hardware encoding (-codec h264-vaapi / h265-vaapi; Intel Quick Sync through libva):
 * 4:2:0 (NV12 surfaces) only. Falls back to the software encoder when libavcodec has no
 * VAAPI encoder or no render node works. */
static string			vaapi_device_sel = "auto";	/* -vaapi-device: render node or "auto" */
static AVBufferRef*		vaapi_device = NULL;		/* hw device, kept across files */
static string			vaapi_device_path;
static AVBufferRef*		vaapi_frames = NULL;		/* surface pool of the open encoder */
static AVFrame*			vaapi_nv12 = NULL;		/* NV12 staging frame for the upload */
static bool			fmp4_vaapi = false;		/* the open encoder is a VAAPI one */

static const AVCodec*		fmp4_vbi_codec=NULL;
static AVCodecContext*		fmp4_vbi_context=NULL;

static uint8_t			fmp4_temp[4*1024*1024];
static uint8_t			fmp4_yuv[(1920*1080)+((1920/2)*(1080/2)*2)];

static int			auto_v4l_avi = 1;
static int			auto_v4l_v4l = 1;

static char			file_prefix[256];

static int			comm_socket_fd = -1;
static string			comm_socket;

static string			capture_path = "/mnt/main/camera-switching";

static volatile struct live_shm_header *live_shm_head() {
	return (volatile struct live_shm_header*)live_shm;
}

/* copy the latest complete VBI frame (both fields) to the live feed for display */
static void vbi_publish_live() {
	if (live_shm == NULL || vbi_buffer == NULL) return;

	volatile struct live_shm_header *xx = live_shm_head();
	if (xx->header != LIVE_SHM_HEADER || xx->vbi_width == 0 || xx->vbi_width != (uint32_t)vbi_width) return;

	const size_t lines = (size_t)xx->vbi_count[0] + (size_t)xx->vbi_count[1];
	if (lines > (size_t)vbi_height) return;
	if ((size_t)xx->vbi_offset + (lines * (size_t)vbi_width) > live_shm_size) return;

	xx->vbi_seq = xx->vbi_seq + 1; /* odd: writing */
	__sync_synchronize();
	memcpy(live_shm + xx->vbi_offset,vbi_buffer,lines * (size_t)vbi_width);
	__sync_synchronize();
	xx->vbi_seq = xx->vbi_seq + 1; /* even: complete */
}

static int how_many_cpus() {
	int number_of_cpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (number_of_cpus < 1) number_of_cpus = 1;
	return number_of_cpus;
}

bool async_avi_queue_add(AVIPacket **pkt) { // you give ownership of the pointer to the I/O thread.
    assert(pkt != NULL);
    assert(async_avi_thread_running);

    if (*pkt != NULL) {
        AVIPacket *p = *pkt;
        *pkt = NULL;

        p->sequence = async_avi_queue_counter++;

        async_avi_queue_lock();
        async_avi_queue.push_back(p);
        async_avi_queue_unlock();
    }

    return true;
}

void *async_avi_thread_proc(void *arg) {
    signed long long expect_pkt = 0;

    while (!async_avi_thread_die) {
        if (async_avi_queue_trylock()) {
            if (!async_avi_queue.empty()) {
                // something in the queue. take it and unlock immediately.
                int patience = 1024;
                AVIPacket *pkt = async_avi_queue.front();
                async_avi_queue.pop_front();
                async_avi_queue_unlock();

                if (pkt->sequence != expect_pkt) {
                    fprintf(stderr,"Async disk I/O thread warning: Packet sequence mismatch expect:%llu got:%llu\n",
                        expect_pkt,pkt->sequence);
                }

                expect_pkt = pkt->sequence + 1;

#if 0
                fprintf(stderr,"PACKET: data=%p len=%zu flags=0x%x stream=%u target_chunk=%llu\n",
                    pkt->data,pkt->data_length,pkt->avi_flags,pkt->stream,pkt->target_chunk);
#endif
                assert(AVI != NULL);

                switch (pkt->stream) {
                    case AVI_STREAM_AUDIO:
                        assert(AVI_audio != NULL);
                        assert(pkt->target_chunk == 0);
                        avi_writer_stream_write(AVI, AVI_audio, pkt->data, pkt->data_length, pkt->avi_flags);
                        break;
                    case AVI_STREAM_VIDEO:
                        assert(AVI_video != NULL);

                        while (AVI_video->sample_write_chunk < pkt->target_chunk && patience-- > 0)
                            avi_writer_stream_write(AVI,AVI_video,NULL,0,0);

                        avi_writer_stream_write(AVI, AVI_video, pkt->data, pkt->data_length, pkt->avi_flags);
                        break;
                    case AVI_STREAM_VBI:
                        assert(AVI_vbi_video != NULL);

                        while (AVI_vbi_video->sample_write_chunk < pkt->target_chunk && patience-- > 0)
                            avi_writer_stream_write(AVI,AVI_vbi_video,NULL,0,0);

                        avi_writer_stream_write(AVI, AVI_vbi_video, pkt->data, pkt->data_length, pkt->avi_flags);
                        break;
                    default:
                        abort();
                        break;
                }

                delete pkt;
            }
            else {
                // nothing in the queue, unlock and sleep
                async_avi_queue_unlock();
                usleep(10000);
            }
        }
        else {
            // failed to lock, sleep and try again
            usleep(1000);
        }
    }

    async_avi_thread_running = false;
    async_avi_thread_die = false;
    return NULL;
}

static int sched_self_realtime() {
	struct sched_param p;
	memset(&p,0,sizeof(p));
	p.sched_priority = 99; /* linux tools like procps interpret this as "RT" */
	return sched_setscheduler(0,SCHED_FIFO,&p);
}

static const char *auth_sw = "Jonathan's videocap-v1";
static const char *auth_sw_isft = "video-capture-v1-public (https://github.com/joncampbell123/video-capture-v1-public)";

static void close_avi_file_doit(avi_writer *avi) {
	avi_writer_end_data(avi);
	avi_writer_finish(avi);
	avi_writer_close_file(avi);
	avi_writer_destroy(avi);
	fprintf(stderr,"AVI file closed\n");
}

static void *close_avi_file_thread(void *x) {
	close_avi_file_doit((struct avi_writer*)x);
	return NULL;
}

static list<pthread_t> close_threads;

static void close_avi_file() {
    async_avi_thread_stop();
    flush_async_queue();

    assert(async_avi_thread_running == false);

	if (AVI_logfile != NULL) {
		{
			time_t n = time(NULL);

			fprintf(AVI_logfile,"Capture stopped %s",ctime(&n)); /* ctime() ends with newline */
		}

		fclose(AVI_logfile);
		AVI_logfile = NULL;
	}
	if (AVI != NULL) {
#ifdef ASYNC_AVI_CLOSURE
		pthread_t xx;

		/* don't let TOO many of these pile up!
		 * OSes do have a finite limit on threads */
		while (close_threads.size() >= 4) {
			xx = close_threads.front();
			close_threads.pop_front();
			if (pthread_join(xx,NULL) != 0)
				fprintf(stderr,"pthread_join() failed\n");
		}

		/* the index writing process can take a long time especially
		 * for long captures. spin that off into a separate thread
		 * so the main thread can quickly carry on.
		 *
		 * ..or else, have your clients complain that the video "freezes"
		 * for a substantial amount of time after recording long segments */
		if (pthread_create(&xx,NULL,close_avi_file_thread,(void*)AVI) == 0) {
			close_threads.push_back(xx);
			usleep(10000);
		}
		else {
			fprintf(stderr,"WARNING: pthread_create() failed, running AVI closure directly.\n");
			close_avi_file_doit(AVI);
		}
#else
		close_avi_file_doit(AVI);
#endif

		AVI = NULL;
		AVI_audio = NULL;
		AVI_video = NULL;
		avi_file = "";
	}
	if (fmp4_vbi_context != NULL) {
		avcodec_close(fmp4_vbi_context);
		av_free(fmp4_vbi_context);
		fmp4_vbi_context = NULL;
	}
	if (fmp4_context != NULL) {
		avcodec_close(fmp4_context);
		av_buffer_unref(&fmp4_context->hw_frames_ctx);
		av_free(fmp4_context);
		fmp4_context = NULL;
	}
	av_buffer_unref(&vaapi_frames);
	av_frame_free(&vaapi_nv12);
	fmp4_vaapi = false;
}

static unsigned int gcd_u(unsigned int a,unsigned int b) {
	while (b != 0) { unsigned int t = a % b; a = b; b = t; }
	return a;
}

/* OpenDML 1.02 video properties header (LIST:strl/vprp) for the video stream.
 * This is how an AVI file states that frames are interlaced and which field comes
 * first, for any codec, including codecs that cannot signal it themselves
 * (HuffYUV has only an "interlaced" bit, Ut Video nothing).
 *
 * FieldInfo[] lists the fields in temporal order. VideoYValidStartLine is 0 for the
 * top field and 1 for the bottom field, the same convention FFmpeg's AVI muxer uses,
 * so a top-field-first file has start lines 0,1 and a bottom-field-first file 1,0. */
static bool write_video_vprp(avi_writer_stream *s) {
	unsigned char tmp[sizeof(riff_vprp_VideoPropHeader) + (2 * sizeof(riff_vprp_VIDEO_FIELD_DESC))];
	riff_vprp_VideoPropHeader *h = (riff_vprp_VideoPropHeader*)tmp;
	riff_vprp_VIDEO_FIELD_DESC *fd = (riff_vprp_VIDEO_FIELD_DESC*)(tmp + sizeof(*h));
	const int order = effective_v4l_interlaced();
	const unsigned int fields = (order >= 0) ? 2 : 1;
	const unsigned int w = (unsigned int)v4l_width,ht = (unsigned int)v4l_height;
	unsigned int i;

	memset(tmp,0,sizeof(tmp));

	/* VideoFormatToken: 0=unknown 1=PAL square 2=PAL CCIR 601 3=NTSC square 4=NTSC CCIR 601
	 * VideoStandard: 0=unknown 1=PAL 2=NTSC 3=SECAM */
	if (v4l_std_known) {
		h->VideoStandard = v4l_std_625 ? 1 : 2;
		if (w == 720 && ht == (v4l_std_625 ? 576u : 480u))
			h->VideoFormatToken = v4l_std_625 ? 2 : 4;
		else if (w == (v4l_std_625 ? 768u : 640u) && ht == (v4l_std_625 ? 576u : 480u))
			h->VideoFormatToken = v4l_std_625 ? 1 : 3;

		h->dwVTotalInLines = v4l_std_625 ? 625 : 525;
		if (h->VideoFormatToken == 2 || h->VideoFormatToken == 4)
			h->dwHTotalInT = v4l_std_625 ? 864 : 858; /* 13.5 MHz samples per line */
		else
			h->dwHTotalInT = w;
		h->dwFrameAspectRatio = (4u << 16) | 3u; /* analog TV picture: 4:3 (x in the high word) */
	}
	else {
		unsigned int g = gcd_u(w,ht);
		h->dwHTotalInT = w;
		h->dwVTotalInLines = ht;
		if (g != 0) h->dwFrameAspectRatio = ((w / g) << 16) | (ht / g);
	}

	/* vertical refresh in Hz: the field rate for interlaced video */
	if (v4l_framerate_d > 0)
		h->dwVerticalRefreshRate = (uint32_t)(((uint64_t)v4l_framerate_n * fields + (v4l_framerate_d / 2)) / v4l_framerate_d);

	h->dwFrameWidthInPixels = w;
	h->dwFrameHeightInLines = ht;
	h->nbFieldPerFrame = fields;

	for (i=0;i < fields;i++) {
		fd[i].CompressedBMHeight = ht / fields;
		fd[i].CompressedBMWidth = w;
		fd[i].ValidBMHeight = ht / fields;
		fd[i].ValidBMWidth = w;
		fd[i].ValidBMXOffset = 0;
		fd[i].ValidBMYOffset = 0;
		fd[i].VideoXOffsetInT = 0;
		if (fields == 2)
			fd[i].VideoYValidStartLine = (order == 0) ? i : (i ^ 1); /* first entry = first field in time */
		else
			fd[i].VideoYValidStartLine = 0;
	}

	fprintf(stderr,"AVI vprp: %u field(s)%s, standard %u, format token %u, %u Hz\n",
		fields,fields == 2 ? (order == 0 ? " top field first" : " bottom field first") : "",
		(unsigned int)h->VideoStandard,(unsigned int)h->VideoFormatToken,(unsigned int)h->dwVerticalRefreshRate);

	return avi_writer_stream_set_vprp(s,tmp,sizeof(*h) + (fields * sizeof(*fd))) != 0;
}

/* VAAPI device: -vaapi-device, or the first render node that opens (a card may have
 * several: Intel + another GPU) */
static bool vaapi_open_device(void) {
	if (vaapi_device != NULL)
		return true;

	std::vector<string> nodes;
	if (vaapi_device_sel != "auto" && vaapi_device_sel != "")
		nodes.push_back(vaapi_device_sel);
	else
		for (unsigned int n=128;n < 136;n++)
			nodes.push_back("/dev/dri/renderD" + std::to_string(n));

	for (size_t i=0;i < nodes.size();i++) {
		struct stat st;

		if (stat(nodes[i].c_str(),&st) != 0) continue;
		if (av_hwdevice_ctx_create(&vaapi_device,AV_HWDEVICE_TYPE_VAAPI,nodes[i].c_str(),NULL,0) == 0) {
			vaapi_device_path = nodes[i];
			fprintf(stderr,"VAAPI: device %s\n",nodes[i].c_str());
			return true;
		}
		fprintf(stderr,"VAAPI: %s does not open\n",nodes[i].c_str());
	}
	return false;
}

/* NV12 surface pool at the capture size + the staging frame */
static bool vaapi_make_frames(void) {
	AVHWFramesContext *fc;

	av_buffer_unref(&vaapi_frames);
	av_frame_free(&vaapi_nv12);
	if ((vaapi_frames = av_hwframe_ctx_alloc(vaapi_device)) == NULL)
		return false;
	fc = (AVHWFramesContext*)vaapi_frames->data;
	fc->format = AV_PIX_FMT_VAAPI;
	fc->sw_format = AV_PIX_FMT_NV12;
	fc->width = v4l_width;
	fc->height = v4l_height;
	fc->initial_pool_size = 16;
	if (av_hwframe_ctx_init(vaapi_frames) < 0) {
		fprintf(stderr,"VAAPI: no %ux%u NV12 surfaces: software encoding\n",v4l_width,v4l_height);
		av_buffer_unref(&vaapi_frames);
		return false;
	}
	vaapi_nv12 = av_frame_alloc();
	vaapi_nv12->format = AV_PIX_FMT_NV12;
	vaapi_nv12->width = v4l_width;
	vaapi_nv12->height = v4l_height;
	if (av_frame_get_buffer(vaapi_nv12,64) < 0) {
		av_frame_free(&vaapi_nv12);
		av_buffer_unref(&vaapi_frames);
		return false;
	}
	return true;
}

/* planar 4:2:0 frame (the live feed / encoder input) -> NV12 -> a VAAPI surface; the
 * returned frame goes to avcodec_send_frame() instead of the planar one */
static AVFrame *vaapi_upload(const AVFrame *src) {
	AVFrame *hw;
	int y,x;

	if (av_frame_make_writable(vaapi_nv12) < 0)
		return NULL;
	for (y=0;y < src->height;y++)
		memcpy(vaapi_nv12->data[0] + (size_t)y * vaapi_nv12->linesize[0],
			src->data[0] + (size_t)y * src->linesize[0],(size_t)src->width);
	for (y=0;y < (src->height >> 1);y++) {
		const unsigned char *u = src->data[1] + (size_t)y * src->linesize[1];
		const unsigned char *v = src->data[2] + (size_t)y * src->linesize[2];
		unsigned char *d = vaapi_nv12->data[1] + (size_t)y * vaapi_nv12->linesize[1];

		for (x=0;x < (src->width >> 1);x++) {
			d[2*x] = u[x];
			d[2*x+1] = v[x];
		}
	}
	if ((hw = av_frame_alloc()) == NULL)
		return NULL;
	if (av_hwframe_get_buffer(vaapi_frames,hw,0) < 0 ||
		av_hwframe_transfer_data(hw,vaapi_nv12,0) < 0) {
		av_frame_free(&hw);
		return NULL;
	}
	hw->pts = src->pts;
	hw->key_frame = src->key_frame;
	return hw;
}

static void open_avi_file() {
    AVCodecID codec_id = AV_CODEC_ID_H264;
	riff_avih_AVIMAINHEADER *mainhdr;
	riff_strh_AVISTREAMHEADER *strhdr;
	AVDictionary *opt_dict = NULL;
	char tmp[256];

	if (AVI != NULL)
		return;

    v4l_video_async_track = 0;

    assert(async_avi_thread_running == false);

	if (v4l_width == 0 || v4l_height == 0)
		return;
	if ((AVI = avi_writer_create()) == NULL)
		return;

	v4l_basetime = -1;
	v4l_last_frame = 0;
	v4l_last_vbi = 0;
	v4l_last_frame_delta = 0;
	v4l_last_vbi_delta = 0;

	const bool want_vaapi = (want_codec_sel == "h264-vaapi" || want_codec_sel == "h265-vaapi");

	if (want_codec_sel == "h265" || want_codec_sel == "h265-422" || want_codec_sel == "h265-vaapi")
		codec_id = AV_CODEC_ID_H265;
	else if (want_codec_sel == "h264" || want_codec_sel == "h264-422" || want_codec_sel == "h264-vaapi")
		codec_id = AV_CODEC_ID_H264;
	else if (want_codec_sel == "mpeg4" || want_codec_sel == "mpeg4-422")
		codec_id = AV_CODEC_ID_MPEG4;
	else if (want_codec_sel == "mpeg2" || want_codec_sel == "mpeg2-422")
		codec_id = AV_CODEC_ID_MPEG2VIDEO;
	else if (want_codec_sel == "huffyuv")
		codec_id = AV_CODEC_ID_FFVHUFF; /* HuffYUV only supports RGB or 4:2:2 YUV, anything else needs FFMPEG's variant of it */
	else if (want_codec_sel == "huffyuv-422")
		codec_id = AV_CODEC_ID_HUFFYUV; /* HuffYUV supports RGB or 4:2:2 YUV, this works fine */
	else if (want_codec_sel == "utvideo" || want_codec_sel == "utvideo-422")
		codec_id = AV_CODEC_ID_UTVIDEO;

	fmp4_vaapi = false;
	if (want_vaapi && vaapi_open_device()) {
		const AVCodec *c = avcodec_find_encoder_by_name(codec_id == AV_CODEC_ID_H265 ? "hevc_vaapi" : "h264_vaapi");

		if (c == NULL)
			fprintf(stderr,"VAAPI: this libavcodec has no %s encoder: software encoding\n",
				codec_id == AV_CODEC_ID_H265 ? "hevc_vaapi" : "h264_vaapi");
		else if (vaapi_make_frames()) {
			fmp4_codec = c;
			fmp4_vaapi = true;
		}
	}
	else if (want_vaapi) {
		fprintf(stderr,"VAAPI: no usable render node: software encoding\n");
	}
	if (!fmp4_vaapi && fmp4_codec != NULL && strstr(fmp4_codec->name,"_vaapi") != NULL)
		fmp4_codec = NULL;	/* was VAAPI for the previous file */
	if (fmp4_codec == NULL) {
		if ((fmp4_codec = avcodec_find_encoder(codec_id)) != NULL)
			fmp4_copies_input = !strcmp(fmp4_codec->name,"libx264") || !strcmp(fmp4_codec->name,"libx265");
		if (fmp4_codec == NULL) {
			fprintf(stderr,"FFMPEG error, cannot find H.264 encoder\n");
			close_avi_file();
			return;
		}
	}

	/* main encoder */
	if ((fmp4_context = avcodec_alloc_context3(fmp4_codec)) == NULL) {
		fprintf(stderr,"Cannot alloc context\n");
		close_avi_file();
		return;
	}
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59,18,100) /* FFMPEG 5.0.3+ */
	/* not needed */
#else
	avcodec_get_context_defaults3(fmp4_context,fmp4_codec);
#endif

	if (codec_id == AV_CODEC_ID_H265) {
		if (v4l_height >= 720) {
			if (v4l_codec_yshr == 0)
				fmp4_context->bit_rate = 22500000;
			else
				fmp4_context->bit_rate = 17500000;
		}
		else {
			if (v4l_codec_yshr == 0)
				fmp4_context->bit_rate = 15000000;
			else
				fmp4_context->bit_rate = 12000000;
		}
	}
	else if (codec_id == AV_CODEC_ID_H264) {
		if (v4l_height >= 720) {
			if (v4l_codec_yshr == 0)
				fmp4_context->bit_rate = 25000000;
			else
				fmp4_context->bit_rate = 20000000;
		}
		else {
			if (v4l_codec_yshr == 0)
				fmp4_context->bit_rate = 20000000;
			else
				fmp4_context->bit_rate = 15000000;
		}
	}
	else if (codec_id == AV_CODEC_ID_MPEG4) {
		if (v4l_height >= 720) {
			if (v4l_codec_yshr == 0)
				fmp4_context->bit_rate = 30000000;
			else
				fmp4_context->bit_rate = 25000000;
		}
		else {
			if (v4l_codec_yshr == 0)
				fmp4_context->bit_rate = 25000000;
			else
				fmp4_context->bit_rate = 20000000;
		}
	}
	else if (codec_id == AV_CODEC_ID_MPEG2VIDEO) {
		if (v4l_height >= 720) {
			if (v4l_codec_yshr == 0)
				fmp4_context->bit_rate = 75000000;
			else
				fmp4_context->bit_rate = 50000000;
		}
		else {
			if (v4l_codec_yshr == 0)
				fmp4_context->bit_rate = 35000000;
			else
				fmp4_context->bit_rate = 25000000;
		}
	}
	else if (codec_id == AV_CODEC_ID_HUFFYUV) {
		/* anything to do? */
	}
	else if (codec_id == AV_CODEC_ID_FFVHUFF) {
		/* anything to do? */
	}
	else if (codec_id == AV_CODEC_ID_UTVIDEO) {
		/* anything to do? */
	}
	else {
		abort();
	}

	fmp4_context->keyint_min = AVI_FRAMES_PER_GROUP;
	fmp4_context->time_base.num = v4l_framerate_d; /* NTS: ffmpeg means time interval as "amount of time between frames" */
	fmp4_context->time_base.den = v4l_framerate_n;
	fmp4_context->height = v4l_height;
	fmp4_context->width = v4l_width;
	fmp4_context->gop_size = AVI_FRAMES_PER_GROUP;
	fmp4_context->max_b_frames = 0; /* don't use B-frames */

	if (fmp4_vaapi) {
		fmp4_context->pix_fmt = AV_PIX_FMT_VAAPI;
		fmp4_context->hw_frames_ctx = av_buffer_ref(vaapi_frames);
		fmp4_context->rc_max_rate = fmp4_context->bit_rate;
	}
	else if (v4l_codec_yshr == 0)
		fmp4_context->pix_fmt = jpeg_yuv ? AV_PIX_FMT_YUVJ422P : AV_PIX_FMT_YUV422P;
	else
		fmp4_context->pix_fmt = jpeg_yuv ? AV_PIX_FMT_YUVJ420P : AV_PIX_FMT_YUV420P;

	fmp4_context->bit_rate_tolerance = fmp4_context->bit_rate / 2;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59,18,100) /* FFMPEG 5.0.3+ */
	/* no equivalent */
#else
	fmp4_context->noise_reduction = 0;
#endif
	fmp4_context->spatial_cplx_masking = 0.0;
	fmp4_context->temporal_cplx_masking = 0.0;
	fmp4_context->p_masking = 0.0;
	fmp4_context->nsse_weight = 0;
	fmp4_context->flags = 0;
	fmp4_context->flags2 = 0;
	fmp4_context->qmin = 1;
	fmp4_context->qmax = 63;
//	fmp4_context->me_range = 8;
//	fmp4_context->dia_size = -3;
	if (codec_id == AV_CODEC_ID_MPEG4 || codec_id == AV_CODEC_ID_MPEG2VIDEO) {
		fmp4_context->rc_max_rate = fmp4_context->bit_rate * 2;
		fmp4_context->rc_min_rate = fmp4_context->bit_rate / 15;
		/* they cannot match else these codecs complain about rc_eq */
	}
	else {
		fmp4_context->rc_max_rate = fmp4_context->bit_rate;
		fmp4_context->rc_min_rate = fmp4_context->bit_rate;
	}
//	fmp4_context->pre_dia_size = 3;
	fmp4_context->rc_buffer_size = fmp4_context->bit_rate * 2;
	fmp4_context->sample_aspect_ratio.num = 1;
	fmp4_context->sample_aspect_ratio.den = 1;
	fmp4_context->thread_count = how_many_cpus();
	fmp4_context->thread_type = 0;//FF_THREAD_SLICE;
	fmp4_context->thread_type |= FF_THREAD_FRAME;
//	fmp4_context->flags2 |= CODEC_FLAG2_FAST;
//	fmp4_context->flags |= CODEC_FLAG_LOW_DELAY;
	if (effective_v4l_interlaced() >= 0 && fmp4_vaapi) {
		/* the VAAPI encoders code frames only: interlaced sources are encoded as
		 * woven frames (the AVI vprp chunk still says interlaced) */
		fmp4_context->field_order = AV_FIELD_PROGRESSIVE;
	}
	else if (effective_v4l_interlaced() >= 0) {
		fmp4_context->flags |= AV_CODEC_FLAG_INTERLACED_DCT;
		fmp4_context->field_order = (effective_v4l_interlaced() == 0) ? AV_FIELD_TT : AV_FIELD_BB;

		/* HuffYUV: predict each field on its own and set the interlaced flag in the
		 * extradata (byte 2 = 0x10). The format has no field order; that goes in the
		 * AVI vprp chunk (see write_video_vprp). */
		if (codec_id == AV_CODEC_ID_HUFFYUV || codec_id == AV_CODEC_ID_FFVHUFF)
			fmp4_context->flags |= AV_CODEC_FLAG_INTERLACED_ME;
	}
	else {
		fmp4_context->field_order = AV_FIELD_PROGRESSIVE;
	}

	fprintf(stderr,"Encoder using %u threads\n",how_many_cpus());

    if (fmp4_vaapi) {
        av_dict_set(&opt_dict,"profile",codec_id == AV_CODEC_ID_H265 ? "main" : "high",0);
        av_dict_set(&opt_dict,"rc_mode","VBR",0);
    }
    else if (codec_id == AV_CODEC_ID_H264 || codec_id == AV_CODEC_ID_H265) {
        av_dict_set(&opt_dict,"rc-lookahead","10",0); /* NTS: If latency is a problem, reduce this value */

        if (v4l_codec_yshr == 0 && codec_id == AV_CODEC_ID_H264)
            av_dict_set(&opt_dict,"profile","high422",0); /* or else it won't work */
        else if (v4l_codec_yshr == 0 && codec_id == AV_CODEC_ID_H265)
            av_dict_set(&opt_dict,"profile","main422-10",0); /* or else it won't work */
        else
            av_dict_set(&opt_dict,"profile","main",0);

        av_dict_set(&opt_dict,"preset","ultrafast",0);
        av_dict_set(&opt_dict,"x264-params","trellis=0:scenecut=0:mixed-refs=0:me=dia:subme=0:aq-mode=0:b-adapt=0:deblock=1:rc-lookahead=0:ref=0:weightb=0:weightp=0",0);
        av_dict_set(&opt_dict,"coder","vlc",0);
        av_dict_set(&opt_dict,"mbtree","1",0);
        av_dict_set(&opt_dict,"crf","1",0);
        av_dict_set(&opt_dict,"vbr","1",0);
    }
    else if (codec_id == AV_CODEC_ID_MPEG4) {
        // any tweaks?
    }
    else if (codec_id == AV_CODEC_ID_MPEG2VIDEO) {
        // any tweaks?
    }
    else if (codec_id == AV_CODEC_ID_HUFFYUV) {
        // any tweaks?
    }
    else if (codec_id == AV_CODEC_ID_FFVHUFF) {
        // any tweaks?
    }
    else if (codec_id == AV_CODEC_ID_UTVIDEO) {
        // any tweaks?
    }
    else {
        abort();
    }

	if (avcodec_open2(fmp4_context,fmp4_codec,&opt_dict)) {
		fprintf(stderr,"Cannot open codec%s\n",fmp4_vaapi ? " (VAAPI: try -vaapi-device, or a software codec)" : "");
		av_dict_free(&opt_dict);
		close_avi_file();
		return;
	}

	av_dict_free(&opt_dict);
	if (fmp4_vaapi)
		fprintf(stderr,"VAAPI: encoding with %s on %s (%ux%u NV12)\n",fmp4_codec->name,
			vaapi_device_path.c_str(),v4l_width,v4l_height);
	update_now_time();
	avi_audio_samples = 0;
	avi_frame_counter = 0;
	avi_vbi_frame_counter = 0;
	avi_file_start_time = NOW;
    avi_file_start_monotime = monoNOW;
	sprintf(tmp,"/%s%12.3f.avi",file_prefix,NOW);
	avi_file = capture_path + tmp;

	if (capture_vbi && vbi_fd >= 0) {
		if (fmp4_vbi_codec != NULL && strstr(fmp4_vbi_codec->name,"_vaapi") != NULL)
		fmp4_vbi_codec = NULL;
	if (fmp4_vbi_codec == NULL) {
			if ((fmp4_vbi_codec = avcodec_find_encoder(codec_id)) == NULL) {
				fprintf(stderr,"FFMPEG error, cannot find H.264 encoder\n");
				close_avi_file();
				return;
			}
		}

		/* main encoder */
		if ((fmp4_vbi_context = avcodec_alloc_context3(fmp4_vbi_codec)) == NULL) {
			fprintf(stderr,"Cannot alloc context\n");
			close_avi_file();
			return;
		}
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59,18,100) /* FFMPEG 5.0.3+ */
		/* not needed */
#else
		avcodec_get_context_defaults3(fmp4_vbi_context,fmp4_vbi_codec);
#endif

		fmp4_vbi_context->bit_rate = 6000000;
		fmp4_vbi_context->keyint_min = AVI_FRAMES_PER_GROUP;
		fmp4_vbi_context->time_base.num = v4l_framerate_d; /* NTS: ffmpeg means time interval as "amount of time between frames" */
		fmp4_vbi_context->time_base.den = v4l_framerate_n;
		fmp4_vbi_context->height = vbi_enc_height;
		fmp4_vbi_context->width = vbi_width;
		fmp4_vbi_context->gop_size = AVI_FRAMES_PER_GROUP;
		fmp4_vbi_context->max_b_frames = 0; /* don't use B-frames */
		/* HuffYUV has no 4:2:0 (huffyuv-422 + VBI failed with "Cannot open codec") */
		fmp4_vbi_context->pix_fmt = (codec_id == AV_CODEC_ID_HUFFYUV) ? AV_PIX_FMT_YUV422P : AV_PIX_FMT_YUV420P;
		fmp4_vbi_context->bit_rate_tolerance = fmp4_vbi_context->bit_rate / 2;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59,18,100) /* FFMPEG 5.0.3+ */
		/* no equivalent */
#else
		fmp4_vbi_context->noise_reduction = 0;
#endif
		fmp4_vbi_context->spatial_cplx_masking = 0.0;
		fmp4_vbi_context->temporal_cplx_masking = 0.0;
		fmp4_vbi_context->p_masking = 0.0;
		fmp4_vbi_context->nsse_weight = 0;
		fmp4_vbi_context->flags = 0;
		fmp4_vbi_context->flags2 = 0;
		fmp4_vbi_context->qmin = 1;
		fmp4_vbi_context->qmax = 63;
		if (codec_id == AV_CODEC_ID_MPEG4 || codec_id == AV_CODEC_ID_MPEG2VIDEO) {
			fmp4_vbi_context->rc_max_rate = fmp4_vbi_context->bit_rate * 2;
			fmp4_vbi_context->rc_min_rate = fmp4_vbi_context->bit_rate / 15;
			/* they cannot match else these codecs complain about rc_eq */
		}
		else {
			fmp4_vbi_context->rc_max_rate = fmp4_vbi_context->bit_rate;
			fmp4_vbi_context->rc_min_rate = fmp4_vbi_context->bit_rate;
			fmp4_vbi_context->rc_buffer_size = fmp4_vbi_context->bit_rate * 2;
		}
		fmp4_vbi_context->sample_aspect_ratio.num = 1;
		fmp4_vbi_context->sample_aspect_ratio.den = 1;

		av_dict_set(&opt_dict,"rc-lookahead","10",0); /* NTS: If latency is a problem, reduce this value */
		av_dict_set(&opt_dict,"preset","veryfast",0);
		av_dict_set(&opt_dict,"vbr","1",0);

		if (avcodec_open2(fmp4_vbi_context,fmp4_vbi_codec,&opt_dict)) {
			fprintf(stderr,"Cannot open codec\n");
			av_dict_free(&opt_dict);
			close_avi_file();
			return;
		}

		av_dict_free(&opt_dict);
	}

	if (!avi_writer_open_file(AVI,avi_file.c_str())) {
		fprintf(stderr,"Cannot open AVI file %s\n",avi_file.c_str());
		close_avi_file();
		return;
	}

	assert(AVI_logfile == NULL);
	string logname = avi_file + ".log";
	AVI_logfile = fopen(logname.c_str(),"wb");
	if (AVI_logfile == NULL) {
		fprintf(stderr,"Failed to open log capture file\n");
		close_avi_file();
		return;
	}
	setbuf(AVI_logfile,NULL);

	{
		time_t n = time(NULL);

		fprintf(AVI_logfile,"AVI capture started for file %s\n",avi_file.c_str());
		fprintf(AVI_logfile,"Capture started %s",ctime(&n)); /* ctime() ends with newline */
		fprintf(AVI_logfile,"Capture is %u x %u with audio at %uHz %u-channel\n",
			v4l_width,v4l_height,audio_rate,audio_channels);
	}

	AVI_video_fmt.biXPelsPerMeter = 0;
	AVI_video_fmt.biYPelsPerMeter = 0;
	AVI_video_fmt.biClrUsed = 0;
	AVI_video_fmt.biClrImportant = 0;

	AVI_video_fmt.biSize = sizeof(windows_BITMAPINFOHEADER);
	AVI_video_fmt.biWidth = v4l_width;
	AVI_video_fmt.biHeight = v4l_height;
	AVI_video_fmt.biPlanes = 1;
	AVI_video_fmt.biBitCount = 24;
	if (codec_id == AV_CODEC_ID_H265)
		AVI_video_fmt.biCompression = avi_fourcc_const('h','e','v','1');
	else if (codec_id == AV_CODEC_ID_H264)
		AVI_video_fmt.biCompression = avi_fourcc_const('H','2','6','4');
	else if (codec_id == AV_CODEC_ID_MPEG4)
		AVI_video_fmt.biCompression = avi_fourcc_const('F','M','P','4');
	else if (codec_id == AV_CODEC_ID_MPEG2VIDEO)
		AVI_video_fmt.biCompression = avi_fourcc_const('m','p','g','2');
	else if (codec_id == AV_CODEC_ID_HUFFYUV)
		AVI_video_fmt.biCompression = avi_fourcc_const('H','F','Y','U');
	else if (codec_id == AV_CODEC_ID_FFVHUFF)
		AVI_video_fmt.biCompression = avi_fourcc_const('F','F','V','H');
	else if (codec_id == AV_CODEC_ID_UTVIDEO)
		AVI_video_fmt.biCompression = (v4l_codec_yshr == 0) ? avi_fourcc_const('U','L','Y','2') : avi_fourcc_const('U','L','Y','0');
	else
		abort();
	AVI_video_fmt.biSizeImage = v4l_width * v4l_height * 3;
	AVI_video_fmt.biXPelsPerMeter = 0;
	AVI_video_fmt.biYPelsPerMeter = 0;
	AVI_video_fmt.biClrUsed = 0;
	AVI_video_fmt.biClrImportant = 0;

	/* HuffYUV: The bits_per_coded_sample field is very important because that is how HuffYUV differentiates between RGB and YUV */
	if (codec_id == AV_CODEC_ID_HUFFYUV || codec_id == AV_CODEC_ID_FFVHUFF || codec_id == AV_CODEC_ID_UTVIDEO)
		AVI_video_fmt.biBitCount = fmp4_context->bits_per_coded_sample;

	if (audio_channels != 0) {
		AVI_audio_fmt.wFormatTag = windows_WAVE_FORMAT_PCM;
		AVI_audio_fmt.nChannels = audio_channels;
		AVI_audio_fmt.nSamplesPerSec = audio_rate;
		AVI_audio_fmt.wBitsPerSample = 16;
		AVI_audio_fmt.nBlockAlign = 2 * audio_channels;
		AVI_audio_fmt.nAvgBytesPerSec = AVI_audio_fmt.nSamplesPerSec * AVI_audio_fmt.nBlockAlign;
		AVI_audio_fmt.cbSize = 0;
	}
	else {
		memset(&AVI_audio_fmt,0,sizeof(AVI_audio_fmt));
	}

	if ((mainhdr = avi_writer_main_header(AVI)) == NULL) {
		fprintf(stderr,"Unable to create stream header\n");
		close_avi_file();
		return;
	}
	mainhdr->dwMicroSecPerFrame =		((1000 * v4l_framerate_d) / v4l_framerate_n) * 1000;
	mainhdr->dwMaxBytesPerSec =		v4l_width * v4l_height * 40;
	mainhdr->dwPaddingGranularity =		0;
	mainhdr->dwFlags =			riff_avih_AVIMAINHEADER_flags_MUSTUSEINDEX |
						riff_avih_AVIMAINHEADER_flags_HASINDEX |
						riff_avih_AVIMAINHEADER_flags_ISINTERLEAVED |
						riff_avih_AVIMAINHEADER_flags_TRUSTCKTYPE;
	mainhdr->dwSuggestedBufferSize =	v4l_width * v4l_height * 2;
	mainhdr->dwWidth =			v4l_width;
	mainhdr->dwHeight =			v4l_height;

	if (audio_channels != 0) {
		if ((AVI_audio = avi_writer_new_stream(AVI)) == NULL) {
			fprintf(stderr,"Unable to create audio stream\n");
			close_avi_file();
			return;
		}

		if (!avi_writer_stream_set_format(AVI_audio,&AVI_audio_fmt,sizeof(windows_WAVEFORMAT))) {
			fprintf(stderr,"Unable to set audio stream format\n");
			close_avi_file();
			return;
		}
		if ((strhdr = avi_writer_stream_header(AVI_audio)) == NULL) {
			fprintf(stderr,"Unable to set audio stream header\n");
			close_avi_file();
			return;
		}
		strhdr->fccType =			avi_fccType_audio;
		strhdr->fccHandler =			0;
		strhdr->dwFlags =			0;
		strhdr->dwScale =			1;
		strhdr->dwRate =			audio_rate;
		strhdr->dwSuggestedBufferSize =		AVI_audio_fmt.nAvgBytesPerSec;
		strhdr->dwSampleSize =			AVI_audio_fmt.nBlockAlign;
	}
	else {
		AVI_audio = NULL;
	}

	if ((AVI_video = avi_writer_new_stream(AVI)) == NULL) {
		fprintf(stderr,"Unable to create video stream\n");
		close_avi_file();
		return;
	}

	{
		unsigned char tmp[4096];
		size_t tmp_sz = 0;

		memcpy(tmp+tmp_sz,&AVI_video_fmt,sizeof(windows_BITMAPINFOHEADER));
		tmp_sz += sizeof(windows_BITMAPINFOHEADER);

		if (fmp4_context->extradata && fmp4_context->extradata_size != 0) {
			fprintf(stderr,"Video codec: %u bytes extradata\n",fmp4_context->extradata_size);
			memcpy(tmp+tmp_sz,fmp4_context->extradata,fmp4_context->extradata_size);
			tmp_sz += fmp4_context->extradata_size;
			assert(tmp_sz <= sizeof(tmp));
		}

		if (!avi_writer_stream_set_format(AVI_video,tmp,tmp_sz)) {
			fprintf(stderr,"Unable to set audio stream format\n");
			close_avi_file();
			return;
		}
	}
	if ((strhdr = avi_writer_stream_header(AVI_video)) == NULL) {
		fprintf(stderr,"Unable to set audio stream header\n");
		close_avi_file();
		return;
	}
	strhdr->fccType =			avi_fccType_video;
    strhdr->fccHandler =        AVI_video_fmt.biCompression;
    strhdr->dwFlags =			0;
	strhdr->dwScale =			v4l_framerate_d;
	strhdr->dwRate =			v4l_framerate_n;
	strhdr->dwSuggestedBufferSize =		v4l_width * v4l_height * 2;
	strhdr->dwSampleSize =			0;
	strhdr->rcFrame.right =			v4l_width;
	strhdr->rcFrame.bottom =		v4l_height;

	if (!write_video_vprp(AVI_video)) {
		fprintf(stderr,"Unable to set video properties (vprp)\n");
		close_avi_file();
		return;
	}

	if (capture_vbi && vbi_fd >= 0) {
		if ((AVI_vbi_video = avi_writer_new_stream(AVI)) == NULL) {
			fprintf(stderr,"Unable to create video stream\n");
			close_avi_file();
			return;
		}

		AVI_vbi_video_fmt.biXPelsPerMeter = 0;
		AVI_vbi_video_fmt.biYPelsPerMeter = 0;
		AVI_vbi_video_fmt.biClrUsed = 0;
		AVI_vbi_video_fmt.biClrImportant = 0;

		AVI_vbi_video_fmt.biSize = sizeof(windows_BITMAPINFOHEADER);
		AVI_vbi_video_fmt.biWidth = vbi_width;
		AVI_vbi_video_fmt.biHeight = vbi_enc_height;
		AVI_vbi_video_fmt.biPlanes = 1;
		AVI_vbi_video_fmt.biBitCount = AVI_video_fmt.biBitCount;
		AVI_vbi_video_fmt.biCompression = AVI_video_fmt.biCompression;
		AVI_vbi_video_fmt.biSizeImage = vbi_width * vbi_enc_height * 3;
		AVI_vbi_video_fmt.biXPelsPerMeter = 0;
		AVI_vbi_video_fmt.biYPelsPerMeter = 0;
		AVI_vbi_video_fmt.biClrUsed = 0;
		AVI_vbi_video_fmt.biClrImportant = 0;

		{
			unsigned char tmp[4096];
			size_t tmp_sz = 0;

			memcpy(tmp+tmp_sz,&AVI_vbi_video_fmt,sizeof(windows_BITMAPINFOHEADER));
			tmp_sz += sizeof(windows_BITMAPINFOHEADER);

			if (fmp4_vbi_context->extradata && fmp4_vbi_context->extradata_size != 0) {
				fprintf(stderr,"VBI codec: %u bytes extradata\n",fmp4_vbi_context->extradata_size);
				memcpy(tmp+tmp_sz,fmp4_vbi_context->extradata,fmp4_vbi_context->extradata_size);
				tmp_sz += fmp4_vbi_context->extradata_size;
				assert(tmp_sz <= sizeof(tmp));
			}

			if (!avi_writer_stream_set_format(AVI_vbi_video,tmp,tmp_sz)) {
				fprintf(stderr,"Unable to set audio stream format\n");
				close_avi_file();
				return;
			}
		}

		if ((strhdr = avi_writer_stream_header(AVI_vbi_video)) == NULL) {
			fprintf(stderr,"Unable to set audio stream header\n");
			close_avi_file();
			return;
		}
		strhdr->fccType =			avi_fccType_video;
		strhdr->fccHandler =			AVI_vbi_video_fmt.biCompression;
		strhdr->dwFlags =			0;
		strhdr->dwScale =			v4l_framerate_d;
		strhdr->dwRate =			v4l_framerate_n;
		strhdr->dwSuggestedBufferSize =		vbi_width * vbi_enc_height * 2;
		strhdr->dwSampleSize =			0;
		strhdr->rcFrame.right =			vbi_width;
		strhdr->rcFrame.bottom =		vbi_enc_height;
	}

	if (!avi_writer_begin_header(AVI)) {
		fprintf(stderr,"Cannot begin AVI header\n");
		close_avi_file();
		return;
	}
	if (!avi_writer_end_header(AVI)) {
		fprintf(stderr,"Cannot end AVI header\n");
		close_avi_file();
		return;
	}
	{
		struct v4l2_format vbifmt = v4l_vbi_capfmt;
		bool write_vbi = capture_vbi && vbi_fd >= 0;
		riff_chunk chunk;
		char tmp[1024];
		struct tm *tm;
		time_t now;

		now = time(NULL);
		tm = localtime(&now);

		riff_stack_begin_new_chunk_here(AVI->riff,&chunk);
		riff_stack_set_chunk_list_type(&chunk,riff_LIST,avi_fourcc_const('I','N','F','O'));
		riff_stack_push(AVI->riff,&chunk);

		riff_stack_begin_new_chunk_here(AVI->riff,&chunk);
		riff_stack_set_chunk_data_type(&chunk,avi_fourcc_const('C','P','S','W'));
		riff_stack_push(AVI->riff,&chunk);
		riff_stack_write(AVI->riff,riff_stack_top(AVI->riff),auth_sw,strlen(auth_sw));
		riff_stack_pop(AVI->riff);

		riff_stack_begin_new_chunk_here(AVI->riff,&chunk);
		riff_stack_set_chunk_data_type(&chunk,avi_fourcc_const('I','S','F','T')); /* ISFT standard RIFF info chunk */
		riff_stack_push(AVI->riff,&chunk);
		riff_stack_write(AVI->riff,riff_stack_top(AVI->riff),auth_sw_isft,strlen(auth_sw_isft));
		riff_stack_pop(AVI->riff);

		if (tm != NULL) {
			sprintf(tmp,"%04u-%02u-%02u",tm->tm_year+1900,tm->tm_mon+1,tm->tm_mday);
			riff_stack_begin_new_chunk_here(AVI->riff,&chunk);
			riff_stack_set_chunk_data_type(&chunk,avi_fourcc_const('I','C','R','D')); /* ISFT standard RIFF info chunk */
			riff_stack_push(AVI->riff,&chunk);
			riff_stack_write(AVI->riff,riff_stack_top(AVI->riff),tmp,strlen(tmp));
			riff_stack_pop(AVI->riff);
		}

		/* it would be good for archiving purposes to record exactly which scanlines
		 * the capture card is returning to us. */
		{
			struct v4l2_crop crop;

			memset(&crop,0,sizeof(crop));
			crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			if (ioctl(v4l_fd,VIDIOC_G_CROP,&crop) == 0) {
				sprintf(tmp,"CROP %ux%u+%ux%u",
						(unsigned int)crop.c.left,
						(unsigned int)crop.c.top,
						(unsigned int)crop.c.width,
						(unsigned int)crop.c.height);

				riff_stack_begin_new_chunk_here(AVI->riff,&chunk);
				riff_stack_set_chunk_data_type(&chunk,avi_fourcc_const('V','C','R','P')); /* video crop at the time of capture */
				riff_stack_push(AVI->riff,&chunk);
				riff_stack_write(AVI->riff,riff_stack_top(AVI->riff),tmp,strlen(tmp));
				riff_stack_pop(AVI->riff);
			}
		}

		{
			struct v4l2_cropcap cropcap;

			memset(&cropcap,0,sizeof(cropcap));
			cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			if (ioctl(v4l_fd,VIDIOC_CROPCAP,&cropcap) >= 0) {
				sprintf(tmp,"CROPCAP bounds=%ux%u+%ux%u, default=%ux%u+%ux%u",
						(unsigned int)cropcap.bounds.left,
						(unsigned int)cropcap.bounds.top,
						(unsigned int)cropcap.bounds.width,
						(unsigned int)cropcap.bounds.height,
						(unsigned int)cropcap.defrect.left,
						(unsigned int)cropcap.defrect.top,
						(unsigned int)cropcap.defrect.width,
						(unsigned int)cropcap.defrect.height);

				riff_stack_begin_new_chunk_here(AVI->riff,&chunk);
				riff_stack_set_chunk_data_type(&chunk,avi_fourcc_const('V','C','R','C')); /* video crop caps */
				riff_stack_push(AVI->riff,&chunk);
				riff_stack_write(AVI->riff,riff_stack_top(AVI->riff),tmp,strlen(tmp));
				riff_stack_pop(AVI->riff);
			}
		}

		{
			const char *encodedfield_str = "";
			const char *field_str = "";
			char px[5];

			*((uint32_t*)px) = v4l_fmt.fmt.pix.pixelformat;
			px[4] = 0;

			switch (v4l_fmt.fmt.pix.field) {
				case V4L2_FIELD_ANY:		field_str = "any"; break;
				case V4L2_FIELD_NONE:		field_str = "none"; break;
				case V4L2_FIELD_TOP:		field_str = "top"; break;
				case V4L2_FIELD_BOTTOM:		field_str = "bottom"; break;
				case V4L2_FIELD_INTERLACED:	field_str = "interlaced"; break;
				case V4L2_FIELD_SEQ_TB:		field_str = "seq-tb"; break;
				case V4L2_FIELD_SEQ_BT:		field_str = "seq-bt"; break;
				case V4L2_FIELD_ALTERNATE:	field_str = "alternate"; break;
				case V4L2_FIELD_INTERLACED_TB:	field_str = "interlaced-tb"; break;
				case V4L2_FIELD_INTERLACED_BT:	field_str = "interlaced-bt"; break;
			};

			if (effective_v4l_interlaced() == 1)
				encodedfield_str = "bff";
			else if (effective_v4l_interlaced() == 0)
				encodedfield_str = "tff";
			else if (effective_v4l_interlaced() == -1)
				encodedfield_str = "p";
			else
				encodedfield_str = "?";

			sprintf(tmp,"w=%u h=%u stride=%u size=%u pixfmt='%s' field='%s' encodedfield='%s' async=%u",
					(unsigned int)v4l_fmt.fmt.pix.width,
					(unsigned int)v4l_fmt.fmt.pix.height,
					(unsigned int)v4l_fmt.fmt.pix.bytesperline,
					(unsigned int)v4l_fmt.fmt.pix.sizeimage,
					px,field_str,encodedfield_str,
					async_io?1:0);

			riff_stack_begin_new_chunk_here(AVI->riff,&chunk);
			riff_stack_set_chunk_data_type(&chunk,avi_fourcc_const('V','C','N','F')); /* video capture info */
			riff_stack_push(AVI->riff,&chunk);
			riff_stack_write(AVI->riff,riff_stack_top(AVI->riff),tmp,strlen(tmp));
			riff_stack_pop(AVI->riff);
		}

		if (write_vbi) {
			sprintf(tmp,"VBI %u+%u %u+%u",
					vbifmt.fmt.vbi.start[0],
					vbifmt.fmt.vbi.count[0],
					vbifmt.fmt.vbi.start[1],
					vbifmt.fmt.vbi.count[1]);

			riff_stack_begin_new_chunk_here(AVI->riff,&chunk);
			riff_stack_set_chunk_data_type(&chunk,avi_fourcc_const('V','B','I',' '));
			riff_stack_push(AVI->riff,&chunk);
			riff_stack_write(AVI->riff,riff_stack_top(AVI->riff),tmp,strlen(tmp));
			riff_stack_pop(AVI->riff);
		}

		riff_stack_pop(AVI->riff);
	}

	if (!avi_writer_begin_data(AVI)) {
		fprintf(stderr,"Cannot begin AVI data\n");
		close_avi_file();
		return;
	}

    assert(async_avi_thread_running == false);

    if (async_io) async_avi_thread_start();
}

static void sigma(int __attribute__((unused)) x) {
	if (++DIE >= 20) abort();
}

static void help() {
	fprintf(stderr,"camera_switching_source_sdi [options]\n");
	fprintf(stderr,"Impact Studio Pro Camera Switching system v1.x (C) 2010-2011\n");
	fprintf(stderr,"    -index <N>                             Which Video4linux capture source to use\n");
	fprintf(stderr,"    -path <path>                           Where to capture to\n");
	fprintf(stderr,"    -audio-device <device>                 ALSA device to capture from, 'auto' (ALSA default),\n");
	fprintf(stderr,"                                           or 'capture-card' (the capture card's own ALSA device)\n");
	fprintf(stderr,"    -monitor                               Play the captured audio to the monitor device\n");
	fprintf(stderr,"    -monitor-device <device>               ALSA playback device for -monitor (default 'default')\n");
	fprintf(stderr,"    -shm <name>                            Live feed shm name\n");
	fprintf(stderr,"    -shm-size <n>                          Shared memory size (in MB)\n");
	fprintf(stderr,"    -auto-open-avi <n>                     Whether to auto-open AVI output on V4L device\n");
	fprintf(stderr,"    -auto-open-v4l <n>                     Whether to auto-open V4L device\n");
	fprintf(stderr,"    -socket <n>                            Socket location\n");
	fprintf(stderr,"    -width <n>                             Capture width\n");
	fprintf(stderr,"    -height <n>                            Capture height\n");
	fprintf(stderr,"    -async-io                              Use asynchronous disk I/O (recommended)\n");
	fprintf(stderr,"    -cropbound                             Set crop rect to bounds rect\n");
	fprintf(stderr,"    -cropdef                               Set crop rect to default rect\n");
	fprintf(stderr,"    -cx <n>                                Left crop rect coordinate\n");
	fprintf(stderr,"    -cy <n>                                Top crop rect coordinate\n");
	fprintf(stderr,"    -cw <n>                                Width crop rect coordinate\n");
	fprintf(stderr,"    -ch <n>                                Height crop rect coordinate\n");
	fprintf(stderr,"    -vaapi-device <path|auto>              VAAPI render node for the -vaapi codecs (default auto)\n");
	fprintf(stderr,"    -codec <n>                             Codec.\n");
	fprintf(stderr,"                                                 mpeg2         MPEG-2 4:2:0\n");
	fprintf(stderr,"                                                 mpeg2-422     MPEG-2 4:2:2\n");
	fprintf(stderr,"                                                 mpeg4         MPEG-4 4:2:0\n");
	fprintf(stderr,"                                                 mpeg4-422     MPEG-4 4:2:2\n");
	fprintf(stderr,"                                                 h264          H.264/AVC 4:2:0\n");
	fprintf(stderr,"                                                 h264-422      H.264/AVC 4:2:2\n");
	fprintf(stderr,"                                                 h265          H.265/HEVC 4:2:0\n");
	fprintf(stderr,"                                                 h265-422      H.265/HEVC 4:2:2\n");
	fprintf(stderr,"                                                 h264-vaapi    H.264/AVC 4:2:0, VAAPI hardware (Quick Sync)\n");
	fprintf(stderr,"                                                 h265-vaapi    H.265/HEVC 4:2:0, VAAPI hardware\n");
	fprintf(stderr,"                                                 huffyuv       HuffYUV 4:2:0\n");
	fprintf(stderr,"                                                 huffyuv-422   HuffYUV 4:2:2\n");
	fprintf(stderr,"                                                 utvideo       Ut Video 4:2:0\n");
	fprintf(stderr,"                                                 utvideo-422   Ut Video 4:2:2\n");
	fprintf(stderr,"    -vbi                                   Also capture & record VBI\n");
	fprintf(stderr,"    -vbi-all                               Capture & record ALL VBI data\n");
	fprintf(stderr,"    -input-device <x>                      Card input to select\n");
	fprintf(stderr,"    -input-standard <x>                    Video standard to select\n");
	fprintf(stderr,"    -vcr-hack <x>                          Crop x lines from bottom\n");
	fprintf(stderr,"    -vbi-first <x>                         Open VBI first, then init video (1=yes 0=no -1=auto)\n");
	fprintf(stderr,"    -no-vbi-sysfs                          Don't use sysfs to find vbi device\n");
	fprintf(stderr,"    -fps <n>                               Capture fps\n");
	fprintf(stderr,"    -watch-format                          Restart capture (and the AVI file) when the input format changes\n");
	fprintf(stderr,"    -jpeg-yuv                              YUV is JPEG scale (0-255), else broadcast (16-235)\n");
	fprintf(stderr,"    -swap-fields                           Swap fields, broken capture cards\n");
	fprintf(stderr,"    -fil <top|bottom|p>                    Force field order in case the capture card fails to report it properly\n");
	fprintf(stderr,"    -bright <x>                            Adjust capture card brightness (-1.0) to 1.0)\n");
	fprintf(stderr,"    -contrast <x>                          Adjust capture card contrast (-1.0) to 1.0)\n");
}

static int parse_argv(int argc,char **argv) {
	const char *a;
	int i;

	for (i=1;i < argc;) {
		a = (const char*)argv[i++];

		if (*a == '-') {
			do { a++; } while (*a == '-');

			if (!strcmp(a,"h") || !strcmp(a,"help") || !strcmp(a,"?")) {
				help();
				return 1;
			}
			else if (!strcmp(a,"bright")) {
				a = argv[i++];
				if (a == NULL) return 1;
				v4l_brightness = (int)(atof(a) * 10000);
				if (v4l_brightness < -10000) v4l_brightness = -10000;
				if (v4l_brightness >  10000) v4l_brightness =  10000;
			}
			else if (!strcmp(a,"contrast")) {
				a = argv[i++];
				if (a == NULL) return 1;
				v4l_contrast = (int)(atof(a) * 10000);
				if (v4l_contrast < -10000) v4l_contrast = -10000;
				if (v4l_contrast >  10000) v4l_contrast =  10000;
			}
			else if (!strcmp(a,"fil")) {
				a = argv[i++];
				if (a == NULL) return 1;
				/* force interlacing if the capture card for some reason fails to report interlacing
				 * (looking at you, Linux Happauage PCI-e capture card drivers!) */
				if (!strcmp(a,"top"))
					v4l_force_interlaced = 0;
				else if (!strcmp(a,"bottom"))
					v4l_force_interlaced = 1;
				else if (!strcmp(a,"p"))
					v4l_force_interlaced = -1;
			}
			else if (!strcmp(a,"jpeg-yuv")) {
				jpeg_yuv = true;
			}
			else if (!strcmp(a,"watch-format")) {
				watch_format = true;
			}
			else if (!strcmp(a,"no-vbi-sysfs")) {
				v4l_vbi_via_sysfs = false;
			}
			else if (!strcmp(a,"vbi-first")) {
				v4l_open_vbi_first = atoi(argv[i++]);
			}
			else if (!strcmp(a,"vcr-hack")) {
				v4l_crop_vcr_hack = atoi(argv[i++]);
				if (v4l_crop_vcr_hack < 0) v4l_crop_vcr_hack = 0;
			}
			else if (!strcmp(a,"vbi")) {
				capture_vbi = true;
			}
			else if (!strcmp(a,"vbi-all")) {
				capture_all_vbi = true;
			}
			else if (!strcmp(a,"codec")) {
				want_codec_sel = argv[i++];
            }
			else if (!strcmp(a,"vaapi-device")) {
				vaapi_device_sel = argv[i++];
			}
            else if (!strcmp(a,"swap-fields")) {
                swap_fields = true;
            }
            else if (!strcmp(a,"async-io")) {
                async_io = true;
			}
			else if (!strcmp(a,"cropbound")) {
				v4l_crop_bounds = true;
			}
			else if (!strcmp(a,"cropdef")) {
				v4l_crop_defrect = true;
			}
			else if (!strcmp(a,"cx")) {
				v4l_crop_x = atoi(argv[i++]);
			}
			else if (!strcmp(a,"cy")) {
				v4l_crop_y = atoi(argv[i++]);
			}
			else if (!strcmp(a,"cw")) {
				v4l_crop_w = atoi(argv[i++]);
			}
			else if (!strcmp(a,"ch")) {
				v4l_crop_h = atoi(argv[i++]);
			}
			else if (!strcmp(a,"fps")) {
				want_fps = atof(argv[i++]);
			}
			else if (!strcmp(a,"width")) {
				want_width = atoi(argv[i++]);
			}
			else if (!strcmp(a,"height")) {
				want_height = atoi(argv[i++]);
			}
			else if (!strcmp(a,"socket")) {
				comm_socket = argv[i++];
			}
			else if (!strcmp(a,"audio-device")) {
				audio_device = argv[i++];
			}
			else if (!strcmp(a,"monitor")) {
				audio_monitor = true;
			}
			else if (!strcmp(a,"monitor-device")) {
				audio_monitor_device = argv[i++];
			}
			else if (!strcmp(a,"input-device")) {
				input_device = argv[i++];
			}
			else if (!strcmp(a,"input-standard")) {
				input_standard = argv[i++];
			}
			else if (!strcmp(a,"shm")) {
				live_shm_name = argv[i++];
			}
			else if (!strcmp(a,"shm-size")) {
				live_shm_size_req = atoi(argv[i++]);
				if (live_shm_size_req < 1) live_shm_size_req = 1;
				if (live_shm_size_req > 64) live_shm_size_req = 64;
				live_shm_size_req *= 1024*1024;
			}
			else if (!strcmp(a,"index")) {
				v4l_index = atoi(argv[i++]);
			}
			else if (!strcmp(a,"fpre")) {
				snprintf(file_prefix,sizeof(file_prefix)-1,"%s",argv[i++]);
			}
			else if (!strcmp(a,"auto-open-avi")) {
				auto_v4l_avi = atoi(argv[i++]);
			}
			else if (!strcmp(a,"auto-open-v4l")) {
				auto_v4l_v4l = atoi(argv[i++]);
			}
			else if (!strcmp(a,"pfps")) {
				preview_fps = atof(argv[i++]);
				if (preview_fps < 0.1) preview_fps = 0.1;
			}
			else if (!strcmp(a,"path")) {
				capture_path = argv[i++];
			}
			else {
				help();
				fprintf(stderr,"Unknown switch '%s'\n",a);
				return 1;
			}
		}
		else {
			fprintf(stderr,"Unknown argv %s\n",a);
			return 1;
		}
	}

	if (v4l_devname[0] == 0 && v4l_index == -2)
		v4l_index = 0;
	if (v4l_devname[0] == 0 && v4l_index >= 0)
		sprintf(v4l_devname,"/dev/video%d",v4l_index);

	if (v4l_devname[0] == 0) {
		fprintf(stderr,"You must specify a capture device. Use -help for more information\n");
		return 1;
	}

	return 0;
}

void capture_vbi_open(void) {
	if (capture_vbi) {
		char tmp[256];

        tmp[0] = 0;
		fprintf(stderr,"Opening VBI device\n");

        /* wait! given the v4l index we may be able to use the sysfs pseudo-filesystem
         * to locate for certain which vbi device is associated with it, instead of guessing.
         *
         * while most capture drivers allocate videoN and vbiN with matching N, it turns out
         * the USB versions of Happauge's WinTV capture cards don't. On my laptop, it can
         * easily end up where my webcam is video0 and the Happauge WinTV is video1 and vbi0. */
        if (v4l_vbi_via_sysfs) {
            struct dirent *d;
            char path[256];
            struct stat st;
            DIR *dir;

            sprintf(path,"/sys/class/video4linux/video%u/device/video4linux",v4l_index);

            /* A device can have several video and VBI nodes (the Sensoray 2255 has four
             * channels on one USB interface: video4-7 + vbi0-3), and readdir() order is
             * arbitrary. Pair them by rank: the k-th videoN of the device (by N) goes
             * with the k-th vbiN. With one video and one VBI node this is the old result. */
            if ((dir=opendir(path)) != NULL) {
                std::vector<unsigned int> vids,vbis;

                while ((d=readdir(dir)) != NULL) {
                    if (fstatat(dirfd(dir),d->d_name,&st,0)) /* if we can't stat it, we can't use it */
                        continue;

                    /* we're looking for directories named videoX and vbiX where X is an integer */
                    if (!strncmp(d->d_name,"vbi",3) && isdigit(d->d_name[3]))
                        vbis.push_back((unsigned int)strtoul(d->d_name+3,NULL,10));
                    else if (!strncmp(d->d_name,"video",5) && isdigit(d->d_name[5]))
                        vids.push_back((unsigned int)strtoul(d->d_name+5,NULL,10));
                }
                closedir(dir);

                std::sort(vids.begin(),vids.end());
                std::sort(vbis.begin(),vbis.end());

                if (!vbis.empty()) {
                    size_t rank = 0;

                    for (size_t i=0;i < vids.size();i++) {
                        if (vids[i] == v4l_index) {
                            rank = i;
                            break;
                        }
                    }
                    if (rank >= vbis.size()) rank = 0;

                    fprintf(stderr,"Found VBI device using sysfs. video%u -> vbi%u\n",v4l_index,vbis[rank]);
                    sprintf(tmp,"/dev/vbi%u",vbis[rank]);
                }
            }
        }

        if (tmp[0] == 0)
    		sprintf(tmp,"/dev/vbi%u",v4l_index);

		vbi_fd = open(tmp,O_RDWR);
		if (vbi_fd >= 0) {
			fprintf(stderr,"VBI device %s open\n",tmp);

			memset(&v4l_vbi_cap,0,sizeof(v4l_vbi_cap));
			if (ioctl(vbi_fd,VIDIOC_QUERYCAP,&v4l_vbi_cap) == 0) {
				if (!(v4l_vbi_cap.capabilities & V4L2_CAP_VBI_CAPTURE)) {
					fprintf(stderr,"Device does not support VBI capture\n");
					close(vbi_fd);
					vbi_fd = -1;
				}
			}
			else {
				fprintf(stderr,"Unable to query VBI\n");
				close(vbi_fd);
				vbi_fd = -1;
			}
		}
		else {
			fprintf(stderr,"Failed to open VBI device %s, %s\n",tmp,strerror(errno));
		}
	}

	if (vbi_fd >= 0) {
		v4l_vbi_capfmt.type = V4L2_BUF_TYPE_VBI_CAPTURE;
		if (ioctl(vbi_fd,VIDIOC_G_FMT,&v4l_vbi_capfmt)) {
			fprintf(stderr,"Failed to query VBI format\n");
			close(vbi_fd);
			vbi_fd = -1;
		}
		else {
			fprintf(stderr,"VBI format:\n");
			fprintf(stderr,"   sampling_rate: %uHz\n",v4l_vbi_capfmt.fmt.vbi.sampling_rate);
			fprintf(stderr,"   offset:        %u\n",v4l_vbi_capfmt.fmt.vbi.offset);
			fprintf(stderr,"   samples/line:  %u\n",v4l_vbi_capfmt.fmt.vbi.samples_per_line);
			fprintf(stderr,"   sample_fmt:    0x%x\n",v4l_vbi_capfmt.fmt.vbi.sample_format);
			fprintf(stderr,"   start:         %u, %u\n",v4l_vbi_capfmt.fmt.vbi.start[0],v4l_vbi_capfmt.fmt.vbi.start[1]);
			fprintf(stderr,"   count:         %u, %u\n",v4l_vbi_capfmt.fmt.vbi.count[0],v4l_vbi_capfmt.fmt.vbi.count[1]);
			fprintf(stderr,"   flags:         0x%x\n",v4l_vbi_capfmt.fmt.vbi.flags);

			/* we want grayscsale */
			v4l_vbi_capfmt.fmt.vbi.sample_format = V4L2_PIX_FMT_GREY;
			if (ioctl(vbi_fd,VIDIOC_S_FMT,&v4l_vbi_capfmt))
				fprintf(stderr,"Failed to set VBI format (set fmt to grey)\n");
			if (ioctl(vbi_fd,VIDIOC_G_FMT,&v4l_vbi_capfmt))
				fprintf(stderr,"Failed to get VBI format again\n");

			/* now... stride vs width vs height */
			/* note some capture cards in my collection will capture something like 1500 samples per line but
			 * return it as a bitmap 2048 bytes/line! Um.... but apparently that's been fixed? */
			vbi_height = v4l_vbi_capfmt.fmt.vbi.count[0] + v4l_vbi_capfmt.fmt.vbi.count[1];
			vbi_stride = v4l_vbi_capfmt.fmt.vbi.samples_per_line;

			if (capture_all_vbi) {
				// in case the capture card has the wrong info, use ALL the raw data
				vbi_width = vbi_stride;
			}
			else {
				vbi_width = v4l_vbi_capfmt.fmt.vbi.sampling_rate / 15750; // sample rate vs 15.750KHz NTSC horizontal sync rate
				vbi_width -= v4l_vbi_capfmt.fmt.vbi.offset;
			}

			vbi_width += 15;
			vbi_width -= vbi_width % 16;
			/* the 15.75 kHz estimate is for SD; HD raw VBI (component 720p/1080i: 74 MHz)
			 * has fewer samples per line than that: never wider than the data */
			if (vbi_width > vbi_stride)
				vbi_width = vbi_stride;
			fprintf(stderr,"VBI width %u stride %u height %u\n",
				vbi_width,
				vbi_stride,
				vbi_height);

			if (vbi_width <= 128 || vbi_stride <= 128 || vbi_height < 1) {
				fprintf(stderr,"VBI area too small\n");
				close(vbi_fd);
				vbi_fd = -1;
			}

			if (vbi_fd >= 0) {
				/* non-block */
				fcntl(vbi_fd,F_SETFL, fcntl(vbi_fd,F_GETFL) | O_NONBLOCK);

				assert(vbi_buffer == NULL);
				vbi_buffer = new unsigned char [vbi_width * (vbi_height+64)];
				memset(vbi_buffer,16,vbi_width * (vbi_height+64)); /* incl. the padding line (black) */
				vbi_read_field = 0;
			}
		}
	}
}

/* ------------------------------------------------------------------------------
 * Audio monitor
 *
 * The captured audio is copied to an ALSA playback device so the user can listen
 * for problems during capture. Recording never depends on it: the capture loop only
 * copies samples into a small ring buffer (monitor_push, never blocks), and a
 * separate thread writes them to the sound card. A sound server (PulseAudio's ALSA
 * plugin) can stall for tens of milliseconds; that thread absorbs it.
 *
 * Capture and playback run on different clocks, and the capture side arrives in
 * bursts. The ring holds at most 100 ms: when it is full the oldest samples are
 * dropped, which bounds the latency. An underrun restarts playback after a short
 * block of silence.
 * ------------------------------------------------------------------------------ */
static pthread_mutex_t		mon_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t		mon_cond = PTHREAD_COND_INITIALIZER;
static pthread_t		mon_thread;
static bool			mon_running = false;		/* thread exists (main thread's view) */
static volatile bool		mon_quit = false;		/* ask the thread to stop */
static volatile bool		mon_done = false;		/* thread has finished (e.g. open failed) */
static volatile bool		mon_failed = false;		/* it finished because the device failed */
static int16_t*			mon_ring = NULL;		/* audio waiting for the thread */
static size_t			mon_ring_frames = 0;		/* ring capacity in frames */
static size_t			mon_ring_head = 0;		/* next frame to write */
static size_t			mon_ring_fill = 0;		/* frames queued */
static int			mon_rate = 0,mon_channels = 0;
static string			mon_device;
static unsigned long long	mon_dropped = 0;		/* frames dropped to limit latency */
static unsigned long		mon_underruns = 0;

/* write about 1/4 of the device buffer as silence */
static void monitor_prefill(snd_pcm_t *pcm,snd_pcm_uframes_t buffer) {
	static int16_t silence[8 * 1024];
	snd_pcm_uframes_t want = buffer / 4;
	const snd_pcm_uframes_t chunk = sizeof(silence) / sizeof(int16_t) / (snd_pcm_uframes_t)std::max(mon_channels,1);

	memset(silence,0,sizeof(silence));
	while (want > 0 && !mon_quit) {
		snd_pcm_uframes_t n = std::min(want,chunk);
		if (snd_pcm_writei(pcm,silence,n) <= 0) break;
		want -= n;
	}
}

static void *monitor_thread_proc(void *arg) {
	snd_pcm_t *pcm = NULL;
	snd_pcm_uframes_t buffer = 0,period = 0;
	int16_t *tmp = NULL;
	size_t tmp_frames = 0;
	bool need_prefill = true;
	int err;

	(void)arg;

	if ((err = snd_pcm_open(&pcm, mon_device.c_str(), SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
		fprintf(stderr,"Audio monitor: cannot open ALSA playback device '%s', %s\n",mon_device.c_str(),snd_strerror(err));
		pcm = NULL;
		goto fail;
	}

	/* 100 ms device buffer, resampling allowed if the card cannot do the capture rate */
	if ((err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
		(unsigned int)mon_channels, (unsigned int)mon_rate, 1, 100000)) < 0) {
		fprintf(stderr,"Audio monitor: cannot set %dHz %d-channel S16LE on '%s', %s\n",
			mon_rate,mon_channels,mon_device.c_str(),snd_strerror(err));
		goto fail;
	}
	if ((err = snd_pcm_get_params(pcm, &buffer, &period)) < 0 || buffer == 0 || period == 0) {
		fprintf(stderr,"Audio monitor: cannot read buffer size, %s\n",snd_strerror(err));
		goto fail;
	}

	/* start playing once the pre-fill is in, not when the whole buffer is full */
	{
		snd_pcm_sw_params_t *sw = NULL;
		if (snd_pcm_sw_params_malloc(&sw) == 0) {
			if (snd_pcm_sw_params_current(pcm, sw) == 0 &&
				snd_pcm_sw_params_set_start_threshold(pcm, sw, std::max(period,buffer / 4)) == 0)
				snd_pcm_sw_params(pcm, sw);
			snd_pcm_sw_params_free(sw);
		}
	}

	fprintf(stderr,"Audio monitor: playing to '%s', %dHz, buffer %lu frames, period %lu frames\n",
		mon_device.c_str(),mon_rate,(unsigned long)buffer,(unsigned long)period);

	tmp_frames = period;
	tmp = new int16_t[tmp_frames * (size_t)mon_channels];

	/* the silence pre-fill goes in when audio is actually waiting (at start and after
	 * an underrun), otherwise playback would start and immediately run dry */
	need_prefill = true;

	while (!mon_quit) {
		size_t n = 0;

		/* take up to one period from the ring */
		pthread_mutex_lock(&mon_mutex);
		while (mon_ring_fill == 0 && !mon_quit) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME,&ts);
			ts.tv_nsec += 100000000L;
			if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
			pthread_cond_timedwait(&mon_cond,&mon_mutex,&ts);
		}
		if (!mon_quit) {
			size_t tail = (mon_ring_head + mon_ring_frames - mon_ring_fill) % mon_ring_frames;
			n = std::min(mon_ring_fill,tmp_frames);
			for (size_t i=0;i < n;i++) {
				memcpy(tmp + (i * (size_t)mon_channels),mon_ring + (tail * (size_t)mon_channels),sizeof(int16_t) * (size_t)mon_channels);
				if (++tail == mon_ring_frames) tail = 0;
			}
			mon_ring_fill -= n;
		}
		pthread_mutex_unlock(&mon_mutex);
		if (mon_quit || n == 0) continue;

		if (need_prefill) {
			monitor_prefill(pcm,buffer);
			need_prefill = false;
		}

		/* blocking write: this thread is paced by the sound card */
		size_t done = 0;
		while (done < n && !mon_quit) {
			snd_pcm_sframes_t r = snd_pcm_writei(pcm, tmp + (done * (size_t)mon_channels), n - done);
			if (r > 0) {
				done += (size_t)r;
			}
			else if (r == -EPIPE) {
				/* underrun: capture stalled or the card runs faster than the capture clock */
				mon_underruns++;
				update_now_time();
				fprintf(stderr,"Audio monitor: underrun at %.3f\n",monoNOW);
				snd_pcm_prepare(pcm);
				monitor_prefill(pcm,buffer);
			}
			else if (r < 0 && snd_pcm_recover(pcm, (int)r, 1) < 0) {
				fprintf(stderr,"Audio monitor: playback error %s, closing\n",snd_strerror((int)r));
				goto fail;
			}
		}
	}

	snd_pcm_drop(pcm);
	snd_pcm_close(pcm);
	delete[] tmp;
	mon_done = true;
	return NULL;
fail:
	if (pcm != NULL) snd_pcm_close(pcm);
	if (tmp != NULL) delete[] tmp;
	mon_failed = true;
	mon_done = true;
	return NULL;
}

static void monitor_stop() {
	if (!mon_running) return;

	pthread_mutex_lock(&mon_mutex);
	mon_quit = true;
	pthread_cond_signal(&mon_cond);
	pthread_mutex_unlock(&mon_mutex);
	pthread_join(mon_thread,NULL);
	mon_running = false;

	if (mon_failed)
		alsa_play_retry = monoNOW + 5.0;
	else
		fprintf(stderr,"Audio monitor closed (dropped %llu frames to limit latency, %lu underruns)\n",
			mon_dropped,mon_underruns);

	if (mon_ring != NULL) {
		delete[] mon_ring;
		mon_ring = NULL;
	}
	mon_ring_frames = mon_ring_fill = mon_ring_head = 0;
}

static void monitor_start() {
	if (mon_running) return;
	if (audio_rate <= 0 || audio_channels <= 0) return;
	if (monoNOW < alsa_play_retry) return;

	mon_rate = audio_rate;
	mon_channels = audio_channels;
	mon_device = audio_monitor_device;
	mon_ring_frames = (size_t)audio_rate / 10; /* 100 ms */
	mon_ring = new int16_t[mon_ring_frames * (size_t)audio_channels];
	mon_ring_head = mon_ring_fill = 0;
	mon_dropped = 0;
	mon_underruns = 0;
	mon_quit = false;
	mon_done = false;
	mon_failed = false;

	if (pthread_create(&mon_thread,NULL,monitor_thread_proc,NULL) != 0) {
		fprintf(stderr,"Audio monitor: cannot start thread\n");
		delete[] mon_ring;
		mon_ring = NULL;
		alsa_play_retry = monoNOW + 5.0;
		return;
	}
	mon_running = true;
}

/* bring the monitor in line with what the user wants (main thread) */
static void monitor_update_state() {
	if (mon_running && mon_done) /* the thread gave up (device error) */
		monitor_stop();

	if (audio_monitor && alsa_pcm != NULL)
		monitor_start();
	else
		monitor_stop();
}

/* main thread: queue captured audio for the monitor. Never blocks on the sound card. */
static void monitor_push(const int16_t *samples,size_t frames) {
	if (!mon_running || mon_done || frames == 0) return;

	pthread_mutex_lock(&mon_mutex);
	if (frames > mon_ring_frames) { /* only the newest fit */
		mon_dropped += frames - mon_ring_frames;
		samples += (frames - mon_ring_frames) * (size_t)mon_channels;
		frames = mon_ring_frames;
	}
	for (size_t i=0;i < frames;i++) {
		memcpy(mon_ring + (mon_ring_head * (size_t)mon_channels),samples + (i * (size_t)mon_channels),sizeof(int16_t) * (size_t)mon_channels);
		if (++mon_ring_head == mon_ring_frames) mon_ring_head = 0;
	}
	mon_ring_fill += frames;
	if (mon_ring_fill > mon_ring_frames) { /* the oldest were overwritten */
		mon_dropped += mon_ring_fill - mon_ring_frames;
		mon_ring_fill = mon_ring_frames;
	}
	pthread_cond_signal(&mon_cond);
	pthread_mutex_unlock(&mon_mutex);
}

/* ------------------------------------------------------------------------------
 * "capture-card" audio: find the ALSA card that belongs to the same hardware as the
 * V4L device we capture from.
 *
 * sysfs gives the hardware node of the video device (/sys/dev/char/M:m/device) and
 * of each sound card (/sys/class/sound/cardN/device). They belong together if they
 * are the same node (saa7134-alsa), interfaces of the same USB device (GV-USB2,
 * em28xx, cx231xx), or functions of the same PCI slot (cx88-alsa).
 * ------------------------------------------------------------------------------ */
static string sysfs_realpath(const string &path) {
	char tmp[PATH_MAX];
	if (realpath(path.c_str(),tmp) == NULL) return string();
	return string(tmp);
}

static string sysfs_first_line(const string &path) {
	char tmp[256];
	FILE *fp = fopen(path.c_str(),"r");
	if (fp == NULL) return string();
	if (fgets(tmp,sizeof(tmp),fp) == NULL) tmp[0] = 0;
	fclose(fp);
	char *e = tmp + strlen(tmp);
	while (e > tmp && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
	return string(tmp);
}

static string path_dir(const string &p) {
	size_t i = p.rfind('/');
	return (i == string::npos) ? string() : p.substr(0,i);
}

static string path_base(const string &p) {
	size_t i = p.rfind('/');
	return (i == string::npos) ? p : p.substr(i+1);
}

/* "0000:03:00.1" -> true, slot "0000:03:00" */
static bool pci_function_name(const string &b,string &slot) {
	unsigned int dom,bus,dev,fn;
	char tail;
	if (sscanf(b.c_str(),"%x:%x:%x.%x%c",&dom,&bus,&dev,&fn,&tail) != 4) return false;
	slot = b.substr(0,b.rfind('.'));
	return true;
}

/* "1-1:1.0" (bus-port[.port...]:config.interface) */
static bool usb_interface_name(const string &b) {
	unsigned int cfg,ifn;
	size_t c = b.find(':');
	if (c == string::npos || c == 0 || b.find('-') == string::npos || b.find('-') > c) return false;
	return sscanf(b.c_str() + c + 1,"%u.%u",&cfg,&ifn) == 2;
}

/* returns an ALSA device name such as "plughw:CARD=GVUSB2,DEV=0", or "" if none */
static string alsa_device_for_v4l(const char *devname) {
	struct stat st;
	char tmp[256];

	if (devname == NULL || stat(devname,&st) != 0 || !S_ISCHR(st.st_mode)) return string();

	snprintf(tmp,sizeof(tmp),"/sys/dev/char/%u:%u/device",(unsigned int)major(st.st_rdev),(unsigned int)minor(st.st_rdev));
	const string vdev = sysfs_realpath(tmp);
	if (vdev.empty()) return string();

	const string vbase = path_base(vdev),vdir = path_dir(vdev);
	string vslot;
	const bool v_pci = pci_function_name(vbase,vslot);
	const bool v_usb = usb_interface_name(vbase);

	int best_card = -1,best_score = 0;
	string best_id;

	DIR *d = opendir("/sys/class/sound");
	if (d == NULL) return string();
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		int card;
		char tail;
		if (sscanf(de->d_name,"card%d%c",&card,&tail) != 1) continue;

		const string cpath = string("/sys/class/sound/") + de->d_name;
		const string cdev = sysfs_realpath(cpath + "/device");
		if (cdev.empty()) continue;

		const string cbase = path_base(cdev),cdir = path_dir(cdev);
		string cslot;
		int score = 0;

		if (cdev == vdev)
			score = 3;	/* same device node */
		else if (v_usb && usb_interface_name(cbase) && cdir == vdir)
			score = 2;	/* another interface of the same USB device */
		else if (v_pci && pci_function_name(cbase,cslot) && cslot == vslot && cdir == vdir)
			score = 2;	/* another function of the same PCI slot */

		if (score > best_score) {
			best_score = score;
			best_card = card;
			best_id = sysfs_first_line(cpath + "/id");
		}
	}
	closedir(d);

	if (best_card < 0) return string();

	/* the card's first capture PCM device */
	int best_pcm = -1;
	if ((d = opendir("/sys/class/sound")) != NULL) {
		while ((de = readdir(d)) != NULL) {
			int c,dev;
			char dir,tail;
			if (sscanf(de->d_name,"pcmC%dD%d%c%c",&c,&dev,&dir,&tail) != 3) continue;
			if (c != best_card || dir != 'c') continue;
			if (best_pcm < 0 || dev < best_pcm) best_pcm = dev;
		}
		closedir(d);
	}
	if (best_pcm < 0) {
		fprintf(stderr,"Audio: card %d (%s) belongs to %s but has no capture device\n",best_card,best_id.c_str(),devname);
		return string();
	}

	/* plughw: converts rate/format/channels if the card cannot do 48kHz S16 stereo itself */
	if (!best_id.empty())
		snprintf(tmp,sizeof(tmp),"plughw:CARD=%s,DEV=%d",best_id.c_str(),best_pcm);
	else
		snprintf(tmp,sizeof(tmp),"plughw:%d,%d",best_card,best_pcm);

	return string(tmp);
}

void socket_MSG(const char *msg);

/* "OK" (reply to record-on) or "AVI-RESTART" (new file after a format change) */
static void announce_avi(const char *head) {
	char msg[4096];

	snprintf(msg,sizeof(msg),
		"%s\n"
		"File:%s\n"
		"VidCodec:MPEG4\n"
		"VidSize:%ux%u\n"
		"VidRate:%u/%u\n"
		"StartTime:%.3f\n",
		head,
		avi_file.c_str()/*FILE*/,
		v4l_width,v4l_height/*VidSize*/,
		v4l_framerate_n,v4l_framerate_d/*VidRate*/,
		avi_file_start_time/*StartTime*/);
	socket_MSG(msg);
}

void close_v4l() {
	int i,x;

	close_avi_file();
	monitor_stop();

	if (alsa_hw_params != NULL) {
		snd_pcm_hw_params_free(alsa_hw_params);
		alsa_hw_params = NULL;
	}
	if (alsa_pcm != NULL) {
		snd_pcm_close(alsa_pcm);
		alsa_pcm = NULL;
	}

	if (v4l_fd >= 0) {
		x = 1;
		if (ioctl(v4l_fd,VIDIOC_STREAMOFF,&x)) {
			fprintf(stderr,"Unable to stream off, %s\n",strerror(errno));
		}

		for (i=0;i < v4l_buffers;i++) {
			if (v4l_ptr[i] != NULL) {
				munmap(v4l_ptr[i],v4l_framesize);
				v4l_ptr[i] = NULL;
			}
			memset(&v4l_buf[i],0,sizeof(v4l_buf[i]));
		}
	}

	if (vbi_buffer != NULL) {
		delete[] vbi_buffer;
		vbi_buffer = NULL;
	}

	if (vbi_fd >= 0) close(vbi_fd);
	vbi_fd = -1;

	if (v4l_fd >= 0) close(v4l_fd);
	v4l_fd = -1;
}

void apply_v4l_controls(void) {
	struct v4l2_queryctrl qc;
	struct v4l2_control cc;

	if (v4l_fd < 0) return;

	memset(&qc,0,sizeof(qc));
	memset(&cc,0,sizeof(cc));
	cc.id = qc.id = V4L2_CID_BRIGHTNESS;
	if (ioctl(v4l_fd,VIDIOC_QUERYCTRL, &qc) >= 0 && !(qc.flags & V4L2_CTRL_FLAG_DISABLED)) {
		if (v4l_brightness != 0 && qc.minimum < qc.maximum) {
			const signed long range = (qc.maximum - qc.minimum);
			cc.value = qc.default_value + ((range * (signed long)v4l_brightness) / 20000l);
			if (cc.value < qc.minimum) cc.value = qc.minimum;
			if (cc.value > qc.maximum) cc.value = qc.maximum;
		}
		else {
			cc.value = qc.default_value;
		}

		bool ok = false;
		if (ioctl(v4l_fd,VIDIOC_S_CTRL,&cc) >= 0) ok = true;

		fprintf(stderr,"V4L brightness set: min=%d/val=%d/max=%d/def=%d ok=%u\n",
			qc.minimum,cc.value,qc.maximum,qc.default_value,ok);
	}

	memset(&qc,0,sizeof(qc));
	memset(&cc,0,sizeof(cc));
	cc.id = qc.id = V4L2_CID_CONTRAST;
	if (ioctl(v4l_fd,VIDIOC_QUERYCTRL, &qc) >= 0 && !(qc.flags & V4L2_CTRL_FLAG_DISABLED)) {
		if (v4l_contrast != 0 && qc.minimum < qc.maximum) {
			const signed long range = (qc.maximum - qc.minimum);
			cc.value = qc.default_value + ((range * (signed long)v4l_contrast) / 20000l);
			if (cc.value < qc.minimum) cc.value = qc.minimum;
			if (cc.value > qc.maximum) cc.value = qc.maximum;
		}
		else {
			cc.value = qc.default_value;
		}

		bool ok = false;
		if (ioctl(v4l_fd,VIDIOC_S_CTRL,&cc) >= 0) ok = true;

		fprintf(stderr,"V4L contrast set: min=%d/val=%d/max=%d/def=%d ok=%u\n",
			qc.minimum,cc.value,qc.maximum,qc.default_value,ok);
	}
}

int open_v4l() {
	struct v4l2_requestbuffers reqb;
	int x;

	v4l_basetime = -1;
	if (v4l_devname[0] != 0 && (v4l_fd = open(v4l_devname,O_RDWR/*Sensorray drivers require this---WHY?!?*/)) >= 0) {
		struct v4l2_cropcap cropcap;
		struct v4l2_fmtdesc ft;
		char tmp[6];
		int i;

		v4l_bufptr = 0;
		fprintf(stderr,"Opened %s\n",v4l_devname);

		if (ioctl(v4l_fd,VIDIOC_QUERYCAP,&v4l_caps) != 0) {
			fprintf(stderr,"Failed to query caps, %s\n",strerror(errno));
			goto fail;
		}
		fprintf(stderr,"-----CAPS-----\n");
		fprintf(stderr,"Driver: %s\n",v4l_caps.driver);
		fprintf(stderr,"Card:   %s\n",v4l_caps.card);
		fprintf(stderr,"Businfo:%s\n",v4l_caps.bus_info);
		fprintf(stderr,"Ver:    %X\n",v4l_caps.version);
		fprintf(stderr,"Caps:   %X\n",v4l_caps.capabilities);
		if (!(v4l_caps.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
			fprintf(stderr,"ERROR: Not a video capture device\n");
			goto fail;
		}
		else if (!(v4l_caps.capabilities & V4L2_CAP_STREAMING)) {
			fprintf(stderr,"ERROR: Does not support streaming capture\n");
			goto fail;
		}

		if (!input_device.empty()) {
			struct v4l2_input v4l2_input;
			unsigned int index;
			int set_index = -1;

			for (index=0;index < 10000;index++) {
				memset(&v4l2_input,0,sizeof(v4l2_input));
				v4l2_input.index = index;
				if (ioctl(v4l_fd,VIDIOC_ENUMINPUT,&v4l2_input) == 0) {
					fprintf(stderr," [input %u] '%s'\n",index,(const char*)v4l2_input.name);

					if (set_index < 0 && (const char*)v4l2_input.name == input_device)
						set_index = index;
				}
			}

			if (set_index >= 0) {
				unsigned int x = (unsigned int)set_index;

				fprintf(stderr,"Input '%s' is index %u\n",input_device.c_str(),set_index);

				if (ioctl(v4l_fd,VIDIOC_S_INPUT,&x))
					fprintf(stderr,"Failed to set input '%s', %s\n",input_device.c_str(),strerror(errno));
			}
			else {
				fprintf(stderr,"Input '%s' not found\n",input_device.c_str());
			}
		}

		if (!input_standard.empty()) {
			struct v4l2_standard v4l2_std;
			unsigned int index;
			v4l2_std_id std_x;
			bool found=false;

			for (index=0;index < 10000;index++) {
				memset(&v4l2_std,0,sizeof(v4l2_std));
				v4l2_std.index = index;
				if (ioctl(v4l_fd,VIDIOC_ENUMSTD,&v4l2_std) == 0) {
					fprintf(stderr," [input %u] '%s'\n",index,(const char*)v4l2_std.name);

					if (!found && (const char*)v4l2_std.name == input_standard) {
						std_x = v4l2_std.id;
						found = true;
					}
				}
			}

			if (found) {
				fprintf(stderr,"Standard '%s' found\n",input_standard.c_str());

				if (ioctl(v4l_fd,VIDIOC_S_STD,&std_x))
					fprintf(stderr,"Failed to set standard '%s', %s\n",input_standard.c_str(),strerror(errno));
			}
			else {
				fprintf(stderr,"Standard '%s' not found\n",input_standard.c_str());
			}
		}

		/* saa7134 hack:
		 *
		 * the driver resets the crop rectangle when it re-applies the TV standard.
		 * That is understandable, however it does this also when changing the input,
		 * and it does it on opening the vbi or video device.
		 *
		 * The problem is that, if we open and set up the video device, THEN set up
		 * the vbi capture, the VBI capture will trigger the TV standard setup which
		 * will then reset our crop rectangle and then it's almost as if we never
		 * changing it from the default.
		 *
		 * So, if we see the saa7134 driver here, we open the VBI device NOW to avoid
		 * that problem. */
		/* NTS: Set the video standard FIRST (above) then open VBI.
		 *      Video standard affects what scanlines and how many are captured */
		if (v4l_open_vbi_first > 0)
			capture_vbi_open();
		else if (v4l_open_vbi_first < 0) {
			/* auto setting */
			if (!strcmp((char*)v4l_caps.driver,"saa7134")) {
				fprintf(stderr,"saa7134 driver detected, opening VBI device NOW to avoid crop rectangle bugs\n");
				capture_vbi_open();
			}
		}

		memset(&cropcap,0,sizeof(cropcap));
		cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (ioctl(v4l_fd,VIDIOC_CROPCAP,&cropcap) >= 0) {
			fprintf(stderr,"-------CROP CAP-------\n");
			fprintf(stderr,"Bounds:  pos=(%ld,%ld) size=(%ld,%ld)\n",
				(long)cropcap.bounds.left,
				(long)cropcap.bounds.top,
				(long)cropcap.bounds.width,
				(long)cropcap.bounds.height);
			fprintf(stderr,"Defrect: pos=(%ld,%ld) size=(%ld,%ld)\n",
				(long)cropcap.defrect.left,
				(long)cropcap.defrect.top,
				(long)cropcap.defrect.width,
				(long)cropcap.defrect.height);
			fprintf(stderr,"Aspect:  %lu/%lu\n",
				(unsigned long)cropcap.pixelaspect.numerator,
				(unsigned long)cropcap.pixelaspect.denominator);
		}
		else {
			fprintf(stderr,"Unable to ask for crop capabilities\n");
		}

		int final_width = want_width;
		int final_height = want_height;

		/* try to change rect */
		{
			struct v4l2_crop crop;

			/* if we see that the capture card defines a default crop rectangle that
			 * doesn't fit it's own bounds crop rectangle, then say so, because that's
			 * a stupid mistake. Isn't that right, saa7134? */
			if (	cropcap.defrect.left < cropcap.bounds.left ||
				cropcap.defrect.top  < cropcap.bounds.top ||
				(cropcap.defrect.left+cropcap.defrect.width) > (cropcap.bounds.left+cropcap.bounds.width) ||
				(cropcap.defrect.top+cropcap.defrect.height) > (cropcap.bounds.top+cropcap.bounds.height)) {
				fprintf(stderr,"WARNING: Default crop rectangle is out of bounds, according to bounds crop rectangle.\n");
				fprintf(stderr,"         To capture properly, please consider using -cropbounds or setting a crop rectangle.\n");
			}

			crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			if (ioctl(v4l_fd,VIDIOC_G_CROP,&crop) == 0) {
				fprintf(stderr,"Crop is currently: pos=(%ld,%ld) size=(%ld,%ld)\n",
					(long)crop.c.left,
					(long)crop.c.top,
					(long)crop.c.width,
					(long)crop.c.height);

				/* check for saa7134 stupidity where the crop rectangle it
				 * initialized when we opened the device doesn't fit it's own bounds crop rectangle */
				if (	crop.c.left < cropcap.bounds.left ||
					crop.c.top <  cropcap.bounds.top ||
					(crop.c.left+crop.c.width) > (cropcap.bounds.left+cropcap.bounds.width) ||
					(crop.c.top+crop.c.height) > (cropcap.bounds.top+cropcap.bounds.height)) {
					fprintf(stderr,"WARNING: Initial crop rectangle setup by driver (upon opening the device) is out of bounds, according to bounds crop rectangle.\n");
					fprintf(stderr,"         To capture properly, please consider using -cropbounds/-cropdef or setting a crop rectangle.\n");
				}
			}
			else {
				fprintf(stderr,"Cannot query crop rect\n");
				memset(&crop,0,sizeof(crop));
			}

			{
				if (v4l_crop_bounds) {
					crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
					crop.c = cropcap.bounds;

					if (v4l_crop_vcr_hack > 0) {
						if (crop.c.height > v4l_crop_vcr_hack)
							crop.c.height -= v4l_crop_vcr_hack;
					}

					if (ioctl(v4l_fd,VIDIOC_S_CROP,&crop) == 0)
						fprintf(stderr,"Set bound rect... OK\n");
					else
						fprintf(stderr,"Failed to set bound rect\n");
				}
				else if (v4l_crop_defrect) {
					crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
					crop.c = cropcap.defrect;

					if (v4l_crop_vcr_hack > 0) {
						if (crop.c.height > v4l_crop_vcr_hack)
							crop.c.height -= v4l_crop_vcr_hack;
					}

					if (ioctl(v4l_fd,VIDIOC_S_CROP,&crop) == 0)
						fprintf(stderr,"Set def rect... OK\n");
					else
						fprintf(stderr,"Failed to set def rect\n");

					/* look for saa7134 stupidity.
					 * it will specify a default rect, then when we apply it,
					 * it will crop to the bounds rect and we'll unexpectedly
					 * get back a different rect 2 scanlines shorter. */
					crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
					if (ioctl(v4l_fd,VIDIOC_G_CROP,&crop) == 0) {
						if (	crop.c.left != cropcap.defrect.left ||
							crop.c.top != cropcap.defrect.top ||
							crop.c.width != cropcap.defrect.width ||
							crop.c.height != cropcap.defrect.height) {
							fprintf(stderr,"Um... capture card driver rejects it's own default crop rectangle. Okay then...\n");
						}
					}
				}
				else if (v4l_crop_x != CROP_DEFAULT || v4l_crop_y != CROP_DEFAULT ||
					v4l_crop_w != CROP_DEFAULT || v4l_crop_h != CROP_DEFAULT) {
					crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

					if (v4l_crop_x != CROP_DEFAULT)
						crop.c.left = v4l_crop_x;
					if (v4l_crop_y != CROP_DEFAULT)
						crop.c.top = v4l_crop_y;
					if (v4l_crop_w != CROP_DEFAULT)
						crop.c.width = v4l_crop_w;
					if (v4l_crop_h != CROP_DEFAULT)
						crop.c.height = v4l_crop_h;

					if (ioctl(v4l_fd,VIDIOC_S_CROP,&crop) == 0)
						fprintf(stderr,"Set user rect... OK\n");
					else
						fprintf(stderr,"Failed to set user rect\n");
				}

				crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
				if (ioctl(v4l_fd,VIDIOC_G_CROP,&crop) == 0) {
					fprintf(stderr,"Crop is now: pos=(%ld,%ld) size=(%ld,%ld)\n",
						(long)crop.c.left,
						(long)crop.c.top,
						(long)crop.c.width,
						(long)crop.c.height);

					/* match the width/height to avoid hardware scaling */
					if (final_width <= 0) final_width = crop.c.width;
					if (final_height <= 0) final_height = crop.c.height;
				}
			}
		}

		fprintf(stderr,"------FMTS-------\n");
		i=0;
		do {
			ft.index = i;
			ft.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			if (ioctl(v4l_fd,VIDIOC_ENUM_FMT,&ft) != 0) {
				if (errno != EINVAL)
					fprintf(stderr,"----DONE %s\n",strerror(errno));

				break;
			}

			fprintf(stderr,"-----[%u]-----\n",i);
			fprintf(stderr,"Flags:   %X\n",ft.flags);
			fprintf(stderr,"Desc:    %s\n",ft.description);
			*((uint32_t*)tmp) = ft.pixelformat; tmp[4] = 0;
			fprintf(stderr,"Pixelfmt:%X '%s'\n",ft.pixelformat,tmp);

			{
				struct v4l2_frmsizeenum fse;
				unsigned int fi = 0;

				fprintf(stderr,"Frame sizes for pixel formats:\n");
				do {
					fse.index = fi;
					fse.pixel_format = ft.pixelformat;

					unsigned int query_width = 640,query_height = 480;

					if (ioctl(v4l_fd,VIDIOC_ENUM_FRAMESIZES,&fse) != 0) {
						if (errno != EINVAL)
							fprintf(stderr,"Frame size DONE %s\n",strerror(errno));

						break;
					}

					if (fse.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
						const struct v4l2_frmsize_discrete &d = fse.discrete;
						fprintf(stderr,"  Discrete %u: %ux%u\n",fi,(unsigned int)d.width,(unsigned int)d.height);
						query_width = d.width;
						query_height = d.height;
					}
					else if (fse.type == V4L2_FRMSIZE_TYPE_CONTINUOUS || fse.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
						const struct v4l2_frmsize_stepwise &s = fse.stepwise;

						if (fse.type == V4L2_FRMSIZE_TYPE_CONTINUOUS)
							fprintf(stderr,"  Continuous %u: ",fi);
						else
							fprintf(stderr,"  Stepwise %u: ",fi);

						fprintf(stderr,"width(min/max/step)=%u/%u/%u height(min/max/step)=%u/%u/%u",
							(unsigned int)s.min_width,
							(unsigned int)s.max_width,
							(unsigned int)s.step_width,
							(unsigned int)s.min_height,
							(unsigned int)s.max_height,
							(unsigned int)s.step_height);

						query_width = s.max_width;
						query_height = s.max_height;
						fprintf(stderr,"\n");
					}

					{
						struct v4l2_frmivalenum fri;
						unsigned int ri = 0;

						do {
							fri.index = ri;
							fri.pixel_format = ft.pixelformat;
							fri.height = query_height;
							fri.width = query_width;

							if (ioctl(v4l_fd,VIDIOC_ENUM_FRAMEINTERVALS,&fri) != 0) {
								if (errno != EINVAL)
									fprintf(stderr,"Frame interval DONE %s\n",strerror(errno));

								break;
							}

							if (fri.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
								const struct v4l2_fract &d = fri.discrete;

								fprintf(stderr,"     Interval %u/%u (%.3f)\n",
									(unsigned int)d.numerator,
									(unsigned int)d.denominator,
									(double)d.numerator / (double)d.denominator);
							}
							else if (fri.type == V4L2_FRMIVAL_TYPE_CONTINUOUS || fri.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
								const struct v4l2_frmival_stepwise &d = fri.stepwise;

								if (fse.type == V4L2_FRMIVAL_TYPE_CONTINUOUS)
									fprintf(stderr,"  Continuous %u: ",fi);
								else
									fprintf(stderr,"  Stepwise %u: ",fi);

								fprintf(stderr,"     Interval min=%u/%u (%.3f) max=%u/%u (%.3f) step=%u/%u (%.3f)\n",
									(unsigned int)d.min.numerator,
									(unsigned int)d.min.denominator,
									(double)d.min.numerator / (double)d.min.denominator,

									(unsigned int)d.max.numerator,
									(unsigned int)d.max.denominator,
									(double)d.max.numerator / (double)d.max.denominator,

									(unsigned int)d.step.numerator,
									(unsigned int)d.step.denominator,
									(double)d.step.numerator / (double)d.step.denominator);
							}

							ri++;
						} while (1);
					}

					fi++;
				} while (1);
			}

			i++;
		} while (1);

		v4l_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (ioctl(v4l_fd,VIDIOC_G_FMT,&v4l_fmt)) {
			fprintf(stderr,"Cannot get format, %s\n",strerror(errno));
			goto fail;
		}
		fprintf(stderr,"%u x %u (stride=%u size=%u) colorspc=%u int=%d\n",
				v4l_fmt.fmt.pix.width,v4l_fmt.fmt.pix.height,
				v4l_fmt.fmt.pix.bytesperline,v4l_fmt.fmt.pix.sizeimage,
				v4l_fmt.fmt.pix.colorspace,
				v4l_fmt.fmt.pix.field);

		if (final_width <= 0) final_width = v4l_fmt.fmt.pix.width;
		if (final_height <= 0) final_height = v4l_fmt.fmt.pix.height;

		*((uint32_t*)tmp) = v4l_fmt.fmt.pix.pixelformat;
		tmp[4] = 0;
		fprintf(stderr,"  format='%s'\n",tmp);

		{
			uint32_t tf[] = {V4L2_PIX_FMT_YUYV,V4L2_PIX_FMT_UYVY};
			int i=0;

			do {
				v4l_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
				v4l_fmt.fmt.pix.width = final_width;
				v4l_fmt.fmt.pix.height = final_height;
				v4l_fmt.fmt.pix.pixelformat = tf[i];
#if 1
				if (effective_v4l_interlaced() >= 0)
					v4l_fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;
				else
					v4l_fmt.fmt.pix.field = V4L2_FIELD_NONE;
#else
				if (v4l_interlaced == 1)
					v4l_fmt.fmt.pix.field = V4L2_FIELD_INTERLACED_BT;
				else if (v4l_interlaced == 0)
					v4l_fmt.fmt.pix.field = V4L2_FIELD_INTERLACED_TB;
				else
					v4l_fmt.fmt.pix.field = V4L2_FIELD_NONE;
#endif
				v4l_fmt.fmt.pix.bytesperline = final_width * 2;
				v4l_fmt.fmt.pix.sizeimage = v4l_fmt.fmt.pix.bytesperline * v4l_height;
				v4l_fmt.fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
				*((uint32_t*)tmp) = tf[i]; tmp[4] = 0;
				if (ioctl(v4l_fd,VIDIOC_S_FMT,&v4l_fmt) < 0) {
					fprintf(stderr,"Unable to set format %s, %s\n",tmp,strerror(errno));
					/* try another */
					if (++i >= (sizeof(tf)/sizeof(tf[0]))) goto fail;
				}
				else {
					fprintf(stderr,"Set format to '%s'\n",tmp);
					break;
				}
			} while (1);
		}

		v4l_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (ioctl(v4l_fd,VIDIOC_G_FMT,&v4l_fmt)) {
			fprintf(stderr,"Cannot get format, %s\n",strerror(errno));
			goto fail;
		}
		fprintf(stderr,"%u x %u (stride=%u size=%u) colorspc=%u int=%d\n",
				v4l_fmt.fmt.pix.width,v4l_fmt.fmt.pix.height,
				v4l_fmt.fmt.pix.bytesperline,v4l_fmt.fmt.pix.sizeimage,
				v4l_fmt.fmt.pix.colorspace,
				v4l_fmt.fmt.pix.field);

		*((uint32_t*)tmp) = v4l_fmt.fmt.pix.pixelformat;
		tmp[4] = 0;
		fprintf(stderr,"  format='%s'\n",tmp);

		/* V4L2_FIELD_INTERLACED means "temporal order depends on the standard":
		 * bottom field first for 525/60 (M/NTSC), top field first for 625/50. */
		{
			v4l2_std_id cur_std = 0;

			v4l_std_625 = false;
			v4l_std_known = false;
			if (ioctl(v4l_fd,VIDIOC_G_STD,&cur_std) == 0 && cur_std != 0) {
				v4l_std_625 = (cur_std & V4L2_STD_625_50) != 0 && (cur_std & V4L2_STD_525_60) == 0;
				v4l_std_known = true;
			}
			fprintf(stderr,"  standard: %s\n",v4l_std_625 ? "625/50" : "525/60 (or unknown)");
		}

		/* Field order until the first buffer is dequeued (the AVI header and the
		 * encoder may be set up before that) */
		v4l_set_field_order(v4l_fmt.fmt.pix.field);

        /* FIXME: FFMPEG doesn't encode mpeg4 4:2:2? */
		if ((want_codec_sel == "mpeg2-422" || want_codec_sel == "h264-422" || want_codec_sel == "h265-422" || want_codec_sel == "huffyuv-422" || want_codec_sel == "utvideo-422") && (v4l_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV || v4l_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_UYVY)) {
			v4l_codec_yshr = 0;
		}
		else {
			v4l_codec_yshr = 1;
		}

		fprintf(stderr,"new size=%u bpline=%u recording-as=%s wantcodec=%s\n",
				v4l_fmt.fmt.pix.sizeimage,
				v4l_fmt.fmt.pix.bytesperline,
                v4l_codec_yshr==1?"4:2:0":"4:2:2",
                want_codec_sel.c_str());

		v4l_width = v4l_fmt.fmt.pix.width;
		v4l_height = v4l_fmt.fmt.pix.height;
		v4l_stride = v4l_fmt.fmt.pix.bytesperline;
		v4l_width_stride = (v4l_width + 15) & (~15);
		v4l_framesize = v4l_fmt.fmt.pix.sizeimage;

		if (v4l_buffers == 0)
			v4l_buffers = 30;

		{
			struct v4l2_crop crop;

			crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			if (ioctl(v4l_fd,VIDIOC_G_CROP,&crop) == 0) {
				fprintf(stderr,"Crop is now: pos=(%ld,%ld) size=(%ld,%ld)\n",
						(long)crop.c.left,
						(long)crop.c.top,
						(long)crop.c.width,
						(long)crop.c.height);
			}
		}

		memset(reqb.reserved,0,sizeof(reqb.reserved));
		reqb.count = v4l_buffers;
		reqb.memory = V4L2_MEMORY_MMAP;
		reqb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (ioctl(v4l_fd,VIDIOC_REQBUFS,&reqb)) {
			fprintf(stderr,"Unable to request buffers, %s\n",strerror(errno));
			goto fail;
		}
		fprintf(stderr,"buffers=%u\n",reqb.count);
		v4l_buffers = reqb.count;

		/* assume for now an optimisticly high frame rate.
		 * we'll let V4L correct us later */
		v4l_framerate_n = 300000; /* Linux kernel trick: 300Hz common multiple of NTSC and PAL */
		v4l_framerate_d = 1001;

		/* if the user wants anything else, then apply it now */
		if (want_fps > 0) {
			if (want_fps >= 29.9 && want_fps < 29.99) {
				v4l_framerate_n = 30000;
				v4l_framerate_d = 1001;
			}
			else if (want_fps >= 59.9 && want_fps < 59.98) {
				v4l_framerate_n = 60000;
				v4l_framerate_d = 1001;
			}
			else if (want_fps >= 119.8 && want_fps < 119.95) {
				v4l_framerate_n = 120000;
				v4l_framerate_d = 1001;
			}
			else {
				const double fudge = 0.005;
				unsigned int w = (unsigned int)floor(want_fps+fudge);
				double f = want_fps - w;

				assert(f >= 0 && f < 1.0);

				unsigned int wf = (unsigned int)(f * 100);

				if (wf == 0) {
					v4l_framerate_n = w;
					v4l_framerate_d = 1;
				}
				else {
					v4l_framerate_n = (w * 100U) + wf;
					v4l_framerate_d = 100;
				}
			}
		}

		fprintf(stderr,"Initial fps: %u/%u\n",v4l_framerate_n,v4l_framerate_d);

		{
			struct v4l2_streamparm prm;

			memset(&prm,0,sizeof(prm));
			prm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			if (ioctl(v4l_fd,VIDIOC_G_PARM,&prm)) {
				/* not every driver has it (pexhd before 2026-11-27): keep the rate guessed above */
				fprintf(stderr,"Unable to get parameters, %s: keeping %u/%u fps\n",strerror(errno),
					v4l_framerate_n,v4l_framerate_d);
				goto parm_done;
			}

			fprintf(stderr,"----current params----\n");
			fprintf(stderr,"Capable:  0x%08lX\n",(unsigned long)prm.parm.capture.capability);
			fprintf(stderr,"Capmode:  0x%08lX\n",(unsigned long)prm.parm.capture.capturemode);
			fprintf(stderr,"T/p/frame:%u/%u\n",(unsigned)prm.parm.capture.timeperframe.numerator,(unsigned)prm.parm.capture.timeperframe.denominator);
			fprintf(stderr,"Extmode:  0x%08lX\n",(unsigned long)prm.parm.capture.extendedmode);
			fprintf(stderr,"Readbufs: %u\n",(unsigned)prm.parm.capture.readbuffers);

			prm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			prm.parm.capture.capability = 0;
			prm.parm.capture.capturemode = V4L2_CAP_TIMEPERFRAME;
			prm.parm.capture.timeperframe.numerator = v4l_framerate_d;
			prm.parm.capture.timeperframe.denominator = v4l_framerate_n;
			prm.parm.capture.extendedmode = 0;
			prm.parm.capture.readbuffers = v4l_buffers;
			if (ioctl(v4l_fd,VIDIOC_S_PARM,&prm)) {
				/* okay fine try without time/frame */
				prm.parm.capture.capturemode = 0;
				fprintf(stderr," * Not using time/frame\n");
				if (ioctl(v4l_fd,VIDIOC_S_PARM,&prm)) {
					fprintf(stderr,"Unable to set parameters, %s\n",strerror(errno));
					fprintf(stderr," * Oh well\n");
				}
			}

			memset(&prm,0,sizeof(prm));
			prm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			if (ioctl(v4l_fd,VIDIOC_G_PARM,&prm)) {
				/* not every driver has it (pexhd before 2026-11-27): keep the rate guessed above */
				fprintf(stderr,"Unable to get parameters, %s: keeping %u/%u fps\n",strerror(errno),
					v4l_framerate_n,v4l_framerate_d);
				goto parm_done;
			}

			/* if we can, read back what the capture card says is the frame rate */
			if (prm.parm.capture.timeperframe.numerator != 0 && prm.parm.capture.timeperframe.denominator != 0) {
				v4l_framerate_d = prm.parm.capture.timeperframe.numerator;
				v4l_framerate_n = prm.parm.capture.timeperframe.denominator;
			}
			else {
				/* TODO: We need to guess from the TV standard */
			}

			fprintf(stderr,"----new params----\n");
			fprintf(stderr,"Capable:  0x%08lX\n",(unsigned long)prm.parm.capture.capability);
			fprintf(stderr,"Capmode:  0x%08lX\n",(unsigned long)prm.parm.capture.capturemode);
			fprintf(stderr,"T/p/frame:%u/%u\n",(unsigned)prm.parm.capture.timeperframe.numerator,(unsigned)prm.parm.capture.timeperframe.denominator);
			fprintf(stderr,"Extmode:  0x%08lX\n",(unsigned long)prm.parm.capture.extendedmode);
			fprintf(stderr,"Readbufs: %u\n",(unsigned)prm.parm.capture.readbuffers);

			fprintf(stderr,"Final video timeline is %lu/%lu (%.6f)\n",
				(unsigned long)v4l_framerate_n,
				(unsigned long)v4l_framerate_d,
				(double)v4l_framerate_n / v4l_framerate_d);
		}
parm_done:

		/* set up mapping */
		for (i=0;i < v4l_buffers;i++) {
			memset(&v4l_buf[i],0,sizeof(v4l_buf[i]));
			v4l_buf[i].index = i;
			v4l_buf[i].type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			v4l_buf[i].memory = V4L2_MEMORY_MMAP;
			if (ioctl(v4l_fd,VIDIOC_QUERYBUF,&v4l_buf[i])) {
				fprintf(stderr,"Unable to query buffer %u\n",i);
				goto fail;
			}

			fprintf(stderr,"buf[%u] @ %u (+%u) (sz=%u)\n",
				(unsigned int)i,
				(unsigned int)v4l_buf[i].m.offset,
				(unsigned int)v4l_framesize,
				(unsigned int)v4l_buf[i].length);

			v4l_ptr[i] = (unsigned char*)mmap(NULL,v4l_buf[i].length,PROT_READ | PROT_WRITE/*Sensorray: THIS IS REQUIRED?!?!?*/,
				MAP_SHARED,v4l_fd,v4l_buf[i].m.offset);
			if (v4l_ptr[i] == MAP_FAILED) {
				v4l_ptr[i] = NULL;
				fprintf(stderr,"Unable to map buffer %u, %s\n",i,strerror(errno));
				goto fail;
			}
		}

		/* queue up the buffers */
		for (i=0;i < v4l_buffers;i++) {
			v4l_buf[i].index = i;
			v4l_buf[i].type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			v4l_buf[i].memory = V4L2_MEMORY_MMAP;
			if (ioctl(v4l_fd,VIDIOC_QBUF,&v4l_buf[i])) {
				fprintf(stderr,"Unable to q buffer %u\n",i);
				goto fail;
			}
		}

		/* non-block */
		fcntl(v4l_fd,F_SETFL,
			fcntl(v4l_fd,F_GETFL) | O_NONBLOCK);
	}
	else {
		if (v4l_devname[0] != 0 && !v4l_open_shut_up) {
			v4l_open_shut_up = 1;
			fprintf(stderr,"Cannot open V4L device '%s', reason: %s\n",
				v4l_devname,strerror(errno));
		}
	}

	if (v4l_fd >= 0) {
		/* start stream */
		x = 1;
		if (ioctl(v4l_fd,VIDIOC_STREAMON,&x)) {
			fprintf(stderr,"Unable to stream on, %s\n",strerror(errno));
			goto fail;
		}
	}

	if (capture_vbi && vbi_fd < 0)
		capture_vbi_open();

	if (!audio_device.empty()) {
		const char *name;
		int err;

		string card_dev;

		if (audio_device == "auto")
			name = "default";
		else if (audio_device == "capture-card") {
			/* the capture card's own ALSA device, found through sysfs */
			card_dev = alsa_device_for_v4l(v4l_devname);
			if (!card_dev.empty()) {
				fprintf(stderr,"Audio: using the capture card's ALSA device '%s' for %s\n",card_dev.c_str(),v4l_devname);
				name = card_dev.c_str();
			}
			else {
				fprintf(stderr,"Audio: no ALSA device found for %s, using 'default'\n",v4l_devname);
				name = "default";
			}
		}
		else
			name = audio_device.c_str();

		assert(alsa_hw_params == NULL);
		assert(alsa_pcm == NULL);

		if ((err = snd_pcm_open(&alsa_pcm, name, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK)) < 0) {
			fprintf(stderr,"Failed to open ALSA device '%s', %s\n",name,snd_strerror(err));
			goto fail;
		}
		if ((err = snd_pcm_hw_params_malloc(&alsa_hw_params)) < 0) {
			fprintf(stderr,"Failed to alloc ALSA params, %s\n",snd_strerror(err));
			goto fail;
		}
		if ((err = snd_pcm_hw_params_any(alsa_pcm, alsa_hw_params)) < 0) {
			fprintf(stderr,"Failed to init ALSA params, %s\n",snd_strerror(err));
			goto fail;
		}
		if ((err = snd_pcm_hw_params_set_access(alsa_pcm, alsa_hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
			fprintf(stderr,"Failed to set interleaved PCM mode, %s\n",snd_strerror(err));
			goto fail;
		}
		if ((err = snd_pcm_hw_params_set_format(alsa_pcm, alsa_hw_params, SND_PCM_FORMAT_S16_LE)) < 0) {
			fprintf(stderr,"Failed to set S16LE, %s\n",snd_strerror(err));
			goto fail;
		}

        snd_pcm_uframes_t uft;
		unsigned int rate = 48000;
		int dir = 0;

        if ((err = snd_pcm_hw_params_set_rate_near(alsa_pcm, alsa_hw_params, &rate, &dir)) < 0) {
			fprintf(stderr,"Failed to set sample rate, %s\n",snd_strerror(err));
			goto fail;
		}

        fprintf(stderr,"ALSA chose %uHz (dir=%d)\n",rate,dir);
		if ((err = snd_pcm_hw_params_set_channels(alsa_pcm, alsa_hw_params, 2)) < 0) {
			fprintf(stderr,"Failed to set channels, %s\n",snd_strerror(err));
			goto fail;
		}

        uft = rate / 120;
        snd_pcm_hw_params_set_period_size_near(alsa_pcm, alsa_hw_params, &uft, &dir);

        fprintf(stderr,"ALSA period: %lu frames\n",(unsigned long)uft);

        uft = rate / 15;
        snd_pcm_hw_params_set_buffer_size_min(alsa_pcm, alsa_hw_params, &uft);

        uft = rate / 4;
        snd_pcm_hw_params_set_buffer_size_max(alsa_pcm, alsa_hw_params, &uft);

		if ((err = snd_pcm_hw_params(alsa_pcm, alsa_hw_params)) < 0) {
			fprintf(stderr,"Failed to apply params, %s\n",snd_strerror(err));
			goto fail;
		}

        uft = 0;
        snd_pcm_hw_params_get_buffer_size(alsa_hw_params, &uft);
        fprintf(stderr,"ALSA buffer size: %lu frames\n",(unsigned long)uft);

		snd_pcm_hw_params_free(alsa_hw_params);
		alsa_hw_params = NULL;

		if ((err = snd_pcm_prepare(alsa_pcm)) < 0) {
			fprintf(stderr,"Failed to prepare ALSA, %s\n",snd_strerror(err));
			goto fail;
		}

		audio_rate = rate;
		audio_channels = 2;
	}

	if (v4l_fd >= 0 && live_shm != NULL) {
		volatile struct live_shm_header *xx = live_shm_head();

		xx->header = LIVE_SHM_HEADER_UPDATING;
		asm volatile("" ::: "memory"); // complete writes FIRST before proceeding

		xx->width = v4l_width;
		xx->height = v4l_height;
		xx->stride = v4l_width_stride;
		xx->this_generation = ++live_shm_gen;
		xx->audio_channels = audio_channels;
		xx->audio_rate = audio_rate;
		xx->frame_size = (v4l_width_stride * v4l_height) + ((v4l_width_stride>>1) * (v4l_height>>v4l_codec_yshr) * 2);
		xx->in = 0;

		/* raw VBI for display goes at the end of the segment, after the frame slots */
		{
			size_t frames_end = live_shm_size;

			xx->vbi_width = 0;
			xx->vbi_count[0] = xx->vbi_count[1] = 0;
			xx->vbi_start[0] = xx->vbi_start[1] = 0;
			xx->vbi_offset = 0;
			xx->vbi_seq = 0;
			if (vbi_fd >= 0 && vbi_buffer != NULL && vbi_width > 0 && vbi_height > 0) {
				const size_t vbytes = (size_t)vbi_width * (size_t)vbi_height;

				if (live_shm_size > vbytes + sizeof(*xx) + (2 * (size_t)xx->frame_size)) {
					frames_end = (live_shm_size - vbytes) & (~((size_t)63));
					xx->vbi_offset = (uint32_t)frames_end;
					xx->vbi_count[0] = v4l_vbi_capfmt.fmt.vbi.count[0];
					xx->vbi_count[1] = v4l_vbi_capfmt.fmt.vbi.count[1];
					xx->vbi_start[0] = v4l_vbi_capfmt.fmt.vbi.start[0];
					xx->vbi_start[1] = v4l_vbi_capfmt.fmt.vbi.start[1];
					xx->vbi_width = vbi_width;
				}
			}
			xx->slots = (frames_end - sizeof(*xx)) / xx->frame_size;
		}

		if (v4l_codec_yshr == 0)
			xx->color_fmt = LIVE_COLOR_FMT_YUV422;	
		else
			xx->color_fmt = LIVE_COLOR_FMT_YUV420;

		asm volatile("" ::: "memory"); // complete writes FIRST before proceeding

		xx->header = LIVE_SHM_HEADER;
		asm volatile("" ::: "memory"); // complete writes FIRST before proceeding
	}

	apply_v4l_controls();

	if (watch_format && v4l_fd >= 0) {
		struct v4l2_event_subscription sub;
		struct v4l2_format f;
		int inp = 0;

		/* the format capture runs with, for drivers without source change events */
		memset(&f,0,sizeof(f));
		f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (ioctl(v4l_fd,VIDIOC_G_FMT,&f) == 0)
			watch_format_pix = f.fmt.pix;
		else
			memset(&watch_format_pix,0,sizeof(watch_format_pix));
		watch_format_next_check = NOW + 1;

		memset(&sub,0,sizeof(sub));
		sub.type = V4L2_EVENT_SOURCE_CHANGE;
		if (ioctl(v4l_fd,VIDIOC_G_INPUT,&inp) == 0)
			sub.id = (uint32_t)inp;
		watch_format_events = ioctl(v4l_fd,VIDIOC_SUBSCRIBE_EVENT,&sub) == 0;
		fprintf(stderr,"Watching for format changes (%s)\n",
			watch_format_events ? "source change events" : "no events from the driver: checking the format every second");
	}

	if (auto_v4l_avi && v4l_fd >= 0) {
		open_avi_file();
		if (avi_restart_announce && AVI != NULL) {
			announce_avi("AVI-RESTART");
			avi_restart_announce = false;
		}
	}
	return 0;
fail:
	close_v4l();
	return 1;
}

/* -watch-format: has the input format changed? (V4L2_EVENT_SOURCE_CHANGE, or for drivers
 * without events a different G_FMT once a second) */
static bool format_changed(void) {
	bool changed = false;

	if (watch_format_events) {
		struct v4l2_event ev;
		struct pollfd pfd;

		/* poll() first: an event is pending only when it reports POLLPRI. Calling
		 * VIDIOC_DQEVENT every loop iteration took the driver's device lock each time,
		 * which pexhd's input monitor holds through its I2C reads (~100 ms twice a
		 * second): the capture loop stalled and fell behind at 1080p60. */
		pfd.fd = v4l_fd;
		pfd.events = POLLPRI;
		pfd.revents = 0;
		if (poll(&pfd,1,0) <= 0 || !(pfd.revents & POLLPRI))
			return false;

		do {
			memset(&ev,0,sizeof(ev));
			if (ioctl(v4l_fd,VIDIOC_DQEVENT,&ev) != 0) break; /* ENOENT: none pending */
			if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
				fprintf(stderr,"Source change event (changes=0x%x)\n",ev.u.src_change.changes);
				changed = true;
			}
		} while (ev.pending > 0);
	}
	else if (NOW >= watch_format_next_check) {
		struct v4l2_format f;

		watch_format_next_check = NOW + 1;
		memset(&f,0,sizeof(f));
		f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (ioctl(v4l_fd,VIDIOC_G_FMT,&f) == 0 && watch_format_pix.width != 0 &&
			(f.fmt.pix.width != watch_format_pix.width ||
			 f.fmt.pix.height != watch_format_pix.height ||
			 f.fmt.pix.field != watch_format_pix.field ||
			 f.fmt.pix.pixelformat != watch_format_pix.pixelformat)) {
			fprintf(stderr,"Format changed: %ux%u field %u -> %ux%u field %u\n",
				watch_format_pix.width,watch_format_pix.height,watch_format_pix.field,
				f.fmt.pix.width,f.fmt.pix.height,f.fmt.pix.field);
			changed = true;
		}
	}

	return changed;
}

/* stop capture (and the AVI), read the new format, start again (with a new AVI file if
 * one was being recorded). If the new signal is not usable yet, the main loop retries
 * every 2 seconds and the AVI starts once capture does. */
static void restart_for_format_change(void) {
	const bool was_recording = (AVI != NULL);

	fprintf(stderr,"Input format changed: restarting capture%s\n",
		was_recording ? " and the AVI file" : "");
	close_v4l();
	avi_restart_announce = was_recording && auto_v4l_avi;
	v4l_device_retry = NOW + 2;
	if (open_v4l() != 0)
		fprintf(stderr,"New format not available yet, retrying\n");
	else
		fprintf(stderr,"Capture restarted at %ux%u\n",v4l_width,v4l_height);
}

void copy_grey_to_planar_y(unsigned char *framep[3],int stride[3],unsigned char *sdi) {
	if (framep[0]) {
		unsigned int y;
		for (y=0;y < v4l_height;y++)
			memcpy(framep[0]+(y*stride[0]),sdi+(y*v4l_width),v4l_width);
	}
	if (framep[1]) memset(framep[1],128,stride[1] * (v4l_height>>v4l_codec_yshr));
	if (framep[2]) memset(framep[2],128,stride[2] * (v4l_height>>v4l_codec_yshr));
}

/* Where field fc (0 = first in the buffer) comes from and goes to.
 *
 * Woven frames (fields interleaved line by line): buffer line y is picture line y,
 * so field fc starts on buffer line fc and lands on picture line fc. The temporal
 * order does not move any lines; it is only signalled to the encoder.
 *
 * Sequential frames (SEQ_TB/SEQ_BT): the first half of the buffer is the older field,
 * which is the top field (picture line 0) for top field first, else the bottom field.
 *
 * swap_fields exchanges the two picture positions for broken capture drivers. */
static void field_layout(unsigned int fc,unsigned int srcs,unsigned int hh,unsigned char *sdi,
	unsigned char **srf,unsigned int *dst_y,unsigned int *src_step) {
	unsigned int y;

	if (v4l_interlaced_woven) {
		*srf = sdi + (fc * srcs);
		*src_step = srcs * 2;
		y = fc;
	}
	else {
		*srf = sdi + (fc * hh * srcs);
		*src_step = srcs;
		y = (v4l_interlaced == 0) ? fc : (fc ^ 1);
	}

	if (swap_fields) y ^= 1;
	*dst_y = y;
}

/* one row of packed 4:2:2 (UYVY or YUYV) -> planar Y, U, V */
static void row_422_c(const unsigned char *src,unsigned char *Y,unsigned char *U,unsigned char *V,unsigned int w,bool uyvy) {
	const unsigned int yo = uyvy ? 1 : 0,uo = uyvy ? 0 : 1,vo = uyvy ? 2 : 3;

	for (unsigned int x=0;x < w;x += 2) {
		Y[0] = src[yo];
		Y[1] = src[yo+2];
		*U++ = src[uo];
		*V++ = src[vo];
		src += 4;
		Y += 2;
	}
}

#if defined(__x86_64__) || defined(__i386__)
#include <tmmintrin.h>

/* SSSE3: 16 pixels (32 bytes) per step; at 1080p60 the plain loop took a large share of
 * the capture thread */
__attribute__((target("ssse3")))
static void row_422_ssse3(const unsigned char *src,unsigned char *Y,unsigned char *U,unsigned char *V,unsigned int w,bool uyvy) {
	const __m128i sy = uyvy ?
		_mm_setr_epi8(1,3,5,7,9,11,13,15,-1,-1,-1,-1,-1,-1,-1,-1) :
		_mm_setr_epi8(0,2,4,6,8,10,12,14,-1,-1,-1,-1,-1,-1,-1,-1);
	const __m128i sc = uyvy ?	/* U0..U3 V0..V3 */
		_mm_setr_epi8(0,4,8,12,2,6,10,14,-1,-1,-1,-1,-1,-1,-1,-1) :
		_mm_setr_epi8(1,5,9,13,3,7,11,15,-1,-1,-1,-1,-1,-1,-1,-1);
	unsigned int x = 0;

	for (;(x+16) <= w;x += 16) {
		const __m128i a = _mm_loadu_si128((const __m128i*)src);
		const __m128i b = _mm_loadu_si128((const __m128i*)(src+16));
		const __m128i yy = _mm_unpacklo_epi64(_mm_shuffle_epi8(a,sy),_mm_shuffle_epi8(b,sy));
		const __m128i uv = _mm_unpacklo_epi32(_mm_shuffle_epi8(a,sc),_mm_shuffle_epi8(b,sc)); /* Ua Ub Va Vb */

		_mm_storeu_si128((__m128i*)Y,yy);
		_mm_storel_epi64((__m128i*)U,uv);
		_mm_storel_epi64((__m128i*)V,_mm_srli_si128(uv,8));
		src += 32; Y += 16; U += 8; V += 8;
	}
	if (x < w) row_422_c(src,Y,U,V,w-x,uyvy);
}

static void row_422(const unsigned char *src,unsigned char *Y,unsigned char *U,unsigned char *V,unsigned int w,bool uyvy) {
	static int have_ssse3 = -1;

	if (have_ssse3 < 0) have_ssse3 = __builtin_cpu_supports("ssse3") ? 1 : 0;
	if (have_ssse3) row_422_ssse3(src,Y,U,V,w,uyvy);
	else row_422_c(src,Y,U,V,w,uyvy);
}
#else
#define row_422 row_422_c
#endif

void copy_yuyv_to_planar_yuv_422(unsigned char *framep[3],int stride[3],unsigned char *sdi) {
	unsigned int render_height = v4l_height & (~7);
	unsigned int srcs = v4l_width*2;
	unsigned int hh = v4l_height>>1;
	unsigned char *Y,*U,*V;
	unsigned int field,fc;
	unsigned char *src;
	unsigned int x,y;

	if (v4l_interlaced >= 0) {
		/* fc=field count
		 * field=vertical field position */
		for (fc=0;fc < 2;fc++) {
			unsigned char *srf;
			unsigned int src_step;

			field_layout(fc,srcs,hh,sdi,&srf,&field,&src_step);

			/* 4:2:2 SDI -> 4:2:2 progressive scan */
			for (y=field;y < render_height;y += 2) {
				src = srf; srf += src_step;
				Y = framep[0] + ( (y+0)       * stride[0]);
				U = framep[1] + ( (y+0)       * stride[1]);
				V = framep[2] + ( (y+0)       * stride[2]);
				row_422(src,Y,U,V,(unsigned int)v4l_width,false);
			}
		}
	}
	else {
		/* 4:2:2 SDI -> 4:2:2 progressive scan */
		for (y=0;y < render_height;y++) {
			Y = framep[0] + ( (y+0)       * stride[0]);
			U = framep[1] + ( (y+0)       * stride[1]);
			V = framep[2] + ( (y+0)       * stride[2]);

			if (swap_fields) /* dumbass capture card drivers */
				src = (unsigned char*)sdi + (((y^1)+0) * srcs);
			else
				src = (unsigned char*)sdi + ((y+0) * srcs);

			row_422(src,Y,U,V,(unsigned int)v4l_width,false);
		}
	}
}

void copy_uyvy_to_planar_yuv_422(unsigned char *framep[3],int stride[3],unsigned char *sdi) {
	unsigned int render_height = v4l_height & (~7);
	unsigned int srcs = v4l_width*2;
	unsigned int hh = v4l_height>>1;
	unsigned char *Y,*U,*V;
	unsigned int field,fc;
	unsigned char *src;
	unsigned int x,y;

	if (v4l_interlaced >= 0) {
		/* fc=field count
		 * field=vertical field position */
		for (fc=0;fc < 2;fc++) {
			unsigned char *srf;
			unsigned int src_step;

			field_layout(fc,srcs,hh,sdi,&srf,&field,&src_step);

			/* 4:2:2 SDI -> 4:2:2 progressive scan */
			for (y=field;y < render_height;y += 2) {
				src = srf; srf += src_step;
				Y = framep[0] + ( (y+0)       * stride[0]);
				U = framep[1] + ( (y+0)       * stride[1]);
				V = framep[2] + ( (y+0)       * stride[2]);
				row_422(src,Y,U,V,(unsigned int)v4l_width,true);
			}
		}
	}
	else {
		/* 4:2:2 SDI -> 4:2:2 progressive scan */
		for (y=0;y < render_height;y++) {
			Y = framep[0] + ( (y+0)       * stride[0]);
			U = framep[1] + ( (y+0)       * stride[1]);
			V = framep[2] + ( (y+0)       * stride[2]);

			if (swap_fields) /* dumbass capture card drivers */
				src = (unsigned char*)sdi + (((y^1)+0) * srcs);
			else
				src = (unsigned char*)sdi + ((y+0) * srcs);

			row_422(src,Y,U,V,(unsigned int)v4l_width,true);
		}
	}

}

void copy_yuyv_to_planar_yuv_420(unsigned char *framep[3],int stride[3],unsigned char *sdi) {
	unsigned int render_height = v4l_height & (~7);
	unsigned int srcs = v4l_width*2;
	unsigned int hh = v4l_height>>1;
	unsigned char *Y,*U,*V;
	unsigned int field,fc;
	unsigned char *src;
	unsigned int x,y;

	if (v4l_interlaced >= 0) {
		/* fc=field count
		 * field=vertical field position */
		for (fc=0;fc < 2;fc++) {
			unsigned char *srf;
			unsigned int src_step;

			field_layout(fc,srcs,hh,sdi,&srf,&field,&src_step);

			/* 4:2:2 SDI -> 4:2:0 progressive scan */
			for (y=field;y < render_height;y += 4) {
				src = srf; srf += src_step;
				Y = framep[0] + ( (y+0)       * stride[0]);
				U = framep[1] + (((y+1) >> 1) * stride[1]);
				V = framep[2] + (((y+1) >> 1) * stride[2]);
				row_422(src,Y,U,V,(unsigned int)v4l_width,false);

				src = srf; srf += src_step;
				Y = framep[0] + ( (y+2)       * stride[0]);
				for (x=0;x < (unsigned int)v4l_width;x += 2) {
					/* Y U Y V */
					Y[0] = src[0];
					Y[1] = src[2];
					src += 4;
					Y += 2;
				}
			}
		}
	}
	else {
		/* 4:2:2 SDI -> 4:2:0 progressive scan */
		for (y=0;y < render_height;y += 2) {
			Y = framep[0] + ( (y+0)       * stride[0]);
			U = framep[1] + (((y+0) >> 1) * stride[1]);
			V = framep[2] + (((y+0) >> 1) * stride[2]);
			src = (unsigned char*)sdi + ((y+0) * srcs);
			row_422(src,Y,U,V,(unsigned int)v4l_width,false);

			Y = framep[0] + ( (y+1)       * stride[0]);
			src = (unsigned char*)sdi + ((y+1) * srcs);
			for (x=0;x < (unsigned int)v4l_width;x += 2) {
				/* Y U Y V */
				Y[0] = src[0];
				Y[1] = src[2];
				src += 4;
				Y += 2;
			}
		}
	}
}

void copy_uyvy_to_planar_yuv_420(unsigned char *framep[3],int stride[3],unsigned char *sdi) {
	unsigned int render_height = v4l_height & (~7);
	unsigned int srcs = v4l_width*2;
	unsigned int hh = v4l_height>>1;
	unsigned char *Y,*U,*V;
	unsigned int field,fc;
	unsigned char *src;
	unsigned int x,y;

	if (v4l_interlaced >= 0) {
		/* fc=field count
		 * field=vertical field position */
		for (fc=0;fc < 2;fc++) {
			unsigned char *srf;
			unsigned int src_step;

			field_layout(fc,srcs,hh,sdi,&srf,&field,&src_step);

			/* 4:2:2 SDI -> 4:2:0 progressive scan */
			for (y=field;y < render_height;y += 4) {
				src = srf; srf += src_step;
				Y = framep[0] + ( (y+0)       * stride[0]);
				U = framep[1] + (((y+1) >> 1) * stride[1]);
				V = framep[2] + (((y+1) >> 1) * stride[2]);
				row_422(src,Y,U,V,(unsigned int)v4l_width,true);

				src = srf; srf += src_step;
				Y = framep[0] + ( (y+2)       * stride[0]);
				for (x=0;x < (unsigned int)v4l_width;x += 2) {
					/* U Y V Y */
					Y[0] = src[1];
					Y[1] = src[3];
					src += 4;
					Y += 2;
				}
			}
		}
	}
	else {
		/* 4:2:2 SDI -> 4:2:0 progressive scan */
		for (y=0;y < render_height;y += 2) {
			Y = framep[0] + ( (y+0)       * stride[0]);
			U = framep[1] + (((y+0) >> 1) * stride[1]);
			V = framep[2] + (((y+0) >> 1) * stride[2]);
			src = (unsigned char*)sdi + ((y+0) * srcs);
			row_422(src,Y,U,V,(unsigned int)v4l_width,true);

			Y = framep[0] + ( (y+1)       * stride[0]);
			src = (unsigned char*)sdi + ((y+1) * srcs);
			for (x=0;x < (unsigned int)v4l_width;x += 2) {
				/* U Y V Y */
				Y[0] = src[1];
				Y[1] = src[3];
				src += 4;
				Y += 2;
			}
		}
	}
}

void socket_index_msg(const char *typ,unsigned long long frame,unsigned long long base,unsigned long length,bool keyframe=true) {
	char msg[4096];
	size_t len;

	len = sprintf(msg,"INDEX-UPDATE\n"
		"Frame:%llu\n"
		"Base:%llu\n"
		"Length:%lu\n"
		"Key:%u\n",
		frame,base,length,keyframe?1:0);

	if (send(comm_socket_fd,msg,len,MSG_DONTWAIT) < 0) {
//		fprintf(stderr,"Unable to send index update. The GUI must be really distracted or confused.\n");
	}
}

void socket_MSG(const char *msg) {
	if (comm_socket_fd < 0) return; /* no GUI (run from the command line) */
	while (send(comm_socket_fd,msg,strlen(msg),0) < 0) usleep(10000);
}

void process_socket_io() {
	char msg[4096],*p,*n;
	int rd;

	rd = recv(comm_socket_fd,msg,sizeof(msg),MSG_DONTWAIT);
	if (rd <= 0) return;
	msg[rd] = 0;

	n = strchr(msg,'\n');
	if (n) *n++ = 0;

	if (!strncmp(msg,"bright ",7)) {
		char *v = msg + 7;
		while (*v == ' ') v++;
		v4l_brightness = (int)(atof(v) * 10000);
		apply_v4l_controls();
	}
	else if (!strncmp(msg,"contrast ",9)) {
		char *v = msg + 9;
		while (*v == ' ') v++;
		v4l_contrast = (int)(atof(v) * 10000);
		apply_v4l_controls();
	}
	else if (!strcmp(msg,"monitor-on")) {
		audio_monitor = true;
		alsa_play_retry = 0;
		fprintf(stderr,"Audio monitor on\n");
	}
	else if (!strcmp(msg,"monitor-off")) {
		audio_monitor = false;
		monitor_stop();
	}
	else if (!strncmp(msg,"monitor-device ",15)) {
		char *v = msg + 15;
		while (*v == ' ') v++;
		string dev = (*v != 0) ? v : "default";
		if (dev != audio_monitor_device) {
			audio_monitor_device = dev;
			monitor_stop(); /* reopened on the new device if monitoring */
			alsa_play_retry = 0;
		}
	}
	else if (!strcmp(msg,"capture-on")) {
		auto_v4l_v4l = true;
	}
	else if (!strcmp(msg,"capture-off")) {
		auto_v4l_v4l = false;
	}
	else if (!strcmp(msg,"record-on")) {
		auto_v4l_avi = true;
		v4l_open_shut_up = 0;
		if (v4l_fd < 0) open_v4l();
		if (v4l_fd >= 0) open_avi_file();
		avi_restart_announce = false;	/* this reply announces it */
		if (AVI != NULL) {
			/* TODO: Announce the audio format when the GUI supports audio playback */
			announce_avi("OK");
		}
		else {
			fprintf(stderr,"Failed to start recording.\n");
			socket_MSG("FAILED");
		}
	}
	else if (!strcmp(msg,"record-off")) {
		auto_v4l_avi = false;
		avi_restart_announce = false;
		close_avi_file();
	}
	else {
		fprintf(stderr,"Unknown command '%s'\n",msg);
	}
}

uint32_t scribble_marker = 0;

bool scribble_mark_detect(unsigned int stride,unsigned int height,unsigned char *ptr) {
	unsigned int found = 0;
	unsigned int y;

	if (stride < (4*5)) return false;
	if (scribble_marker == 0) return false;

	unsigned int so = (stride - (4*5)) / 2;

	for (y=0;y < height;y++) {
		if (	((uint32_t*)(ptr+so))[0] == 0xFFFFFFFF &&
			((uint32_t*)(ptr+so))[1] == 0x00000000 &&
			((uint32_t*)(ptr+so))[2] == scribble_marker &&
			((uint32_t*)(ptr+so))[3] == ~scribble_marker &&
			((uint32_t*)(ptr+so))[4] == 0xADADADAD) {
			found++;
		}
		ptr += stride;
	}

	return found >= ((height*7)/8);
}

void scribble_mark_frame(unsigned int stride,unsigned int height,unsigned char *ptr) {
	unsigned int y;

	if (scribble_marker == 0) scribble_marker = (uint32_t)rand() * (uint32_t)rand();
	if (stride < (4*5)) return;

	unsigned int so = (stride - (4*5)) / 2;

	for (y=0;y < height;y++) {
		/* put it right down the middle of the frame using a pattern very unlikely to appear in video capture.
		 * furthermore the random nature of the mark makes it very unlikely. */
		((uint32_t*)(ptr+so))[0] = 0xFFFFFFFF;
		((uint32_t*)(ptr+so))[1] = 0x00000000;
		((uint32_t*)(ptr+so))[2] = scribble_marker;
		((uint32_t*)(ptr+so))[3] = ~scribble_marker;
		((uint32_t*)(ptr+so))[4] = 0xADADADAD;
		ptr += stride;
	}
}

int main(int argc,char **argv) {
	/* sync */
	signed long long audio_drift = 0;
	double next_audio_check = 0;
	/* other */
	double sleep_until = 0;
	int rd;

	/* FIXME: Someday we should also support ALSA */
	system("modprobe snd-pcm-oss >/dev/null 2>&1");

	strcpy(v4l_devname,"");

	if (parse_argv(argc,argv))
		return 1;
//	if (sched_self_realtime())
//		fprintf(stderr,"Warning: cannot make myself a realtime process\n");

	if (comm_socket != "") {
		struct sockaddr_un un;
		struct stat st;

		comm_socket_fd = socket(AF_UNIX,SOCK_SEQPACKET,0);
		if (comm_socket_fd < 0) {
			fprintf(stderr,"Cannot create socket\n");
			return 1;
		}

		memset(&un,0,sizeof(un));
		un.sun_family = AF_UNIX;
		if ((comm_socket.length()+2) >= sizeof(un.sun_path)) {
			fprintf(stderr,"Socket path too long\n");
			return 1;
		}
		strcpy(un.sun_path,comm_socket.c_str());
		if (connect(comm_socket_fd,(struct sockaddr*)(&un),sizeof(un))) {
			fprintf(stderr,"Cannot connect to socket %s\n",strerror(errno));
			return 1;
		}
	}

	if (live_shm_name != "") {
		/* IMPORTANT: We open the file read/write but set the permissions to read only.
		 *            We don't want other processes writing to our buffer */
		live_shm_fd = shm_open(live_shm_name.c_str(),O_RDWR|O_CREAT|O_TRUNC|O_EXCL,0444);
		if (live_shm_fd < 0) {
			fprintf(stderr,"Unable to create shared memory segment '%s'\n",live_shm_name.c_str());
			return 1;
		}

		if (ftruncate(live_shm_fd,live_shm_size_req) < 0) {
			shm_unlink(live_shm_name.c_str());
			close(live_shm_fd);
			fprintf(stderr,"Unable to expand shared memory segment '%s'\n",live_shm_name.c_str());
			return 1;
		}

		live_shm_size = (live_shm_size_req + 0xFFFUL) & (~0xFFFUL);
		live_shm = (unsigned char*)mmap(NULL,live_shm_size,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_POPULATE,live_shm_fd,0);
		if (live_shm == (unsigned char*)(-1)) {
			shm_unlink(live_shm_name.c_str());
			close(live_shm_fd);
			fprintf(stderr,"Unable to map shared memory segment '%s'\n",live_shm_name.c_str());
			return 1;
		}

		/* set up the struct, indicate nothing to offer */
		volatile struct live_shm_header *xx = live_shm_head();
		xx->in = 0;
		xx->slots = 0;
		xx->width = 0;
		xx->height = 0;
		xx->stride = 0;
		xx->frame_size = 0;
		xx->header = LIVE_SHM_HEADER;
		xx->color_fmt = 0;
	}

	signal(SIGINT, sigma);
	signal(SIGQUIT,sigma);
	signal(SIGTERM,sigma);

	while (!DIE) {
		update_now_time();
		if (sleep_until < NOW) sleep_until = NOW;
		else if (sleep_until > (NOW+0.1)) sleep_until = NOW+0.1;

		if (comm_socket_fd >= 0)
			process_socket_io();

		if (v4l_fd >= 0 && !auto_v4l_v4l && !auto_v4l_avi)
			close_v4l();

		if (v4l_fd >= 0 && watch_format && format_changed())
			restart_for_format_change();

		if (v4l_fd < 0) {
			if (NOW >= v4l_device_retry && (auto_v4l_v4l || auto_v4l_avi)) {
				v4l_device_retry = NOW + 2;
				open_v4l();
			}
		}
		else {
			struct v4l2_buffer *vb = NULL;
			unsigned char *ptr = NULL;
			double framt = -1.0;

			memset(&v4l_buf[v4l_bufptr],0,sizeof(v4l_buf[v4l_bufptr]));
			v4l_buf[v4l_bufptr].index = v4l_bufptr;
			v4l_buf[v4l_bufptr].type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			v4l_buf[v4l_bufptr].memory = V4L2_MEMORY_MMAP;
			if (ioctl(v4l_fd,VIDIOC_QUERYBUF,&v4l_buf[v4l_bufptr]) == 0) {
				if (v4l_buf[v4l_bufptr].flags & V4L2_BUF_FLAG_DONE) {
					if (ioctl(v4l_fd,VIDIOC_DQBUF,&v4l_buf[v4l_bufptr]) == 0) {
						vb = &v4l_buf[v4l_bufptr];
						/* DQBUF returns the oldest completed buffer, which is not
						 * necessarily the one we asked about: use the one it returned
						 * (the QBUF below requeues that one too) */
						if (vb->index < (unsigned int)v4l_buffers)
							ptr = v4l_ptr[vb->index];
						else
							ptr = v4l_ptr[v4l_bufptr];
						if (vb->timestamp.tv_sec > 0 && AVI != NULL) {
							/* NOTE: The Linux kernel API says the timestamp can come from the device,
							 *       or from other parts of the Video4Linux API. It never said anything
							 *       about the time matching the system clock, it could be anything.
							 *       So we have to subtract and add to translate the clock value.
							 *
							 *       2019/12/24: Testing with a USB capture device shows timestamps that
							 *                   match /proc/uptime! */
							framt = (double)vb->timestamp.tv_sec +
								(((double)vb->timestamp.tv_usec) / 1000000);

							if (v4l_basetime < 0) {
								/* The V4L linux API helpfully allows the driver to indicate if the timestamp is monotonic time */
								if ((vb->flags&V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
									fprintf(stderr,"Timestamp init: V4L driver is using monotonic time, yay!\n");
									v4l_basetime = avi_file_start_monotime;
								}
								else {
									fprintf(stderr,"Timestamp init: V4L driver is not using monotonic time, boo...\n");
									v4l_basetime = framt - (monoNOW - avi_file_start_monotime);
								}
							}

							framt -= v4l_basetime;
							if (framt < 0) framt = 0;
						}

						/* Linux cx23885 hack: Why does the card report FIELD_INTERLACED for get/set FMT ioctls
						 * and then report V4L2_FIELD_ANY here?????? Another complaint: If you capture VHS tapes
						 * and the video signal is unstable in just the right way the captured fields change places.
						 *
						 * Also despite what the source code suggests, the driver will only accept FIELD_INTERLACED */
						if (vb->field == V4L2_FIELD_ANY && v4l_fmt.fmt.pix.field == V4L2_FIELD_INTERLACED)
							vb->field = V4L2_FIELD_INTERLACED;

						v4l_set_field_order(vb->field);
					}
					else {
						fprintf(stderr,"Cannot dq buf %u\n",v4l_bufptr);
					}
				}
			}
			else {
				fprintf(stderr,"Cannot query buffer %u\n",v4l_bufptr);
			}

			if (ptr != NULL) {
				bool ignore_frame = false;

				if (scribble_mark_detect(v4l_stride,v4l_height,ptr)) {
					fprintf(stderr,"Duplicate frame returned by capture card, ignoring\n");
					ignore_frame = true;
				}

				AVFrame *av_frame = av_frame_alloc();

				av_frame->top_field_first = (effective_v4l_interlaced() == 0) ? 1 : 0;
				av_frame->interlaced_frame = (effective_v4l_interlaced() >= 0);
				av_frame->key_frame = (avi_frame_counter % AVI_FRAMES_PER_GROUP) == 0;
				av_frame->pts = AV_NOPTS_VALUE;
				av_frame->width = v4l_width;
				av_frame->height = v4l_height;

				if (fmp4_context != NULL)
					av_frame->format = fmp4_context->pix_fmt;

				if (ignore_frame) {
				}
				else if (v4l_fd >= 0 && live_shm != NULL) {
					volatile struct live_shm_header *xx = live_shm_head();
					assert(xx->header == LIVE_SHM_HEADER);
					assert(xx->width != 0);
					assert(xx->height != 0);
					assert(xx->stride != 0);
					assert(xx->frame_size != 0);
					assert(xx->slots != 0);
					assert(xx->in >= 0 && xx->in < xx->slots);

					/* direct conversion to the shared memory segment, convert only once */
					av_frame->data[0] = live_shm + sizeof(struct live_shm_header) +
						(xx->frame_size * xx->in);
					av_frame->linesize[0] = v4l_width_stride;
					av_frame->data[1] = av_frame->data[0] + (v4l_width_stride * v4l_height);
					av_frame->linesize[1] = v4l_width_stride >> 1;
					av_frame->data[2] = av_frame->data[1] + ((v4l_width_stride/2) * (v4l_height>>v4l_codec_yshr));
					av_frame->linesize[2] = v4l_width_stride >> 1;
					assert((av_frame->data[2] + (av_frame->linesize[2]*(v4l_height>>v4l_codec_yshr))) <= (live_shm + live_shm_size));

					if (v4l_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_GREY) {
						/* A curious grayscale capture mode of the webcam in my laptop that causes red LEDs to flash.
						 * Video capture with depth? */
						copy_grey_to_planar_y(av_frame->data,av_frame->linesize,ptr);
					}
					else if (v4l_codec_yshr == 0) {
						if (v4l_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV)
							copy_yuyv_to_planar_yuv_422(av_frame->data,av_frame->linesize,ptr);
						else if (v4l_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_UYVY)
							copy_uyvy_to_planar_yuv_422(av_frame->data,av_frame->linesize,ptr);
					}
					else if (v4l_codec_yshr == 1) {
						if (v4l_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV)
							copy_yuyv_to_planar_yuv_420(av_frame->data,av_frame->linesize,ptr);
						else if (v4l_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_UYVY)
							copy_uyvy_to_planar_yuv_420(av_frame->data,av_frame->linesize,ptr);
					}

					unsigned int ch=0;
					for (;ch < audio_channels;ch++) {
						xx->map[xx->in].audio_max_level[ch] = audio_max_level[ch];
//						xx->map[xx->in].audio_avg_level[ch] = (uint32_t)audio_sum_level[ch] / (audio_sum_count != 0 ? audio_sum_count : 1);
//						fprintf(stderr,"ch=%u max=%u avg=%u count=%u\n",
//							ch,
//							xx->map[xx->in].audio_max_level[ch],
//							xx->map[xx->in].audio_avg_level[ch],
//							audio_sum_count);

						audio_sum_level[ch] = 0;
						if (audio_max_level[ch] >= 256)
							audio_max_level[ch] -= 256;
						else
							audio_max_level[ch] = 0;
					}
					audio_sum_count = 0;
					for (;ch < 4;ch++) {
						xx->map[xx->in].audio_max_level[ch] = 0;
//						xx->map[xx->in].audio_avg_level[ch] = 0;
					}
					xx->map[xx->in].offset = (uint32_t)((size_t)av_frame->data[0] - (size_t)live_shm);
					xx->map[xx->in].generation = xx->this_generation;
					xx->map[xx->in].field_order = effective_v4l_interlaced() + 1;
					if ((++xx->in) == xx->slots) {
						xx->this_generation++;
						xx->in = 0;
					}
				}
				else {
					av_frame->data[0] = fmp4_yuv;
					av_frame->linesize[0] = v4l_width_stride;
					av_frame->data[1] = av_frame->data[0] + (v4l_width_stride * v4l_height);
					av_frame->linesize[1] = v4l_width_stride >> 1;
					av_frame->data[2] = av_frame->data[1] + ((v4l_width_stride/2) * (v4l_height>>v4l_codec_yshr));
					av_frame->linesize[2] = v4l_width_stride >> 1;

					if (v4l_codec_yshr == 0) {
						if (v4l_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV)
							copy_yuyv_to_planar_yuv_422(av_frame->data,av_frame->linesize,ptr);
						else if (v4l_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_UYVY)
							copy_uyvy_to_planar_yuv_422(av_frame->data,av_frame->linesize,ptr);
					}
					else if (v4l_codec_yshr == 1) {
						if (v4l_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV)
							copy_yuyv_to_planar_yuv_420(av_frame->data,av_frame->linesize,ptr);
						else if (v4l_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_UYVY)
							copy_uyvy_to_planar_yuv_420(av_frame->data,av_frame->linesize,ptr);
					}
				}

				if (AVI && !ignore_frame) {
					if (AVI_video) {
						AVPacket *pkt;
						int gotit=0;

						if (framt >= 0) {
							double t = (framt * v4l_framerate_n) / v4l_framerate_d;
							if (t < -100) fprintf(stderr,"Warning: Frame time away behind AVI start time t=%.3f framt=%.3f avi_start=%.3f bt=%.3f\n",t,framt,avi_file_start_time,v4l_basetime);
							if (t < 0) t = 0;
							unsigned long long n = (unsigned long long)floor(t+0.5);

							if (avi_frame_counter < n) {
								int patience = 1000;
								while (patience-- > 0 && avi_frame_counter < n) {
									avi_frame_counter++;
								}
							}
							else if (avi_frame_counter > (n+3ull)) {
								/* My Mirabox HDMI capture card seems to have a weird problem with 1080p60
								 * where it allows 60fps capture but it seems to send frames at like 61fps.
								 * The distance between avi_frame_counter and n slowly grows ever larger and
								 * manifests itself as a slow loss of A/V sync in the capture, often to about
								 * 3-6 seconds of A/V sync error within 1-2 minutes. It doesn't do this if you
								 * capture a 1080p60 source at 30fps nor does it do this with a 1080p60 source
								 * captured as 720p60. Weird. */
								double adj = ((double)(avi_frame_counter-(n+3ull)) * v4l_framerate_d) / v4l_framerate_n;
								if (adj > 0.25) adj = 0.25;

								fprintf(stderr,"avi_frame_counter exceeds source frame time (%llu > %llu) source must be sending a little fast (adj=%.3f)\n",
										avi_frame_counter,n,adj);

								avi_file_start_monotime -= adj;
								avi_file_start_time -= adj;
								v4l_basetime -= adj;
							}
						}

						v4l_last_frame_delta = avi_frame_counter - v4l_last_frame;
						v4l_last_frame = avi_frame_counter;

						/* encode to MPEG-4 and store */
						pkt = av_packet_alloc(); /* FFMPEG 4.4 no longer likes AVPacket allocated on stack */
						assert(pkt != NULL);
						// NTS: Unlike FFMPEG 4.4.x, the packet coming back will point to it's own allocated buffers
						//      and it will ignore our pointers passed in. FFMPEG 4.4.x also has this API.
						rd = -1;
						av_frame->pts = avi_frame_counter;
						AVFrame *hw_frame = NULL;
						if (fmp4_vaapi) {
							if ((hw_frame = vaapi_upload(av_frame)) == NULL)
								fprintf(stderr,"VAAPI: upload of frame %llu failed\n",avi_frame_counter);
						}
						else if (fmp4_copies_input && av_frame->buf[0] == NULL)
							av_frame->buf[0] = av_buffer_create(av_frame->data[0],
								(int)(v4l_width_stride * v4l_height * 2),no_free,NULL,0);
						if ((!fmp4_vaapi || hw_frame != NULL) &&
							avcodec_send_frame(fmp4_context,hw_frame ? hw_frame : av_frame) >= 0) {
							rd = 0;
							if (avcodec_receive_packet(fmp4_context,pkt) >= 0) {
								rd = pkt->size;
								gotit = 1;
							}
						}
						if (rd < 0) {
							printf("Unable to encode frame rd=%d\n",rd);
						}
						else if (gotit == 0 || pkt->data == NULL || pkt->size == 0) {
							printf("Hm? No output on %llu? rd=%d gotit=%u data=%p size=%d\n",
								(unsigned long long)avi_frame_counter,rd,gotit,pkt->data,pkt->size);
						}
						else {
							rd = pkt->size;
							if (rd > 0) {
								int patience = 1000;

								if (!async_io) {
									while (AVI_video->sample_write_chunk < pkt->dts && patience-- > 0)
										avi_writer_stream_write(AVI,AVI_video,NULL,0,0);
								}

								if (async_io) {
									AVIPacket *p = new AVIPacket();
									p->stream = AVI_STREAM_VIDEO;
									p->set_data(pkt->data,rd);
									p->set_flags((pkt->flags&AV_PKT_FLAG_KEY) ? riff_idx1_AVIOLDINDEX_flags_KEYFRAME : 0);
									p->target_chunk = pkt->dts;
									async_avi_queue_add(&p);
								}
								else {
									if (avi_writer_stream_write(AVI,AVI_video,pkt->data,rd,
										(pkt->flags&AV_PKT_FLAG_KEY) ? riff_idx1_AVIOLDINDEX_flags_KEYFRAME : 0)) {
										avi_writer_stream_index *si = AVI_video->sample_index + AVI_video->sample_write_chunk - 1;
										socket_index_msg("V",pkt->dts,si->offset,si->length,(pkt->flags&AV_PKT_FLAG_KEY)?1:0);
									}
								}
							}
							else {
								printf("Hm? No output on %llu\n",avi_frame_counter);
							}
						}

						av_packet_unref(pkt);
						av_packet_free(&pkt);
						av_frame_free(&hw_frame);
						avi_frame_counter++;
					}
				}

				if (async_io && AVI != NULL && AVI_video != NULL && !ignore_frame) {
					async_avi_queue_lock();

					while (v4l_video_async_track < AVI_video->sample_write_chunk) {
						avi_writer_stream_index *si = AVI_video->sample_index + v4l_video_async_track;

						if (si->length != 0 || (si->dwFlags & AV_PKT_FLAG_KEY) != 0) {
							socket_index_msg("V",v4l_video_async_track,si->offset,si->length,(si->dwFlags&riff_idx1_AVIOLDINDEX_flags_KEYFRAME)?1:0);
							v4l_video_async_track++;
							break;
						}
						else {
							v4l_video_async_track++;
						}
					}

					async_avi_queue_unlock();
				}

				/* I'm seeing problems with capture cards returning frames where nothing changed.
				 * To detect that, scribble on the frame before queueing the buffer and see if
				 * the marks come back unchanged. */
				scribble_mark_frame(v4l_stride,v4l_height,ptr);

				vb->timestamp.tv_sec = 0;
				vb->timestamp.tv_usec = 0;
				if (ioctl(v4l_fd,VIDIOC_QBUF,&v4l_buf[v4l_bufptr]) == 0) {
					if (++v4l_bufptr >= v4l_buffers)
						v4l_bufptr = 0;
				}

				av_frame_unref(av_frame);
				av_frame_free(&av_frame);
			}
		}

		/* VBI */
		if (vbi_fd >= 0 && vbi_buffer != NULL) {
			double framt = monoNOW;
			AVFrame *av_frame;
			int rd,expect;

			av_frame = av_frame_alloc();
			av_frame->key_frame = (avi_vbi_frame_counter % AVI_FRAMES_PER_GROUP) == 0;
			av_frame->pts = AV_NOPTS_VALUE;
			av_frame->width = vbi_width;
			av_frame->height = vbi_enc_height;

			if (fmp4_vbi_context != NULL)
				av_frame->format = fmp4_vbi_context->pix_fmt;

			assert(vbi_read_field <= 1);
			expect = vbi_stride * v4l_vbi_capfmt.fmt.vbi.count[vbi_read_field];
			if (expect != 0) {
				rd = read(vbi_fd,fmp4_temp,expect);
				if (rd == expect) {
					/* transfer to buffer */
					for (unsigned int sy=0;sy < v4l_vbi_capfmt.fmt.vbi.count[vbi_read_field];sy++) {
						unsigned int cpy = std::min((unsigned int)vbi_stride,(unsigned int)vbi_width);
						unsigned int dy = (vbi_read_field ? v4l_vbi_capfmt.fmt.vbi.count[0] : 0) + sy;
						assert(dy < vbi_height);
						memcpy(vbi_buffer+(dy*vbi_width),fmp4_temp+(sy*vbi_stride),cpy);
					}

					/* progressive (count[1] == 0): one read is the whole frame */
					if (v4l_vbi_capfmt.fmt.vbi.count[1] != 0)
						vbi_read_field ^= 1;
					if (!vbi_read_field)
						vbi_publish_live(); /* both fields in: show them in the GUI */

					if (!vbi_read_field && AVI && fmp4_vbi_context != NULL) {
						/* direct conversion to the shared memory segment, convert only once */
						av_frame->data[0] = vbi_buffer;
						av_frame->linesize[0] = vbi_width;

						assert((vbi_width*vbi_enc_height) <= sizeof(fmp4_yuv));
						memset(fmp4_yuv,128,vbi_width*vbi_enc_height);

						av_frame->data[1] = fmp4_yuv;
						av_frame->linesize[1] = vbi_width >> 1;
						av_frame->data[2] = fmp4_yuv;
						av_frame->linesize[2] = vbi_width >> 1;

						AVPacket *pkt;
						int gotit=0;

						if (framt > 0) {
							double t = ((framt - avi_file_start_monotime) * v4l_framerate_n) / v4l_framerate_d;
							if (t < -100) fprintf(stderr,"Warning: Frame time away behind AVI start time t=%.3f framt=%.3f avi_start=%.3f bt=%.3f\n",t,framt,avi_file_start_time,v4l_basetime);
							if (t < 0) t = 0;
							unsigned long long n = (unsigned long long)floor(t+0.5);

							if (avi_vbi_frame_counter < n) {
								int patience = 1000;
								while (patience-- > 0 && avi_vbi_frame_counter < n) {
									avi_vbi_frame_counter++;
								}
							}
						}

						v4l_last_vbi_delta = avi_vbi_frame_counter - v4l_last_vbi;
						v4l_last_vbi = avi_vbi_frame_counter;

						/* encode to MPEG-4 and store */
						pkt = av_packet_alloc();
						assert(pkt != NULL);
						// NTS: Unlike FFMPEG 4.4.x, the packet coming back will point to it's own allocated buffers
						//      and it will ignore our pointers passed in. FFMPEG 4.4.x also has this API.
						rd = -1;
						av_frame->pts = avi_frame_counter;
						if (avcodec_send_frame(fmp4_vbi_context,av_frame) >= 0) {
							rd = 0;
							if (avcodec_receive_packet(fmp4_vbi_context,pkt) >= 0) {
								rd = pkt->size;
								gotit = 1;
							}
						}
						if (rd < 0) {
							printf("Unable to encode frame rd=%d\n",rd);
						}
						else if (gotit == 0 || pkt->data == NULL || pkt->size == 0) {
							printf("Hm? No output on %llu? rd=%d gotit=%u data=%p size=%d\n",
								(unsigned long long)avi_frame_counter,rd,gotit,pkt->data,pkt->size);
						}
						else {
							rd = pkt->size;
							if (rd > 0) {
								int patience = 1000;

								if (!async_io) {
									while (AVI_vbi_video->sample_write_chunk < pkt->dts && patience-- > 0)
										avi_writer_stream_write(AVI,AVI_vbi_video,NULL,0,0);
								}

								if (async_io) {
									AVIPacket *p = new AVIPacket();
									p->stream = AVI_STREAM_VBI;
									p->set_data(pkt->data,rd);
									p->set_flags((pkt->flags&AV_PKT_FLAG_KEY) ? riff_idx1_AVIOLDINDEX_flags_KEYFRAME : 0);
									p->target_chunk = pkt->dts;
									async_avi_queue_add(&p);
								}
								else {
									avi_writer_stream_write(AVI,AVI_vbi_video,pkt->data,rd,
										(pkt->flags&AV_PKT_FLAG_KEY) ? riff_idx1_AVIOLDINDEX_flags_KEYFRAME : 0);
								}
							}
							else {
								printf("Hm? No output on %llu\n",avi_vbi_frame_counter);
							}
						}

						av_packet_unref(pkt);
						av_packet_free(&pkt);
						avi_vbi_frame_counter++;
					}
				}
				else if (rd == 0 || (rd < 0 && errno == EAGAIN)) {
				}
				else if (rd >= 0) {
					fprintf(stderr,"VBI unexpected read=%d expect=%d\n",rd,expect);
				}
				else if (errno != EINTR) {
					/* the driver cannot deliver VBI for this input (pexhd: HDMI, VGA, 1080p:
					 * ENOLINK). Retrying every loop iteration only costs time in the
					 * driver: stop VBI until capture is reopened (format change, reopen) */
					fprintf(stderr,"VBI read error %s: VBI off until capture restarts\n",strerror(errno));
					close(vbi_fd);
					vbi_fd = -1;
				}
			}
			else {
				vbi_read_field ^= 1;
			}

			av_frame_unref(av_frame);
			av_frame_free(&av_frame);

		}

		/* ALSA audio */
		if (alsa_pcm != NULL) {
			int err;
			snd_pcm_sframes_t min_avail = (snd_pcm_sframes_t)(audio_rate / 30);
			snd_pcm_sframes_t avail=0,delay=0;
			snd_pcm_uframes_t max;
			snd_pcm_sframes_t done;

			/* update time */
			update_now_time();

			/* open/close the audio monitor as requested */
			monitor_update_state();

			/* So ALSA, how much is waiting for us? */
			err = snd_pcm_avail_delay(alsa_pcm, &avail, &delay);

			/* ALSA thinks in "frames" not bytes.
			 * don't read the data until we have 1/10th of a second ready to avoid AVI chunk overhead. */
			if (avail >= min_avail || avail == 0/*why does ALSA not return any count until we readi?*/) {
                if (async_io) {
                    /* the reason we check here is audio capture limits the check interval to 15-30 times a second */
                    async_avi_queue_lock();
                    if (async_avi_queue.size() >= 256)
                        fprintf(stderr,"WARNING: Async I/O queue backup, %zu items\n",async_avi_queue.size());
                    async_avi_queue_unlock();
                }

				max = sizeof(fmp4_temp) / sizeof(int16_t) / audio_channels;
				done = snd_pcm_readi(alsa_pcm, fmp4_temp, max);

				if (done > 0) {
					unsigned long b = (unsigned long)done * sizeof(int16_t) * audio_channels;
					assert(b <= sizeof(fmp4_temp));

					if (avail < done)
						avail = done;

					// metering
					for (unsigned int samp=0;samp < (unsigned int)done;samp++) {
						int16_t *s = (int16_t*)fmp4_temp + (samp * audio_channels);
						for (unsigned int ch=0;ch < (unsigned int)audio_channels;ch++) {
							audio_sum_level[ch] += (unsigned int)abs(s[ch]);
							audio_max_level[ch] = std::max(audio_max_level[ch],(unsigned int)abs(s[ch]));
						}

                        audio_sum_count++;
                    }

					// listen: copy to the monitor output (never blocks, drops if it cannot keep up)
					monitor_push((const int16_t*)fmp4_temp,(size_t)done);

					if (AVI) {
						if (AVI_audio) {
							// what time should we be at?
							unsigned long long samp_should = 0;
							double tt;

							// time to sample
							tt = monoNOW - avi_file_start_monotime;
							if (tt < 0) tt = 0;
							samp_should = (unsigned long long)(tt * audio_rate);
							if (samp_should >= avail)
								samp_should -= avail;
							else
								samp_should = 0;

#if 0 // NTS: Capture delay according to ALSA seems to be buffer size + available.
      //      This is causing visible A/V sync error in my captures. Stop doing this.
							// then consider what ALSA says is the delay
							if (samp_should >= (unsigned long long)delay)
								samp_should -= (unsigned long long)delay;
							else
								samp_should = 0ULL;
#endif

							// smooth out the difference. if it is consistently off we'll adjust it.
							if (fabs((double)samp_should - (double)avi_audio_samples) >= ((double)audio_rate * 0.5)) {
								avi_audio_err = (double)samp_should - (double)avi_audio_samples;
							}
							else {
								const double smoothing = 0.1;

								avi_audio_err =
									(avi_audio_err * smoothing) +
									(((double)samp_should - (double)avi_audio_samples) * (1.0 - smoothing));
							}

							// add/remove padding to keep sync
							// keep noise down by printing only if delta is not 1
							if (v4l_last_frame_delta != 1 || (capture_vbi && v4l_last_vbi_delta != 1)) {
							fprintf(stderr,"AVI A/V err at %.3f: %.6f samp=%llu should=%llu vidlast=%llu+%llu vbilast=%llu+%llu\n",tt,
                                avi_audio_err,(unsigned long long)avi_audio_samples,(unsigned long long)samp_should,
                                (unsigned long long)v4l_last_frame,(unsigned long long)v4l_last_frame_delta,
                                (unsigned long long)v4l_last_vbi,  (unsigned long long)v4l_last_vbi_delta);
							}

							if (avi_audio_err >= ((double)audio_rate * 0.02)) {
								unsigned int pad = (unsigned int)(avi_audio_err);
								unsigned int pi;

								fprintf(stderr,"AVI audio stream is behind at %.3f, adding padding %u samples\n",tt,pad);

								if (AVI_logfile != NULL)
									fprintf(AVI_logfile,"AVI audio stream is behind at %.3f, adding padding %u samples at %llu samples\n",
										tt,pad,(unsigned long long)avi_audio_samples);

								/* write a distinctive pattern so future software can distinguish our padding */
								if ((pad*audio_channels*sizeof(int16_t)) > sizeof(fmp4_yuv))
									pad = sizeof(fmp4_yuv) / audio_channels / sizeof(int16_t);

								avi_audio_err -= pad;
								for (pi=0;pi < (pad*audio_channels);pi++)
									((int16_t*)fmp4_yuv)[pi] = pi & 1; /* 0 1 0 1 ... */

                                // write audio
                                if (async_io) {
                                    AVIPacket *p = new AVIPacket();
                                    p->stream = AVI_STREAM_AUDIO;
                                    p->set_data(fmp4_yuv,pad*audio_channels*sizeof(int16_t));
                                    p->set_flags(riff_idx1_AVIOLDINDEX_flags_KEYFRAME);
                                    async_avi_queue_add(&p);
                                }
                                else {
                                    avi_writer_stream_write(AVI, AVI_audio, fmp4_yuv, pad*audio_channels*sizeof(int16_t), riff_idx1_AVIOLDINDEX_flags_KEYFRAME);
                                }

								avi_audio_samples += (unsigned long)pad;
							}
							else if (avi_audio_err <= -((double)audio_rate * 0.1)) {
								double pad = (-avi_audio_err) / audio_rate;
								if (pad > 0.1) pad = 0.1;

								fprintf(stderr,"AVI audio stream is ahead at %.3f, slowing video down %.6f seconds\n",tt,pad);

								if (AVI_logfile != NULL)
									fprintf(AVI_logfile,"AVI audio stream is ahead at %.3f, slowing video down %.6f seconds at %llu samples\n",
										tt,pad,(unsigned long long)avi_audio_samples);

								avi_file_start_monotime -= pad;
								avi_file_start_time -= pad;
								avi_audio_err += (unsigned int)(pad * audio_rate);
							}

							// write audio
                            if (async_io) {
                                AVIPacket *p = new AVIPacket();
                                p->stream = AVI_STREAM_AUDIO;
                                p->set_data(fmp4_temp,b);
                                p->set_flags(riff_idx1_AVIOLDINDEX_flags_KEYFRAME);
                                async_avi_queue_add(&p);
                            }
                            else {
                                avi_writer_stream_write(AVI, AVI_audio, fmp4_temp, b, riff_idx1_AVIOLDINDEX_flags_KEYFRAME);
                            }
							avi_audio_samples += (unsigned long)done;
						}
					}
				}
				else if (done < 0) {
                    double tt;

                    // time to sample
                    tt = monoNOW - avi_file_start_monotime;
                    if (tt < 0) tt = 0;

					if (done == -EPIPE) {
						fprintf(stderr,"ALSA buffer underrun at %.3f!\n",tt);
						if (AVI_logfile != NULL)
							fprintf(AVI_logfile,"ALSA buffer underrun at %.3f!\n",tt);

						if ((err = snd_pcm_prepare(alsa_pcm)) < 0) {
							fprintf(stderr,"Failed to prepare ALSA, %s\n",snd_strerror(err));
						}
					}
					else if (done == -EAGAIN) {
						// yeah, this is to be expected
					}
					else {
						fprintf(stderr,"ALSA readi error %s\n",snd_strerror(done));
					}
				}
			}
		}

		f_sleep_until(sleep_until);
		sleep_until += 1.0 / 240;
	}

	if (comm_socket_fd >= 0) {
		unlink(comm_socket.c_str());
		close(comm_socket_fd);
	}

	close_v4l();

	printf("Closing AVI file... hold on\n");

	if (live_shm != NULL) {
		munmap(live_shm,live_shm_size);
		live_shm = NULL;
	}
	live_shm_size = 0;
	if (live_shm_fd >= 0) {
		shm_unlink(live_shm_name.c_str());
		close(live_shm_fd);
		live_shm_fd = -1;
	}
	close_avi_file();

	while (close_threads.size() > 0) {
		fprintf(stderr,"Waiting for thread %u\n",close_threads.front());
		if (pthread_join(close_threads.front(),NULL) != 0)
			fprintf(stderr,"pthread_join failed\n");
		close_threads.pop_front();
	}

	return 0;
}

