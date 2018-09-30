
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
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <sched.h>

#include <pthread.h> /* portable thread library -- we're multi-threaded now! */

#include <alsa/asoundlib.h>	/* ALSA sound library */

#include <linux/videodev2.h>	/* Linux Video4Linux 2 capture interface */

#include "now.h"
#include "live_feed.h"
#include "avi_writer.h"

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavcodec/avcodec.h>
}

/* comment out this define if your AVI files come out corrupt when stopping capture.
 * the intent is that AVI closure carry out AVI index writing away from the main
 * thread so this program is free to feed live video and capture more */
#define ASYNC_AVI_CLOSURE

#define AVI_FRAMES_PER_GROUP		15
#define AVI_B_FRAMES			0

using namespace std;

#include <list>
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

/* FIXME: Red Hat 4.1.2 Video4Linux doesn't have V4L2_FIELD_INTERLACED_BT? */
#ifndef V4L2_FIELD_INTERLACED_BT
#define V4L2_FIELD_INTERLACED_BT 0xDEADBEEF /* define it as a constant that doesn't exist */
#endif

#ifndef V4L2_FIELD_INTERLACED_TB
#define V4L2_FIELD_INTERLACED_TB 0xDEADBEED /* define it as a constant that doesn't exist */
#endif

static unsigned int		audio_sum_count = 0;
static unsigned int		audio_sum_level[4] = {0,0,0,0};
static unsigned int		audio_max_level[4] = {0,0,0,0};

static struct v4l2_capability	v4l_vbi_cap;
static struct v4l2_format	v4l_vbi_capfmt;

static unsigned char*		vbi_buffer = NULL;		/* [vbi_width * vbi_height] */
static unsigned int		vbi_read_field = 0;

static bool			swap_fields = false;

static bool             async_io = false;

static double			avi_audio_err = 0;
static snd_pcm_hw_params_t*	alsa_hw_params = NULL;
static snd_pcm_t*		alsa_pcm = NULL;

static volatile int		DIE = 0;

static FILE*			AVI_logfile = NULL;

static int			vbi_fd = -1;
static int			vbi_width,vbi_height,vbi_stride;
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
static string			v4l_codec_sel,want_codec_sel;		// h264, h264-422
static bool			v4l_crop_bounds = false;
static bool			v4l_crop_defrect = false;
static int			v4l_open_vbi_first = -1;		// -1 = auto  0 = no  1 = yes
static int			v4l_crop_vcr_hack = 0;
static int			v4l_crop_x = CROP_DEFAULT;
static int			v4l_crop_y = CROP_DEFAULT;
static int			v4l_crop_w = CROP_DEFAULT;
static int			v4l_crop_h = CROP_DEFAULT;
static int			v4l_width = 720,v4l_height = 480;
static double       want_fps = -1;
static int			want_width = -1,want_height = -1;
static int			v4l_framerate_n = 30000,v4l_framerate_d = 1001;
static int			v4l_interlaced = 0;
static int			v4l_interlaced_woven = 0;
static int			v4l_framesize = 0;
static int			v4l_stride = 720;
static int			v4l_width_stride = 720;
static int			v4l_buffers = 0;
static int			v4l_bufptr = 0;
static char			v4l_devname[64];
static int			v4l_fd = -1;
static struct v4l2_buffer	v4l_buf[30];
static unsigned char*		v4l_ptr[30]={NULL};
static double			v4l_basetime = -1,v4l_baseclock = 0;

static struct v4l2_capability	v4l_caps;
static struct v4l2_format	v4l_fmt;

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
static AVCodec*			fmp4_codec=NULL;
static AVCodecContext*		fmp4_context=NULL;

static AVCodec*			fmp4_vbi_codec=NULL;
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
		av_free(fmp4_context);
		fmp4_context = NULL;
	}
}

static void open_avi_file() {
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

    v4l_last_frame = 0;
    v4l_last_vbi = 0;
    v4l_last_frame_delta = 0;
    v4l_last_vbi_delta = 0;

	avcodec_register_all();
	if (fmp4_codec == NULL) {
		if ((fmp4_codec = avcodec_find_encoder(AV_CODEC_ID_H264)) == NULL) {
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
	avcodec_get_context_defaults3(fmp4_context,fmp4_codec);

	if (v4l_codec_yshr == 0)
		fmp4_context->bit_rate = 12000000;
	else
		fmp4_context->bit_rate = 8000000;

	fmp4_context->keyint_min = AVI_FRAMES_PER_GROUP;
	fmp4_context->time_base.num = v4l_framerate_d; /* NTS: ffmpeg means time interval as "amount of time between frames" */
	fmp4_context->time_base.den = v4l_framerate_n;
	fmp4_context->height = v4l_height;
	fmp4_context->width = v4l_width;
	fmp4_context->gop_size = AVI_FRAMES_PER_GROUP;
	fmp4_context->max_b_frames = 0; /* don't use B-frames */

	if (v4l_codec_yshr == 0)
		fmp4_context->pix_fmt = AV_PIX_FMT_YUV422P;
	else
		fmp4_context->pix_fmt = AV_PIX_FMT_YUV420P;

	fmp4_context->bit_rate_tolerance = fmp4_context->bit_rate / 2;
	fmp4_context->noise_reduction = 0;
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
	fmp4_context->rc_max_rate = fmp4_context->bit_rate;
	fmp4_context->rc_min_rate = fmp4_context->bit_rate;
//	fmp4_context->pre_dia_size = 3;
	fmp4_context->rc_buffer_size = fmp4_context->bit_rate * 2;
	fmp4_context->sample_aspect_ratio.num = 1;
	fmp4_context->sample_aspect_ratio.den = 1;
	fmp4_context->thread_count = how_many_cpus();
	fmp4_context->thread_type = 0;//FF_THREAD_SLICE;
	fmp4_context->thread_type |= FF_THREAD_FRAME;
//	fmp4_context->flags2 |= CODEC_FLAG2_FAST;
//	fmp4_context->flags |= CODEC_FLAG_LOW_DELAY;
	if (v4l_interlaced >= 0) fmp4_context->flags |= CODEC_FLAG_INTERLACED_DCT;

	fprintf(stderr,"Encoder using %u threads\n",how_many_cpus());

	av_dict_set(&opt_dict,"rc-lookahead","10",0); /* NTS: If latency is a problem, reduce this value */

	if (v4l_codec_yshr == 0)
		av_dict_set(&opt_dict,"profile","high422",0); /* or else it won't work */
	else
		av_dict_set(&opt_dict,"profile","main",0);

	av_dict_set(&opt_dict,"preset","veryfast",0);
	av_dict_set(&opt_dict,"vbr","1",0);

	if (avcodec_open2(fmp4_context,fmp4_codec,&opt_dict)) {
		fprintf(stderr,"Cannot open codec\n");
		av_dict_free(&opt_dict);
		close_avi_file();
		return;
	}

	av_dict_free(&opt_dict);
	update_now_time();
	avi_audio_samples = 0;
	avi_frame_counter = 0;
	avi_vbi_frame_counter = 0;
	avi_file_start_time = NOW;
	sprintf(tmp,"/%s%12.3f.avi",file_prefix,NOW);
	avi_file = capture_path + tmp;

	if (capture_vbi && vbi_fd >= 0) {
		if (fmp4_vbi_codec == NULL) {
			if ((fmp4_vbi_codec = avcodec_find_encoder(AV_CODEC_ID_H264)) == NULL) {
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
		avcodec_get_context_defaults3(fmp4_vbi_context,fmp4_vbi_codec);

		fmp4_vbi_context->bit_rate = 6000000;
		fmp4_vbi_context->keyint_min = AVI_FRAMES_PER_GROUP;
		fmp4_vbi_context->time_base.num = v4l_framerate_d; /* NTS: ffmpeg means time interval as "amount of time between frames" */
		fmp4_vbi_context->time_base.den = v4l_framerate_n;
		fmp4_vbi_context->height = vbi_height;
		fmp4_vbi_context->width = vbi_width;
		fmp4_vbi_context->gop_size = AVI_FRAMES_PER_GROUP;
		fmp4_vbi_context->max_b_frames = 0; /* don't use B-frames */
		fmp4_vbi_context->pix_fmt = AV_PIX_FMT_YUV420P;
		fmp4_vbi_context->bit_rate_tolerance = fmp4_vbi_context->bit_rate / 2;
		fmp4_vbi_context->noise_reduction = 0;
		fmp4_vbi_context->spatial_cplx_masking = 0.0;
		fmp4_vbi_context->temporal_cplx_masking = 0.0;
		fmp4_vbi_context->p_masking = 0.0;
		fmp4_vbi_context->nsse_weight = 0;
		fmp4_vbi_context->flags = 0;
		fmp4_vbi_context->flags2 = 0;
		fmp4_vbi_context->qmin = 1;
		fmp4_vbi_context->qmax = 63;
		fmp4_vbi_context->rc_max_rate = fmp4_vbi_context->bit_rate;
		fmp4_vbi_context->rc_min_rate = fmp4_vbi_context->bit_rate;
		fmp4_vbi_context->rc_buffer_size = fmp4_vbi_context->bit_rate * 2;
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
	AVI_video_fmt.biCompression = avi_fourcc_const('H','2','6','4'); /* FFMPEG MPEG-4 */
	AVI_video_fmt.biSizeImage = v4l_width * v4l_height * 3;
	AVI_video_fmt.biXPelsPerMeter = 0;
	AVI_video_fmt.biYPelsPerMeter = 0;
	AVI_video_fmt.biClrUsed = 0;
	AVI_video_fmt.biClrImportant = 0;

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

	if (!avi_writer_stream_set_format(AVI_video,&AVI_video_fmt,sizeof(windows_BITMAPINFOHEADER))) {
		fprintf(stderr,"Unable to set audio stream format\n");
		close_avi_file();
		return;
	}
	if ((strhdr = avi_writer_stream_header(AVI_video)) == NULL) {
		fprintf(stderr,"Unable to set audio stream header\n");
		close_avi_file();
		return;
	}
	strhdr->fccType =			avi_fccType_video;
	strhdr->fccHandler =			avi_fourcc_const('H','2','6','4');
	strhdr->dwFlags =			0;
	strhdr->dwScale =			v4l_framerate_d;
	strhdr->dwRate =			v4l_framerate_n;
	strhdr->dwSuggestedBufferSize =		v4l_width * v4l_height * 2;
	strhdr->dwSampleSize =			0;
	strhdr->rcFrame.right =			v4l_width;
	strhdr->rcFrame.bottom =		v4l_height;

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
		AVI_vbi_video_fmt.biHeight = vbi_height;
		AVI_vbi_video_fmt.biPlanes = 1;
		AVI_vbi_video_fmt.biBitCount = 24;
		AVI_vbi_video_fmt.biCompression = avi_fourcc_const('H','2','6','4'); /* FFMPEG MPEG-4 */
		AVI_vbi_video_fmt.biSizeImage = vbi_width * vbi_height * 3;
		AVI_vbi_video_fmt.biXPelsPerMeter = 0;
		AVI_vbi_video_fmt.biYPelsPerMeter = 0;
		AVI_vbi_video_fmt.biClrUsed = 0;
		AVI_vbi_video_fmt.biClrImportant = 0;

		if (!avi_writer_stream_set_format(AVI_vbi_video,&AVI_vbi_video_fmt,sizeof(windows_BITMAPINFOHEADER))) {
			fprintf(stderr,"Unable to set audio stream format\n");
			close_avi_file();
			return;
		}
		if ((strhdr = avi_writer_stream_header(AVI_vbi_video)) == NULL) {
			fprintf(stderr,"Unable to set audio stream header\n");
			close_avi_file();
			return;
		}
		strhdr->fccType =			avi_fccType_video;
		strhdr->fccHandler =			avi_fourcc_const('H','2','6','4');
		strhdr->dwFlags =			0;
		strhdr->dwScale =			v4l_framerate_d;
		strhdr->dwRate =			v4l_framerate_n;
		strhdr->dwSuggestedBufferSize =		vbi_width * vbi_height * 2;
		strhdr->dwSampleSize =			0;
		strhdr->rcFrame.right =			vbi_width;
		strhdr->rcFrame.bottom =		vbi_height;
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

			sprintf(tmp,"w=%u h=%u stride=%u size=%u pixfmt='%s' field='%s' async=%u",
					(unsigned int)v4l_fmt.fmt.pix.width,
					(unsigned int)v4l_fmt.fmt.pix.height,
					(unsigned int)v4l_fmt.fmt.pix.bytesperline,
					(unsigned int)v4l_fmt.fmt.pix.sizeimage,
					px,field_str,
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
	fprintf(stderr,"    -audio-device <device>                 ALSA device to capture from or 'auto'\n");
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
	fprintf(stderr,"    -codec <n>                             Codec.\n");
	fprintf(stderr,"                                                 h264      H.264/AVC 4:2:0\n");
	fprintf(stderr,"                                                 h264-422  H.264/AVC 4:2:2\n");
	fprintf(stderr,"    -vbi                                   Also capture & record VBI\n");
	fprintf(stderr,"    -vbi-all                               Capture & record ALL VBI data\n");
	fprintf(stderr,"    -input-device <x>                      Card input to select\n");
	fprintf(stderr,"    -input-standard <x>                    Video standard to select\n");
	fprintf(stderr,"    -vcr-hack <x>                          Crop x lines from bottom\n");
	fprintf(stderr,"    -vbi-first <x>                         Open VBI first, then init video (1=yes 0=no -1=auto)\n");
    fprintf(stderr,"    -no-vbi-sysfs                          Don't use sysfs to find vbi device\n");
    fprintf(stderr,"    -fps <n>                               Capture fps\n");
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
                want_fps = atoi(argv[i++]);
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

            if ((dir=opendir(path)) != NULL) {
                while ((d=readdir(dir)) != NULL) {
                    if (fstatat(dirfd(dir),d->d_name,&st,0)) /* if we can't stat it, we can't use it */
                        continue;

                    /* we're looking for a directory named vbiX where X is an integer */
                    if (!strncmp(d->d_name,"vbi",3) && isdigit(d->d_name[3])) {
                        fprintf(stderr,"Found VBI device using sysfs. video%u -> %s\n",v4l_index,d->d_name);
                        sprintf(tmp,"/dev/%s",d->d_name);
                        break;
                    }
                }
                closedir(dir);
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
				vbi_read_field = 0;
			}
		}
	}
}

void close_v4l() {
	int i,x;

	close_avi_file();

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
				if (v4l_interlaced >= 0)
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

		if (want_codec_sel == "h264-422" && (v4l_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV || v4l_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_UYVY)) {
			v4l_codec_sel = "h264-422";
			v4l_codec_yshr = 0;
		}
		else {
			v4l_codec_sel = "h264";
			v4l_codec_yshr = 1;
		}

		fprintf(stderr,"new size=%u bpline=%u recording-as=%s\n",
				v4l_fmt.fmt.pix.sizeimage,
				v4l_fmt.fmt.pix.bytesperline,
				v4l_codec_sel.c_str());

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
				fprintf(stderr,"Unable to get parameters, %s\n",strerror(errno));
				goto fail;
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
				fprintf(stderr,"Unable to get parameters, %s\n",strerror(errno));
				goto fail;
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

		if (audio_device == "auto")
			name = "default";
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
		xx->width = v4l_width;
		xx->height = v4l_height;
		xx->stride = v4l_width_stride;
		xx->this_generation = ++live_shm_gen;
		xx->audio_channels = audio_channels;
		xx->audio_rate = audio_rate;
		xx->frame_size = (v4l_width_stride * v4l_height) + ((v4l_width_stride>>1) * (v4l_height>>v4l_codec_yshr) * 2);
		xx->in = 0;
		xx->slots = (live_shm_size - sizeof(*xx)) / xx->frame_size;
		xx->header = LIVE_SHM_HEADER;

		if (v4l_codec_yshr == 0)
			xx->color_fmt = LIVE_COLOR_FMT_YUV422;	
		else
			xx->color_fmt = LIVE_COLOR_FMT_YUV420;
	}

	if (auto_v4l_avi && v4l_fd >= 0) open_avi_file();
	return 0;
fail:
	close_v4l();
	return 1;
}

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
		for (fc=0,field=(v4l_interlaced == 0)?1:0;fc < 2;fc++,field ^= 1) {
			unsigned char *srf;
			if (v4l_interlaced_woven)
				srf = (unsigned char*)sdi + (fc * srcs);
			else
				srf = (unsigned char*)sdi + (fc * hh * srcs);
			/* 4:2:2 SDI -> 4:2:2 progressive scan */
			for (y=field;y < render_height;y += 2) {
				src = srf; srf += srcs*2;
				Y = framep[0] + ( (y+0)       * stride[0]);
				U = framep[1] + ( (y+0)       * stride[1]);
				V = framep[2] + ( (y+0)       * stride[2]);
				for (x=0;x < (unsigned int)v4l_width;x += 2) {
					/* Y U Y V */
					Y[0] = src[0];
					Y[1] = src[2];
					*U++ = src[1];
					*V++ = src[3];
					src += 4;
					Y += 2;
				}
			}
		}
	}
	else {
		/* 4:2:2 SDI -> 4:2:2 progressive scan */
		for (y=0;y < render_height;y++) {
			Y = framep[0] + ( (y+0)       * stride[0]);
			U = framep[1] + ( (y+0)       * stride[1]);
			V = framep[2] + ( (y+0)       * stride[2]);
			src = (unsigned char*)sdi + ((y+0) * srcs);
			for (x=0;x < (unsigned int)v4l_width;x += 2) {
				/* Y U Y V */
				Y[0] = src[0];
				Y[1] = src[2];
				*U++ = src[1];
				*V++ = src[3];
				src += 4;
				Y += 2;
			}
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
		for (fc=0,field=(v4l_interlaced == 0)?1:0;fc < 2;fc++,field ^= 1) {
			unsigned char *srf;
			if (v4l_interlaced_woven)
				srf = (unsigned char*)sdi + (fc * srcs);
			else
				srf = (unsigned char*)sdi + (fc * hh * srcs);

			/* 4:2:2 SDI -> 4:2:0 progressive scan */
			for (y=field;y < render_height;y += 2) {
				src = srf; srf += srcs;
				Y = framep[0] + ( (y+0)       * stride[0]);
				U = framep[1] + ( (y+0)       * stride[1]);
				V = framep[2] + ( (y+0)       * stride[2]);
				for (x=0;x < (unsigned int)v4l_width;x += 2) {
					/* U Y V Y */
					Y[0] = src[1];
					Y[1] = src[3];
					*U++ = src[0];
					*V++ = src[2];
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
			U = framep[1] + ( (y+0)       * stride[1]);
			V = framep[2] + ( (y+0)       * stride[2]);
			src = (unsigned char*)sdi + ((y+0) * srcs);
			for (x=0;x < (unsigned int)v4l_width;x += 2) {
				/* U Y V Y */
				Y[0] = src[1];
				Y[1] = src[3];
				*U++ = src[0];
				*V++ = src[2];
				src += 4;
				Y += 2;
			}
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
		for (fc=0,field=(v4l_interlaced == 0)?1:0;fc < 2;fc++,field ^= 1) {
			unsigned char *srf;
			if (v4l_interlaced_woven)
				srf = (unsigned char*)sdi + (fc * srcs);
			else
				srf = (unsigned char*)sdi + (fc * hh * srcs);
			/* 4:2:2 SDI -> 4:2:0 progressive scan */
			for (y=field;y < render_height;y += 4) {
				src = srf; srf += srcs*2;
				Y = framep[0] + ( (y+0)       * stride[0]);
				U = framep[1] + (((y+1) >> 1) * stride[1]);
				V = framep[2] + (((y+1) >> 1) * stride[2]);
				for (x=0;x < (unsigned int)v4l_width;x += 2) {
					/* Y U Y V */
					Y[0] = src[0];
					Y[1] = src[2];
					*U++ = src[1];
					*V++ = src[3];
					src += 4;
					Y += 2;
				}

				src = srf; srf += srcs*2;
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
			for (x=0;x < (unsigned int)v4l_width;x += 2) {
				/* Y U Y V */
				Y[0] = src[0];
				Y[1] = src[2];
				*U++ = src[1];
				*V++ = src[3];
				src += 4;
				Y += 2;
			}

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
		for (fc=0,field=(v4l_interlaced == 0)?1:0;fc < 2;fc++,field ^= 1) {
			unsigned char *srf;
			if (v4l_interlaced_woven)
				srf = (unsigned char*)sdi + (fc * srcs);
			else
				srf = (unsigned char*)sdi + (fc * hh * srcs);

			/* 4:2:2 SDI -> 4:2:0 progressive scan */
			for (y=field;y < render_height;y += 4) {
				src = srf; srf += srcs;
				Y = framep[0] + ( (y+0)       * stride[0]);
				U = framep[1] + (((y+1) >> 1) * stride[1]);
				V = framep[2] + (((y+1) >> 1) * stride[2]);
				for (x=0;x < (unsigned int)v4l_width;x += 2) {
					/* U Y V Y */
					Y[0] = src[1];
					Y[1] = src[3];
					*U++ = src[0];
					*V++ = src[2];
					src += 4;
					Y += 2;
				}

				src = srf; srf += srcs;
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
			for (x=0;x < (unsigned int)v4l_width;x += 2) {
				/* U Y V Y */
				Y[0] = src[1];
				Y[1] = src[3];
				*U++ = src[0];
				*V++ = src[2];
				src += 4;
				Y += 2;
			}

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

	if (!strcmp(msg,"capture-on")) {
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
		if (AVI != NULL) {
			/* TODO: Announce the audio format when the GUI supports audio playback */
			sprintf(msg,
				"OK\n"
				"File:%s\n"
				"VidCodec:MPEG4\n"
				"VidSize:%ux%u\n"
				"VidRate:%u/%u\n"
				"StartTime:%.3f\n",
				avi_file.c_str()/*FILE*/,
				v4l_width,v4l_height/*VidSize*/,
				v4l_framerate_n,v4l_framerate_d/*VidRate*/,
				avi_file_start_time/*StartTime*/);
			socket_MSG(msg);
		}
		else {
			fprintf(stderr,"Failed to start recording.\n");
			socket_MSG("FAILED");
		}
	}
	else if (!strcmp(msg,"record-off")) {
		auto_v4l_avi = false;
		close_avi_file();
	}
	else {
		fprintf(stderr,"Unknown command '%s'\n",msg);
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

			if (ioctl(v4l_fd,VIDIOC_QUERYBUF,&v4l_buf[v4l_bufptr]) == 0) {
				if (v4l_buf[v4l_bufptr].flags & V4L2_BUF_FLAG_DONE) {
					if (ioctl(v4l_fd,VIDIOC_DQBUF,&v4l_buf[v4l_bufptr]) == 0) {
						vb = &v4l_buf[v4l_bufptr];
						ptr = v4l_ptr[v4l_bufptr];
						if (vb->timestamp.tv_sec > 0) {
							/* NOTE: The Linux kernel API says the timestamp can come from the device,
							 *       or from other parts of the Video4Linux API. It never said anything
							 *       about the time matching the system clock, it could be anything.
							 *       So we have to subtract and add to translate the clock value. */
							framt = (double)vb->timestamp.tv_sec +
								(((double)vb->timestamp.tv_usec) / 1000000);

							if (v4l_basetime < 0) {
								v4l_basetime = framt;
								v4l_baseclock = NOW;
							}

							framt -= v4l_basetime;
							if (framt < 0) framt = 0;
							framt += v4l_baseclock;
						}

						v4l_interlaced = 0;
						v4l_interlaced_woven = 1;
						if (vb->field == V4L2_FIELD_NONE)
							v4l_interlaced = -1;
						else if (vb->field == V4L2_FIELD_INTERLACED || vb->field == V4L2_FIELD_INTERLACED_BT)
							v4l_interlaced = 1;
						else if (vb->field == V4L2_FIELD_INTERLACED_TB)
							v4l_interlaced = 0;
						else if (vb->field == V4L2_FIELD_SEQ_TB)
							{ v4l_interlaced = 1; v4l_interlaced_woven = 0; }
						else if (vb->field == V4L2_FIELD_SEQ_BT)
							{ v4l_interlaced = 1; v4l_interlaced_woven = 1; }
						else
							v4l_interlaced = -1;
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
				AVFrame av_frame;

				memset(&av_frame,0,sizeof(av_frame));
				av_frame.top_field_first = 1;//(v4l_interlaced == 0); FIXME!
				av_frame.interlaced_frame = (v4l_interlaced >= 0);
				av_frame.key_frame = (avi_frame_counter % AVI_FRAMES_PER_GROUP) == 0;
				av_frame.pts = AV_NOPTS_VALUE;
                av_frame.width = v4l_width;
                av_frame.height = v4l_height;

				if (v4l_fd >= 0 && live_shm != NULL) {
					volatile struct live_shm_header *xx = live_shm_head();
					assert(xx->header == LIVE_SHM_HEADER);
					assert(xx->width != 0);
					assert(xx->height != 0);
					assert(xx->stride != 0);
					assert(xx->frame_size != 0);
					assert(xx->slots != 0);
					assert(xx->in >= 0 && xx->in < xx->slots);

					/* direct conversion to the shared memory segment, convert only once */
					av_frame.data[0] = live_shm + sizeof(struct live_shm_header) +
						(xx->frame_size * xx->in);
					av_frame.linesize[0] = v4l_width_stride;
					av_frame.data[1] = av_frame.data[0] + (v4l_width_stride * v4l_height);
					av_frame.linesize[1] = v4l_width_stride >> 1;
					av_frame.data[2] = av_frame.data[1] + ((v4l_width_stride/2) * (v4l_height>>v4l_codec_yshr));
					av_frame.linesize[2] = v4l_width_stride >> 1;
					assert((av_frame.data[2] + (av_frame.linesize[2]*(v4l_height>>v4l_codec_yshr))) <= (live_shm + live_shm_size));

					if (v4l_codec_yshr == 0) {
						if (v4l_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV)
							copy_yuyv_to_planar_yuv_422(av_frame.data,av_frame.linesize,ptr);
						else if (v4l_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_UYVY)
							copy_uyvy_to_planar_yuv_422(av_frame.data,av_frame.linesize,ptr);
					}
					else if (v4l_codec_yshr == 1) {
						if (v4l_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV)
							copy_yuyv_to_planar_yuv_420(av_frame.data,av_frame.linesize,ptr);
						else if (v4l_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_UYVY)
							copy_uyvy_to_planar_yuv_420(av_frame.data,av_frame.linesize,ptr);
					}

					unsigned int ch=0;
					for (;ch < audio_channels;ch++) {
						xx->map[xx->in].audio_max_level[ch] = audio_max_level[ch];
						xx->map[xx->in].audio_avg_level[ch] = (uint32_t)audio_sum_level[ch] / (audio_sum_count != 0 ? audio_sum_count : 1);
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
						xx->map[xx->in].audio_avg_level[ch] = 0;
					}
					xx->map[xx->in].offset = (uint32_t)((size_t)av_frame.data[0] - (size_t)live_shm);
					xx->map[xx->in].generation = xx->this_generation;
					xx->map[xx->in].field_order = v4l_interlaced + 1;
					if ((++xx->in) == xx->slots) {
						xx->this_generation++;
						xx->in = 0;
					}
				}
				else {
					av_frame.data[0] = fmp4_yuv;
					av_frame.linesize[0] = v4l_width_stride;
					av_frame.data[1] = av_frame.data[0] + (v4l_width_stride * v4l_height);
					av_frame.linesize[1] = v4l_width_stride >> 1;
					av_frame.data[2] = av_frame.data[1] + ((v4l_width_stride/2) * (v4l_height>>v4l_codec_yshr));
					av_frame.linesize[2] = v4l_width_stride >> 1;

					if (v4l_codec_yshr == 0) {
						if (v4l_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV)
							copy_yuyv_to_planar_yuv_422(av_frame.data,av_frame.linesize,ptr);
						else if (v4l_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_UYVY)
							copy_uyvy_to_planar_yuv_422(av_frame.data,av_frame.linesize,ptr);
					}
					else if (v4l_codec_yshr == 1) {
						if (v4l_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV)
							copy_yuyv_to_planar_yuv_420(av_frame.data,av_frame.linesize,ptr);
						else if (v4l_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_UYVY)
							copy_uyvy_to_planar_yuv_420(av_frame.data,av_frame.linesize,ptr);
					}
				}

				if (AVI) {
					if (AVI_video) {
						AVPacket pkt;
						int gotit=0;

						if (framt > 0) {
							double t = ((framt - avi_file_start_time) * v4l_framerate_n) / v4l_framerate_d;
							if (t < -100) fprintf(stderr,"Warning: Frame time away behind AVI start time\n");
							if (t < 0) t = 0;
							unsigned long long n = (unsigned long long)floor(t+0.5);

							if (avi_frame_counter < n) {
								int patience = 1000;
								while (patience-- > 0 && avi_frame_counter < n) {
									avi_frame_counter++;
								}
							}
						}

                        v4l_last_frame_delta = avi_frame_counter - v4l_last_frame;
                        v4l_last_frame = avi_frame_counter;

						/* encode to MPEG-4 and store */
						memset(&pkt,0,sizeof(pkt));
						pkt.data = fmp4_temp;
						pkt.size = sizeof(fmp4_temp);
						pkt.dts = avi_frame_counter;
						pkt.pts = avi_frame_counter;
						av_frame.pts = avi_frame_counter;
						rd = avcodec_encode_video2(fmp4_context,&pkt,&av_frame,&gotit);
						if (rd < 0) {
							printf("Unable to encode frame rd=%d\n",rd);
						}
						else if (gotit == 0) {
							printf("Hm? No output on %llu? rd=%d gotit=%u\n",
								(unsigned long long)avi_frame_counter,rd,gotit);
						}
						else {
							rd = pkt.size;
							if (rd > 0) {
								int patience = 1000;

                                if (!async_io) {
                                    while (AVI_video->sample_write_chunk < pkt.dts && patience-- > 0)
                                        avi_writer_stream_write(AVI,AVI_video,NULL,0,0);
                                }

                                if (async_io) {
                                    AVIPacket *p = new AVIPacket();
                                    p->stream = AVI_STREAM_VIDEO;
                                    p->set_data(fmp4_temp,rd);
                                    p->set_flags((pkt.flags&AV_PKT_FLAG_KEY) ? riff_idx1_AVIOLDINDEX_flags_KEYFRAME : 0);
                                    p->target_chunk = pkt.dts;
                                    async_avi_queue_add(&p);
                                }
                                else {
    								if (avi_writer_stream_write(AVI,AVI_video,fmp4_temp,rd,
	    								(pkt.flags&AV_PKT_FLAG_KEY) ? riff_idx1_AVIOLDINDEX_flags_KEYFRAME : 0)) {
		    							avi_writer_stream_index *si = AVI_video->sample_index + AVI_video->sample_write_chunk - 1;
			    						socket_index_msg("V",pkt.dts,si->offset,si->length,(pkt.flags&AV_PKT_FLAG_KEY)?1:0);
				    				}
                                }
							}
							else {
								printf("Hm? No output on %llu\n",avi_frame_counter);
							}
						}

						avi_frame_counter++;
					}
				}

                if (async_io && AVI != NULL && AVI_video != NULL) {
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

				vb->timestamp.tv_sec = 0;
				vb->timestamp.tv_usec = 0;
				if (ioctl(v4l_fd,VIDIOC_QBUF,&v4l_buf[v4l_bufptr]) == 0) {
					if (++v4l_bufptr >= v4l_buffers)
						v4l_bufptr = 0;
				}
			}
		}

		/* VBI */
		if (vbi_fd >= 0 && vbi_buffer != NULL) {
			double framt = NOW;
			AVFrame av_frame;
			int rd,expect;

			memset(&av_frame,0,sizeof(av_frame));
			av_frame.key_frame = (avi_vbi_frame_counter % AVI_FRAMES_PER_GROUP) == 0;
			av_frame.pts = AV_NOPTS_VALUE;
			av_frame.width = vbi_width;
			av_frame.height = v4l_vbi_capfmt.fmt.vbi.count[0] + v4l_vbi_capfmt.fmt.vbi.count[1];

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

					vbi_read_field ^= 1;
					if (!vbi_read_field && AVI && fmp4_vbi_context != NULL) {
						/* direct conversion to the shared memory segment, convert only once */
						av_frame.data[0] = vbi_buffer;
						av_frame.linesize[0] = vbi_width;

						assert((vbi_width*vbi_height) <= sizeof(fmp4_yuv));
						memset(fmp4_yuv,128,vbi_width*vbi_height);

						av_frame.data[1] = fmp4_yuv;
						av_frame.linesize[1] = vbi_width >> 1;
						av_frame.data[2] = fmp4_yuv;
						av_frame.linesize[2] = vbi_width >> 1;

						AVPacket pkt;
						int gotit=0;

						if (framt > 0) {
							double t = ((framt - avi_file_start_time) * v4l_framerate_n) / v4l_framerate_d;
							if (t < -100) fprintf(stderr,"Warning: Frame time away behind AVI start time\n");
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
						memset(&pkt,0,sizeof(pkt));
						pkt.data = fmp4_temp;
						pkt.size = sizeof(fmp4_temp);
						pkt.dts = avi_vbi_frame_counter;
						pkt.pts = avi_vbi_frame_counter;
						av_frame.pts = avi_vbi_frame_counter;
						rd = avcodec_encode_video2(fmp4_vbi_context,&pkt,&av_frame,&gotit);
						if (rd < 0) {
							printf("Unable to encode frame rd=%d\n",rd);
						}
						else if (gotit == 0) {
							printf("Hm? No output on %llu? rd=%d gotit=%u\n",
								(unsigned long long)avi_vbi_frame_counter,rd,gotit);
						}
						else {
							rd = pkt.size;
							if (rd > 0) {
								int patience = 1000;

                                if (!async_io) {
                                    while (AVI_vbi_video->sample_write_chunk < pkt.dts && patience-- > 0)
                                        avi_writer_stream_write(AVI,AVI_vbi_video,NULL,0,0);
                                }

                                if (async_io) {
                                    AVIPacket *p = new AVIPacket();
                                    p->stream = AVI_STREAM_VBI;
                                    p->set_data(fmp4_temp,rd);
                                    p->set_flags((pkt.flags&AV_PKT_FLAG_KEY) ? riff_idx1_AVIOLDINDEX_flags_KEYFRAME : 0);
                                    p->target_chunk = pkt.dts;
                                    async_avi_queue_add(&p);
                                }
                                else {
    								avi_writer_stream_write(AVI,AVI_vbi_video,fmp4_temp,rd,
	    								(pkt.flags&AV_PKT_FLAG_KEY) ? riff_idx1_AVIOLDINDEX_flags_KEYFRAME : 0);
                                }
							}
							else {
								printf("Hm? No output on %llu\n",avi_vbi_frame_counter);
							}
						}

						avi_vbi_frame_counter++;
					}
				}
				else if (rd == 0 || (rd < 0 && errno == EAGAIN)) {
				}
				else if (rd >= 0) {
					fprintf(stderr,"VBI unexpected read=%d expect=%d\n",rd,expect);
				}
				else {
					fprintf(stderr,"VBI read error %s\n",strerror(errno));
				}
			}
			else {
				vbi_read_field ^= 1;
			}
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

					// metering
					for (unsigned int samp=0;samp < (unsigned int)done;samp++) {
						int16_t *s = (int16_t*)fmp4_temp + (samp * audio_channels);
						for (unsigned int ch=0;ch < (unsigned int)audio_channels;ch++) {
							audio_sum_level[ch] += (unsigned int)abs(s[ch]);
							audio_max_level[ch] = std::max(audio_max_level[ch],(unsigned int)abs(s[ch]));
						}

                        audio_sum_count++;
                    }

					if (AVI) {
						if (AVI_audio) {
							// what time should we be at?
							unsigned long long samp_should = 0;
							double tt;

							// time to sample
							tt = NOW - avi_file_start_time;
							if (tt < 0) tt = 0;
							samp_should = (unsigned long long)(tt * audio_rate);

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
								avi_audio_err *= 0.95;
								avi_audio_err += ((double)samp_should - (double)avi_audio_samples) * (1.0 - 0.95);
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

								avi_file_start_time -= pad;
								avi_audio_err += pad;
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
                    tt = NOW - avi_file_start_time;
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

