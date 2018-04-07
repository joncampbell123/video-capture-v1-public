
#define __STDC_LIMIT_MACROS
#define __STDC_CONSTANT_MACROS

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/un.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>

#include <alsa/asoundlib.h>
#include <alsa/control.h>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xvlib.h>

#include <linux/videodev2.h>

#define CROP_DEFAULT (-9999)

#if GTK_CHECK_VERSION(2,22,0) /* FIXME: When exactly did GtkInfoBar appear in the API? */
# define GTK_HAS_INFO_BAR
#endif

#if !GTK_CHECK_VERSION(2,22,0) /* FIXME: When did the GDK change keycodes from GDK... to GDK_KEY...? */
# define GDK_KEY_Escape GDK_Escape
# define GDK_KEY_Right GDK_Right
# define GDK_KEY_Left GDK_Left
#endif

#ifndef UINT64_C
#define UINT64_C(x) (x##LL)
#endif

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavcodec/avcodec.h>
}

#include "avi_reader.h"
#include "avi_writer.h"
#include "live_feed.h"
#include "vgafont.h"
#include "now.h"

using namespace std;

#include <string>
#include <vector>
#include <list>
#include <map>

#define CONFIG_FILE_NAME	"videocap.ini"
#define V4L_CAP_PROGRAM		"capture_v4l"

/* DEBUG */
//#define METADATA_STOCK_DEBUG

enum {
	IP_PROTO_UDP=0,		/* 0 */
	IP_PROTO_RTP,
	IP_PROTO_MAX
};

const char *ip_proto_str[IP_PROTO_MAX] = {
	"udp",
	"rtp"
};

#if defined(METADATA_STOCK_DEBUG)
typedef struct metadata_list_entry {
	const char		*name;
	const char		*value;
} metadata_list_entry;

static const metadata_list_entry metadata_stock[] = {
	{"Pitch (dummy)",	"144.2"},
	{"Yaw (dummy)",		"221.1"},
	{"Compass (dummy)",	"36.2 N"},
	{"Altitude (dummy)",	"223 ft"},
	{NULL,			NULL}
};
#endif

/* view input mode */
enum {
	VIEW_INPUT_OFF=0,		/* no display */
	VIEW_INPUT_FILE,		/* show file (selected File -> Open) */
	VIEW_INPUT_1,			/* capture device 1 */
	VIEW_INPUT_2,			/* capture device 2 */
	VIEW_INPUT_3,			/* capture device 3 */
	VIEW_INPUT_4,			/* capture device 4 */
	VIEW_INPUT_IP,			/* IP input */
	VIEW_INPUT_MAX			/* -------------------- */
};

enum {
	OSD_OFF=0,
	OSD_SMALL,
	OSD_BIG
};

class video_tracking {
public:
	video_tracking() {
		clear();
	}
	void clear() {
		start_time = -1;
		video_rate_n = video_rate_d = 0;
		video_width = video_height = 0;
		video_current_frame = -1LL;
		video_first_frame = -1LL;
		video_total_frames = 0;
		video_index.clear();
	}
public:
	struct index_entry {
		uint64_t		offset;
		/* { this is consistent with how OpenDML compliant AVI files store chunk length and keyframe status */
		uint32_t		size:31; /* nor would any chunk ever exceed 2GB */
		uint32_t		keyframe:1;
		/* } */
	};
public:
	double				start_time;
	int				video_rate_n,video_rate_d;
	int				video_width,video_height;
	signed long long		video_current_frame;	/* NOTE: This is the last decoded frame or -1LL if none. The next decode step will SKIP this frame and step forward */
	signed long long		video_total_frames;
	signed long long		video_first_frame;
	vector<struct index_entry>	video_index;
};

struct uint_ratio_t {
	unsigned int			m,n;
};

struct uint_ratio_t user_speeds[] = {
	{1,8},	{1,4},	{1,2},	{1,1},
	{2,1},	{4,1},	{8,1},	{16,1}
};
#define user_speeds_total (sizeof(user_speeds) / sizeof(user_speeds[0]))

class InputManager {
public:
	InputManager(int input_index);
	~InputManager();
public:
	void onRecord(bool on);
	void onActivate(bool on);
	void start_recording();
	void stop_recording();
	void start_playback();
	void pause_playback();
	void stop_playback();
	void single_step(int frames=1);	/* frames > 0, step forward. frames < 0, step backwards */
	void socket_command(const char *msg);
	signed long long time_to_target_frame_clipped();
	signed long long time_to_target_frame();
	bool load_external_avi_for_play(const char *path);
	void ChangeSpeed(int n,unsigned int d);
	void increase_speed();
	void decrease_speed();
	bool start_process();
	bool reopen_input();
	void shutdown_process();
	void close_socket();
	void close_shmem();
	void close_avcodec();
	void close_capture_avi();
	void close_play_avi();
	void codec_check();
	bool idle_socket();
	void idle();
public:
	int				index;
	/* Pause/Play/Record state.
	 * Loosely modeled after your typical VHS type VCR.
	 *
	 * Play = Play the capture file for the input
	 * Pause = Display still image from capture file, do not advance automatically (like a VCR in Pause+Play mode)
	 * Stop = Play live video incoming from input (like a VCR tuned to a tv channel and not playing a tape) */
	bool				Paused;
	bool				Playing;
	bool				Recording;
	/* User overrides */
	int				user_ar_n,user_ar_d;
	int				source_ar_n,source_ar_d;

	/* capture process */
	int				cap_pid;			/* PID of the process */

	/* capture process, socket */
	string				socket_path;			/* socket path */
	int				socket_svr_fd;			/* socket (server) */
	int				socket_fd;			/* socket (connection from client) */

	/* capture process, shared memory segment */
	string				shmem_name;			/* shared memory name */
	size_t				shmem_size;			/* shared memory size */
	int				shmem_out;			/* shared memory "in" ptr */
	int				shmem_fd;
	void*				shmem;				/* shared memory ptr */
	int				shmem_slots;
	int				shmem_width,shmem_height;	/* width and height of tracking frame */

	/* capture params */
	int				video_index;
	std::string			audio_device;
	bool				enable_audio;
	bool				enable_vbi;
	std::string			input_device;
	std::string			input_standard;
	int				capture_width;
	int				capture_height;
	bool				crop_bounds;
	bool				crop_defrect;
	int				crop_left,crop_top,crop_width,crop_height;
    double              capture_fps;
	std::string			input_codec;
	std::string			vcrhack;

	/* video capture info */
	video_tracking			vt_play,vt_rec;			/* independent indexes for playback and recording */
	int				capture_avi_fd;
	int				play_avi_fd;
	bool				play_is_rec;			/* capture_avi == play_avi (optimize out string compare) */

	/* prefix added to file name */
	char				file_prefix[128];
	char				osd_name[32];
	char				cfg_name[32];

	/* the AVI file the capture program is writing */
	string				capture_avi;
	string				play_avi;

	/* temporary buffer for messages from client */
	char				sock_msg_tmp[8192];

	/* playback */
	unsigned long long		playback_base_frame;
	double				playback_base_time;
	/* playback speed control (as integer ratios) */
	int				playback_speed_n;
	unsigned int			playback_speed_d;

	/* codec */
	AVCodecContext*			video_avcodec_ctx;
	AVCodec*			video_avcodec;
};

void update_ui();
void switch_input(int x);
bool do_video_source(InputManager *input,bool force_redraw);

InputManager*			Input[VIEW_INPUT_MAX] = {NULL};

/* global variables */
GtkWidget*			x_dialog = NULL;
GtkWidget*			x_dialog_shm = NULL;
GtkWidget*			x_dialog_xvideo = NULL;
GtkWidget*			input_dialog = NULL;
GtkWidget*			input_dialog_device = NULL;
GtkWidget*			input_dialog_standard = NULL;
GtkWidget*			input_dialog_capres = NULL;
GtkWidget*			input_dialog_capfps = NULL;
GtkWidget*			input_dialog_codec = NULL;
GtkWidget*			input_dialog_vcrhack = NULL;
GtkWidget*			input_dialog_vbi_enable = NULL;
GtkWidget*			input_dialog_def_crop = NULL;
GtkWidget*			input_dialog_bounds_crop = NULL;
GtkWidget*			audio_dialog = NULL;
GtkWidget*			audio_dialog_device = NULL;
GtkWidget*			audio_dialog_enable = NULL;
GtkWidget*			ip_dialog = NULL;
GtkWidget*			ip_dialog_ip_addr = NULL;
GtkWidget*			ip_dialog_ip_port = NULL;
GtkWidget*			ip_dialog_protocol = NULL;

bool				user_enable_osd = true;		/* whether the user wants the OSD messages */

bool				use_x_shm = true;		/* use X shared memory segments */
bool				use_xvideo = true;		/* use XVideo extension */
int				xvideo_adapter = 0;		/* which adapter to use */

bool				xvideo_supported = false;
bool				x_shm_supported = false;

unsigned int			xvideo_version = 0;
unsigned int			xvideo_release = 0;
unsigned int			xvideo_request_base = 0;
unsigned int			xvideo_event_base = 0;
unsigned int			xvideo_error_base = 0;

GtkWidget*			client_area = NULL;
Display*			client_area_display = NULL;
XvImage*			client_area_xvimage = NULL;
XImage*				client_area_image = NULL;
GC				client_area_gc = 0;
long				client_area_xid = -1;
int				client_area_screen = -1;
int				client_area_xvideo_port = -1;
int				client_area_width = -1,client_area_height = -1;
int				client_area_aspect_n = 4,client_area_aspect_d = 3;
int				source_image_width = 720,source_image_height = 480;
XShmSegmentInfo			client_area_shminfo = {0};
bool				client_area_xvideo = 0;
bool				client_area_shm = 0;
int				client_area_depth = 0;
int				client_area_fmt_id = 0;
int				client_area_visual_id = 0;
int				client_area_y_plane_index,client_area_u_plane_index,client_area_v_plane_index;
XvImageFormatValues		client_area_xv_format;

void osd_next_state_cb_default();

/* on-screen display */
double				video_should_redraw_t = -1;
double				osd_next_state_t = -1;
int				osd_state = OSD_OFF;
void				(*osd_next_state_cb)() = osd_next_state_cb_default;
string				osd_string;

void osd_next_state_cb_default() {
	osd_next_state_t = -1;

	if (osd_state == OSD_BIG) {
		osd_next_state_t = NOW + 4.5;
		osd_state = OSD_SMALL;
		if (video_should_redraw_t < NOW)
			video_should_redraw_t = NOW + 0.25; /* if video is not moving, then force redraw the frame at this time, else the timeout will be discarded */
	}
	else if (osd_state == OSD_SMALL) {
		if (video_should_redraw_t < NOW)
			video_should_redraw_t = NOW + 0.25;
		osd_state = OSD_OFF;
	}
}

void osd_set_text_big(const char *msg) {
	osd_next_state_cb = osd_next_state_cb_default;
	osd_state = OSD_BIG;
	osd_next_state_t = NOW + 1.5;
	video_should_redraw_t = NOW + 0.25;
	osd_string = msg;
}

/* where in the client area to draw the actual video.
 * XVideo mode: the hardware overlay is scaled to fit this box
 * Non-Xvideo: the software YUV->RGB converter scales it's output to fit in this box */
int				client_area_video_x = 0,client_area_video_y = 0;
int				client_area_video_w = 1,client_area_video_h = 1;

GdkPixbuf*			application_icon_gdk_pixbuf = NULL;
GtkWidget*			messagelabel = NULL;
GtkWidget*			statusbar = NULL;
#ifdef GTK_HAS_INFO_BAR
GtkWidget*			infobar = NULL;
#endif
GtkWidget*			main_window = NULL;
GtkWidget*			main_window_status = NULL;
GtkWidget*			main_window_metadata = NULL;
GtkUIManager*			main_window_ui_mgr = NULL;
GtkWidget*			main_window_contents = NULL;

GtkWidget*			main_window_control_record = NULL;
GtkWidget*			main_window_toolbar_record = NULL;

GtkWidget*			main_window_control_pause = NULL;
GtkWidget*			main_window_toolbar_pause = NULL;

GtkWidget*			main_window_control_play = NULL;
GtkWidget*			main_window_toolbar_play = NULL;

GtkWidget*			main_window_control_stop = NULL;
GtkWidget*			main_window_toolbar_stop = NULL;

GtkRadioAction*			main_window_view_input_action = NULL;
GtkRadioAction*			main_window_toolbar_input_action = NULL;

GtkRadioAction*			main_window_view_aspect_action = NULL;

GtkWidget*			main_window_view_input_none = NULL;
GtkWidget*			main_window_toolbar_input_none = NULL;

GtkWidget*			main_window_view_input_file = NULL;
GtkWidget*			main_window_toolbar_input_file = NULL;

GtkWidget*			main_window_view_input_1 = NULL;
GtkWidget*			main_window_toolbar_input_1 = NULL;

GtkWidget*			main_window_view_input_2 = NULL;
GtkWidget*			main_window_toolbar_input_2 = NULL;

GtkWidget*			main_window_view_input_3 = NULL;
GtkWidget*			main_window_toolbar_input_3 = NULL;

GtkWidget*			main_window_view_input_4 = NULL;
GtkWidget*			main_window_toolbar_input_4 = NULL;

GtkWidget*			main_window_view_input_ip = NULL;
GtkWidget*			main_window_toolbar_input_ip = NULL;

GtkWidget*			main_window_view_osd = NULL;

GSList*				ui_notification_log = NULL;

/* secondary thread for video playback, and mutex to prevent conflicts */
pthread_t			secondary_thread_id = 0;
volatile int			secondary_thread_must_die = 0;
pthread_mutex_t			global_mutex = PTHREAD_MUTEX_INITIALIZER; /* TODO: remove this */

int				CurrentInput = VIEW_INPUT_OFF;

/* IP input configuration */
string				ip_in_addr = "239.1.1.2";
int				ip_in_port = 4000;
int				ip_in_proto = IP_PROTO_RTP;

/* Visual UI configuration */
bool				cfg_show_toolbar = true;
bool				cfg_show_metadata = false;
bool				cfg_show_status = true;

void draw_osd();
void client_area_free();
void client_area_alloc();
void client_area_check_source_size(int w,int h);
void client_area_redraw_source_frame(bool total=false);
void client_area_get_aspect_from_current_input();
void client_area_draw_overlay_borders();
void client_area_update_rects_again();
void secondary_thread_shutdown();

/* C/C++ implementation of Perl's chomp command */
void chomp(char *s) {
	char *e = s+strlen(s)-1;
	while (e >= s && (*e == '\r' || *e == '\n')) *e-- = 0;
}

void InputManager::increase_speed() {
	double curspeed = fabs((double)playback_speed_n) / playback_speed_d;
	double est;
	int nrev=1;
	int i=0;

	if (playback_speed_n < 0) {
		nrev = -1;
		for (i=user_speeds_total-1;i >= 0;) {
			est = (double)user_speeds[i].m / user_speeds[i].n;
			if (est < curspeed) break;
			i--;
		}

		if (i < 0) {
			nrev = 1;
			i = 0;
		}
	}
	else if (playback_speed_n > 0) {
		nrev = 1;
		for (i=0;i < (user_speeds_total-1);) {
			est = (double)user_speeds[i].m / user_speeds[i].n;
			if (est > curspeed) break;
			i++;
		}
	}

	ChangeSpeed((int)user_speeds[i].m*nrev,user_speeds[i].n);
}

void InputManager::decrease_speed() {
	double curspeed = fabs((double)playback_speed_n) / playback_speed_d;
	double est;
	int nrev=1;
	int i=0;

	if (playback_speed_n > 0) {
		nrev = 1;
		for (i=user_speeds_total-1;i >= 0;) {
			est = (double)user_speeds[i].m / user_speeds[i].n;
			if (est < curspeed) break;
			i--;
		}

		if (i < 0) {
			nrev = -1;
			i = 0;
		}
	}
	else if (playback_speed_n < 0) {
		nrev = -1;
		for (i=0;i < (user_speeds_total-1);) {
			est = (double)user_speeds[i].m / user_speeds[i].n;
			if (est > curspeed) break;
			i++;
		}
	}

	ChangeSpeed((int)user_speeds[i].m*nrev,user_speeds[i].n);
}

int how_many_cpus() {
	int number_of_cpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (number_of_cpus < 1) number_of_cpus = 1;
	return number_of_cpus;
}

bool InputManager::load_external_avi_for_play(const char *path) {
	bool result = false;
	struct stat st;
	size_t i;

	if (play_avi != "" && play_avi == path)
		return true;

	close_play_avi();
	play_avi = "";
	vt_play.clear();
	play_is_rec = false;

	if (*path == 0) /* a.k.a path == "" or strlen(path) == 0 */
		return false;

	if (capture_avi != "" && play_avi == capture_avi) {
		fprintf(stderr,"Not reopening file '%s', this input is already capturing that file\n",path);
		play_is_rec = true;
		return true;
	}

	/* try to catch any attempt to open an AVI already opened (or recording) on other inputs.
	 * if we see that, then switch to the other input and return false */
	for (i=VIEW_INPUT_OFF+1;i < VIEW_INPUT_MAX;i++) {
		InputManager *im = Input[i];
		if (im->play_avi == path || im->capture_avi == path) {
			fprintf(stderr,"File '%s' is already in use on input '%s'\n",path,im->osd_name);
			switch_input(i);
			update_ui();

			/* then, direct that input to play the file from the beginning */
			im->stop_playback();
			/* TODO: Some mechanism to direct the live recording to play from the start */
			im->pause_playback();
			return false;
		}
	}

	if (stat(path,&st)) {
		fprintf(stderr,"Cannot stat AVI file '%s', '%s'\n",path,strerror(errno));
		return false;
	}

	/* good. now open the AVI and read in the index */
	assert(play_avi_fd < 0);
	fprintf(stderr,"Loading and opening '%s'\n",path);
	play_avi_fd = open(path,O_RDONLY);
	if (play_avi_fd < 0) {
		fprintf(stderr,"Unable to open AVI file '%s', '%s'\n",path,strerror(errno));
		return false;
	}

	/* if the AVI was recorded by us, the filename should take the form <prefix>-<unix-timecode>.avi.
	 * if that's true, we can figure out the time recording started */
	/* TODO: do this */

	avi_reader *avi = avi_reader_create();
	if (avi == NULL) {
		close_play_avi();
		fprintf(stderr,"Cannot create AVI reader\n");
		return false;
	}
	avi_reader_fd(avi,play_avi_fd);
	if (avi_reader_scan(avi)) {
		unsigned char vstrf_data[6000];
		int v_ffmpeg_codec_id = -1;
		int video_stream = -1;

		/* AVI is OK. read in the OpenDML index, or if that's not available, the older-format index */
		avi_reader_scan_odml_index(avi);
		avi_reader_scan_index1(avi);

		/* we REQUIRE the index */
		if (avi_reader_has_an_index(avi)) {
			/* now locate the video stream */
			/* TODO: if requested by Radeus labs, locate an audio stream too */
			for (i=0;i < avi->avi_streams;i++) {
				avi_reader_stream *s = &avi->avi_stream[i];

				fprintf(stderr,"Trying %u\n",i);
				if (s->strh.fccType == avi_fccType_video && s->strf_chunk.data_length < 6000 && s->strf_chunk.absolute_data_offset != 0ULL &&
					s->strh.dwRate != 0 && s->strh.dwScale != 0 && video_stream < 0) {
					/* is this a video stream that I know how to handle? */
					/* determine this by reading the 'strf' chunk for this stream */
					/* TODO: Support for codecs other than DIVX/MPEG-4 */
					/* TODO: Include flags to indicate whether the BITMAPINFO struct has extra data that FFMPEG needs to decode the stream */
					if (lseek(play_avi_fd,s->strf_chunk.absolute_data_offset,SEEK_SET) == s->strf_chunk.absolute_data_offset &&
						read(play_avi_fd,vstrf_data,s->strf_chunk.data_length) == s->strf_chunk.data_length) {
						windows_BITMAPINFOHEADER *bmp = (windows_BITMAPINFOHEADER*)vstrf_data;
						if (bmp->biCompression == avi_fourcc_const('D','I','V','X') ||
							bmp->biCompression == avi_fourcc_const('F','M','P','4') ||
							bmp->biCompression == avi_fourcc_const('X','V','I','D')) {
							v_ffmpeg_codec_id = AV_CODEC_ID_MPEG4;
							fprintf(stderr,"MPEG-4 video stream index %u %u x %u found\n",
								i,bmp->biWidth,bmp->biHeight);
							video_stream = i;
						}
						else if (bmp->biCompression == avi_fourcc_const('d','i','v','4') ||
							bmp->biCompression == avi_fourcc_const('D','I','V','3')) {
							v_ffmpeg_codec_id = AV_CODEC_ID_MSMPEG4V3;
							fprintf(stderr,"DivX/MS-MPEG-4 video stream index %u %u x %u found\n",
								i,bmp->biWidth,bmp->biHeight);
							video_stream = i;
						}
						else if (bmp->biCompression == avi_fourcc_const('H','2','6','4')) {
							v_ffmpeg_codec_id = AV_CODEC_ID_H264;
							fprintf(stderr,"H.264 video stream index %u %u x %u found\n",
								i,bmp->biWidth,bmp->biHeight);
							video_stream = i;
						}
						else if (bmp->biCompression == avi_fourcc_const('F','L','V','1')) {
							v_ffmpeg_codec_id = AV_CODEC_ID_FLV1;
							fprintf(stderr,"DivX/MS-MPEG-4 video stream index %u %u x %u found\n",
								i,bmp->biWidth,bmp->biHeight);
							video_stream = i;
						}
					}
				}
			}
		}
		else {
			fprintf(stderr,"AVI file has no index, cannot use.\n");
			fprintf(stderr,"If you need the AVI file to work with this program consider using an AVI index repair utility.\n");
		}

		if (video_stream >= 0) {
			windows_BITMAPINFOHEADER *bmp = (windows_BITMAPINFOHEADER*)vstrf_data;
			avi_reader_stream *stream = &avi->avi_stream[video_stream];
			struct video_tracking::index_entry ie;

			/* NTS: Don't forget in Microsoft-land bitmaps are by default upside-down (bottom to top scanline order)
			 *      but only for uncompressed bitmaps, which can be top-to-bottom if the height value is negative.
			 *      Their SDKs also mention a right-to-left pixel order if the width is negative.
			 *      It's just safest to take the absolute value and be done with it */
			vt_play.video_width = abs((int)bmp->biWidth);
			vt_play.video_height = abs((int)bmp->biHeight);
			vt_play.video_rate_n = stream->strh.dwRate;
			vt_play.video_rate_d = stream->strh.dwScale;
			vt_play.video_total_frames = stream->strh.dwLength;
			/* NTS: OpenDML files write dwTotalFrames based on the "legacy" portion below 1/2GB not the actual frame count.
			 *      Get the true framecount from the index. */
			/* TODO: OpenDML AVI files also carry a 'odml:dmlh' chunk where by standard the correct length is written---read that */
			if (avi->avi_stream_odml_index != NULL) {
				if (vt_play.video_total_frames < avi->avi_stream_odml_index[video_stream].count)
					vt_play.video_total_frames = avi->avi_stream_odml_index[video_stream].count;
			}
			else if (avi->avi_stream_index1 != NULL) {
				if (vt_play.video_total_frames < avi->avi_stream_index1[video_stream].count)
					vt_play.video_total_frames = avi->avi_stream_index1[video_stream].count;
			}

			close_avcodec();
			video_avcodec = avcodec_find_decoder((AVCodecID)v_ffmpeg_codec_id);
			if (video_avcodec != NULL) {
				video_avcodec_ctx = avcodec_alloc_context3(video_avcodec);
				if (video_avcodec_ctx != NULL) {
					avcodec_get_context_defaults3(video_avcodec_ctx,video_avcodec);
					/* NTS: Some codecs (like Microsoft MPEG-4) need the width & height from the stream format */
					video_avcodec_ctx->width = vt_play.video_width;
					video_avcodec_ctx->height = vt_play.video_height;
					video_avcodec_ctx->thread_count = 1;//how_many_cpus();
					video_avcodec_ctx->thread_type = 0;//FF_THREAD_SLICE;
					/* NTS: do NOT set FF_THREAD_FRAME because this code cannot handle it,
					 *      it causes FFMPEG to buffer frames according to the number of threads,
					 *      and delay them that much, when this code assumes that there is
					 *      no delay. This code should be updated to properly handle delayed frames,
					 *      so that eventually we can capture with B-frames and still allow
					 *      random access. */
//					video_avcodec_ctx->thread_type |= FF_THREAD_FRAME;
					video_avcodec_ctx->flags2 |= CODEC_FLAG2_FAST;
					if (avcodec_open2(video_avcodec_ctx,video_avcodec,NULL)) {
						fprintf(stderr,"Failed to open codec\n");
						av_free(video_avcodec_ctx);
						video_avcodec_ctx = NULL;
						video_avcodec = NULL;
					}
					else {
						fprintf(stderr,"Video codec for AVI opened\n");
					}
				}
				else {
					fprintf(stderr,"Cannot alloc ffmpeg context\n");
				}
			}
			else {
				fprintf(stderr,"FFMPEG could not locate the decoder\n");
			}

			/* copy the index into memory */
			if (avi->avi_stream_odml_index != NULL && avi->avi_stream_odml_index[video_stream].count != 0) {
				avi_reader_stream_odml_index *idx = &avi->avi_stream_odml_index[video_stream];
				for (i=0;i < idx->count;i++) {
					ie.keyframe = AVI_ODML_INDX_NONKEY(idx->map[i].size)?0:1; /* OpenDML indexes use the 31st bit to signal "keyframe" */
					ie.offset = idx->map[i].offset;
					ie.size = AVI_ODML_INDX_SIZE(idx->map[i].size);
					vt_play.video_index.push_back(ie);
				}
			}
			else if (avi->avi_stream_index1 != NULL && avi->avi_stream_index1[video_stream].count != 0) {
				avi_reader_stream_index1 *idx = &avi->avi_stream_index1[video_stream];
				for (i=0;i < idx->count;i++) {
					/* TODO: This code might pay attention to dwFlags & NOTIME, though such AVIs are rare */
					ie.keyframe = (idx->map[i].dwFlags & riff_idx1_AVIOLDINDEX_flags_KEYFRAME)?1:0;
					ie.offset = idx->map[i].dwOffset;
					ie.size = idx->map[i].dwSize;
					vt_play.video_index.push_back(ie);
				}
			}
			else {
				fprintf(stderr,"BUG: Despite checks, neither index was loaded\n");
			}

			/* the main loop demands we know where the first keyframe lies */
			for (i=0;i < vt_play.video_index.size() && !vt_play.video_index[i].keyframe;) i++;
			vt_play.video_first_frame = i;

			result = true;
			play_avi = path;
			vt_play.video_total_frames = vt_play.video_index.size();

			/* guess the aspect ratio */
			{
				double ar = (double)vt_play.video_width / vt_play.video_height;
				fprintf(stderr,"ar=%.3f\n",ar);
				if (ar > 1.4) {
					source_ar_n = 16;
					source_ar_d = 9;
				}
				else {
					source_ar_n = 4;
					source_ar_d = 3;
				}
			}

			client_area_get_aspect_from_current_input();
			client_area_update_rects_again();

			/* encourage the player to start from the beginning.
			 * If this is the file input source, this should cause the
			 * do_video() routine to preroll the first frame onto the screen */
			playback_base_frame = 0LL;
			playback_base_time = NOW;
		}
		else {
			fprintf(stderr,"No appropriate video stream found\n");
		}
	}

	/* success or failure, we're done parsing the AVI */
	avi_reader_destroy(avi);
	return result;
}

bool load_config_parse_boolean(const char *v) {
	if (isdigit(*v))
		return atoi(v) > 0 ? true : false;
	else if (!strcmp(v,"yes"))
		return true;
	else if (!strcmp(v,"on"))
		return true;

	return false;
}

/* load configuration: section [ip] */
void load_config_section_ip(const char *name,const char *value) {
	if (!strcmp(name,"address"))
		ip_in_addr = value;
	else if (!strcmp(name,"port")) {
		ip_in_port = atoi(value);
		if (ip_in_port < 1) ip_in_port = 1;
		else if (ip_in_port > 65534) ip_in_port = 65534;
	}
	else if (!strcmp(name,"protocol")) {
		if (!strcmp(value,"rtp"))
			ip_in_proto = IP_PROTO_RTP;
		else /* udp */
			ip_in_proto = IP_PROTO_UDP;
	}
}

/* load configuration: section [ui] */
void load_config_section_ui(const char *name,const char *value) {
	if (!strcmp(name,"toolbar"))
		cfg_show_toolbar = load_config_parse_boolean(value);
	else if (!strcmp(name,"metadata"))
		cfg_show_metadata = load_config_parse_boolean(value);
	else if (!strcmp(name,"status"))
		cfg_show_status = load_config_parse_boolean(value);
	else if (!strcmp(name,"osd"))
		user_enable_osd = load_config_parse_boolean(value);
}

int load_config_section_input_sel = -1;
void load_config_section_input(const char *name,const char *value) {
	InputManager *iobj;

	if (load_config_section_input_sel < 0) return;
	iobj = Input[load_config_section_input_sel];

	if (!strcmp(name,"enable audio"))
		iobj->enable_audio = atoi(value) > 0;
	else if (!strcmp(name,"enable vbi"))
		iobj->enable_vbi = atoi(value) > 0;
	else if (!strcmp(name,"audio device"))
		iobj->audio_device = value;
	else if (!strcmp(name,"input device"))
		iobj->input_device = value;
	else if (!strcmp(name,"input standard"))
		iobj->input_standard = value;
	else if (!strcmp(name,"capture fps"))
		iobj->capture_fps = atof(value);
	else if (!strcmp(name,"capture width"))
		iobj->capture_width = atoi(value);
	else if (!strcmp(name,"capture height"))
		iobj->capture_height = atoi(value);
	else if (!strcmp(name,"codec"))
		iobj->input_codec = value;
	else if (!strcmp(name,"vcr hack"))
		iobj->vcrhack = value;
	else if (!strcmp(name,"crop")) {
		iobj->crop_defrect = iobj->crop_bounds = false;
		iobj->crop_left = iobj->crop_top = iobj->crop_width = iobj->crop_height = CROP_DEFAULT;

		if (!strcmp(value,"bound"))
			iobj->crop_bounds = true;
		else if (!strcmp(value,"default"))
			iobj->crop_defrect = true;
		else {
			char *s = (char*)value;

			/* left,top,width,height */
			if (isdigit(*s) || *s == '-') iobj->crop_left = strtol(s,&s,10);
			while (*s && *s != ',') s++;
			if (*s == ',') s++;

			if (isdigit(*s) || *s == '-') iobj->crop_top = strtol(s,&s,10);
			while (*s && *s != ',') s++;
			if (*s == ',') s++;

			if (isdigit(*s) || *s == '-') iobj->crop_width = strtol(s,&s,10);
			while (*s && *s != ',') s++;
			if (*s == ',') s++;

			if (isdigit(*s) || *s == '-') iobj->crop_height = strtol(s,&s,10);
			while (*s && *s != ',') s++;
			if (*s == ',') s++;
		}
	}
}

/* load configuration: section [xwindows] */
void load_config_section_xwindows(const char *name,const char *value) {
	if (!strcmp(name,"shm"))
		use_x_shm = load_config_parse_boolean(value);
	else if (!strcmp(name,"xvideo"))
		use_xvideo = load_config_parse_boolean(value);
}

void load_config_section_global(const char *name,const char *value) {
}

void load_config_section_unknown(const char *name,const char *value) {
}

/* INI-style configuration loader */
void load_configuration_file(const char *path) {
	void (*section_func)(const char *name,const char *value) = load_config_section_global;
	char line[1024],*p,*r,*name,*value;
	string section;
	FILE *fp;

	if ((fp = fopen(path,"r")) == NULL)
		return;

	section = "global";
	while (!feof(fp)) {
		p = line;
		line[0] = 0;
		fgets(line,sizeof(line)-1,fp);
		chomp(line); /* eat trailing \n left by fgets */

		/* skip whitespace */
		while (*p == '\t' || *p == ' ') p++;
		/* if that's it (blank line) then skip to next one */
		if (*p == 0) continue;
		/* lines that start with a pound sign are treated as comments */
		if (*p == '#') continue;

		/* if it starts with '[' then the file is declaring the start of another section */
		if (*p == '[') {
			r = strchr(++p,']'); /* skip '[' and move on to section name */
			if (r == NULL) continue; /* erm, what? */
			*r = 0; /* eat the other bracket and terminate string */
			section = p;
			if (section == "") section = "global";

			if (section == "ip")
				section_func = load_config_section_ip;
			else if (section == "ui")
				section_func = load_config_section_ui;
			else if (section == "xwindows")
				section_func = load_config_section_xwindows;
			else if (section == "global")
				section_func = load_config_section_global;
			else if (section.substr(0,7) == "input, ") { /* C++ string substr() will not throw exception if length is too long */
				const char *name = section.c_str()+7;

				section_func = load_config_section_input;
				load_config_section_input_sel = -1;
				for (unsigned int input=0;input < VIEW_INPUT_MAX;input++) {
					InputManager *iobj = Input[input];
					if (!strcmp(iobj->cfg_name,name)) {
						load_config_section_input_sel = input;
						break;
					}
				}
			}
			else
				section_func = load_config_section_unknown;

			continue;
		}

		/* else, this is a name=value pair */
		r = strchr(p,'=');
		if (r == NULL) {
			name = p;
			value = p+strlen(p);
		}
		else {
			/* eat whitespace preceeding and following the equals sign */
			char *pe = r - 1;
			while (pe > p && (*pe == '\t' || *pe == ' ')) *pe-- = 0;
			*r++ = 0; /* rub out the equals sign */
			while (*r == '\t' || *r == ' ') *r++ = 0; /* also rub out whitespace preceeding the value */
			name = p;
			value = r;
		}

		section_func(name,value);
//		fprintf(stderr,"'%s' = '%s'\n",name,value);
	}

	fclose(fp);
}

void load_configuration() {
	string path = CONFIG_FILE_NAME;	/* DEFAULT: Current working directory */
	struct stat st;

	if (stat(path.c_str(),&st) == 0 && S_ISREG(st.st_mode)) {
		load_configuration_file(path.c_str());
	}
}

const char *save_config_boolean(bool s) {
	return s ? "yes" : "no";
}

void save_configuration() {
	string path = CONFIG_FILE_NAME;
	char tmp[512];
	time_t t;
	FILE *fp;

	t = time(NULL);
	if ((fp = fopen(path.c_str(),"w")) == NULL)
		return;

	/* [ip] */
	fprintf(fp,"# auto-generated %s",ctime(&t)); /* <- ctime() appends \n at the end */
	fprintf(fp,"[ip]\n");
	fprintf(fp,"address = %s\n",ip_in_addr.c_str());
	fprintf(fp,"port = %d\n",ip_in_port);
	fprintf(fp,"protocol = %s\n",ip_proto_str[ip_in_proto]);
	fprintf(fp,"\n");

	/* [ui] */
	fprintf(fp,"[ui]\n");
	fprintf(fp,"toolbar = %s\n",save_config_boolean(cfg_show_toolbar));
	fprintf(fp,"metadata = %s\n",save_config_boolean(cfg_show_metadata));
	fprintf(fp,"status = %s\n",save_config_boolean(cfg_show_status));
	fprintf(fp,"osd = %s\n",save_config_boolean(user_enable_osd));
	fprintf(fp,"\n");

	/* [xwindows] */
	fprintf(fp,"[xwindows]\n");
	fprintf(fp,"shm = %s\n",save_config_boolean(use_x_shm));
	fprintf(fp,"xvideo = %s\n",save_config_boolean(use_xvideo));
	fprintf(fp,"\n");

	/* for each input */
	for (unsigned int input=0;input < VIEW_INPUT_MAX;input++) {
		InputManager *iobj = Input[input];

		if (iobj != NULL) {
			fprintf(fp,"[input, %s]\n",iobj->cfg_name);
			fprintf(fp,"enable vbi = %d\n",iobj->enable_vbi?1:0);
			fprintf(fp,"enable audio = %d\n",iobj->enable_audio?1:0);
			fprintf(fp,"audio device = %s\n",iobj->audio_device.c_str());
			fprintf(fp,"input device = %s\n",iobj->input_device.c_str());
			fprintf(fp,"input standard = %s\n",iobj->input_standard.c_str());
			fprintf(fp,"capture fps = %.3f\n",iobj->capture_fps);
			fprintf(fp,"capture width = %d\n",iobj->capture_width);
			fprintf(fp,"capture height = %d\n",iobj->capture_height);
			fprintf(fp,"vcr hack = %s\n",iobj->vcrhack.c_str());
			fprintf(fp,"codec = %s\n",iobj->input_codec.c_str());

			fprintf(fp,"crop = ");
			if (iobj->crop_bounds)
				fprintf(fp,"bound");
			else if (iobj->crop_defrect)
				fprintf(fp,"default");
			else {
				if (iobj->crop_left != CROP_DEFAULT) fprintf(fp,"%d",(int)(iobj->crop_left));
				fprintf(fp,",");
				if (iobj->crop_top != CROP_DEFAULT) fprintf(fp,"%d",(int)(iobj->crop_top));
				fprintf(fp,",");
				if (iobj->crop_width != CROP_DEFAULT) fprintf(fp,"%d",(int)(iobj->crop_width));
				fprintf(fp,",");
				if (iobj->crop_height != CROP_DEFAULT) fprintf(fp,"%d",(int)(iobj->crop_height));
			}
			fprintf(fp,"\n");

			fprintf(fp,"\n");
		}
	}

	fclose(fp);

	fprintf(stderr,"Configuration saved to %s\n",path.c_str());
}

static inline InputManager *CurrentInputObj() {
	assert(CurrentInput >= 0 && CurrentInput < VIEW_INPUT_MAX);
	assert(Input[CurrentInput] != NULL);
	assert(Input[CurrentInput]->index == CurrentInput);
	return Input[CurrentInput];
}

void free_inputs() {
	for (int i=0;i < VIEW_INPUT_MAX;i++) {
		if (Input[i] != NULL) {
			delete Input[i];
			Input[i] = NULL;
		}
	}
}

int init_inputs() {
	for (int i=0;i < VIEW_INPUT_MAX;i++) {
		if ((Input[i] = new InputManager(i)) == NULL) {
			g_error("Failed to initialize input #%d",i+1);
			return 1;
		}

		if (i >= VIEW_INPUT_1 && i <= VIEW_INPUT_4) {
			Input[i]->video_index = i - VIEW_INPUT_1;
			sprintf(Input[i]->file_prefix,"input%d-",i-VIEW_INPUT_1+1);
			sprintf(Input[i]->osd_name,"Video #%d",i-VIEW_INPUT_1+1);
			sprintf(Input[i]->cfg_name,"input%d",i-VIEW_INPUT_1+1);
		}
		else if (i == VIEW_INPUT_FILE) {
			sprintf(Input[i]->osd_name,"File");
			sprintf(Input[i]->cfg_name,"file");
		}
		else if (i == VIEW_INPUT_IP) {
			sprintf(Input[i]->file_prefix,"ip-%d-",1);
			sprintf(Input[i]->osd_name,"IP");
			sprintf(Input[i]->cfg_name,"ip%d",1);
		}
	}

	return 0;
}

void gtk_check_menu_item_set_active_notoggle (GtkCheckMenuItem *x, bool st) {
	if (gtk_check_menu_item_get_active(x) != (st?1:0))
		gtk_check_menu_item_set_active(x,st?1:0);
}

void gtk_toggle_tool_button_set_active_notoggle (GtkToggleToolButton *x, bool st) {
	if (gtk_toggle_tool_button_get_active(x) != (st?1:0))
		gtk_toggle_tool_button_set_active(x,st?1:0);
}

void InputManager::ChangeSpeed(int n,unsigned int d) {
	assert(n != 0);
	assert(d != 0);
	update_now_time();
	playback_base_frame = time_to_target_frame_clipped();
	playback_base_time = NOW;
	playback_speed_n = n;
	playback_speed_d = d;
}

signed long long InputManager::time_to_target_frame() {
	video_tracking *trk = (play_is_rec ? &vt_rec : &vt_play);
	if ((Playing && Paused) || !Playing) return playback_base_frame;
	signed long long target_frame = ((signed long long)
		(((NOW - playback_base_time) * trk->video_rate_n) / trk->video_rate_d));
	target_frame = (target_frame * (signed long long)playback_speed_n) / (signed long long)playback_speed_d;
	target_frame += playback_base_frame;
	return target_frame;
}

signed long long InputManager::time_to_target_frame_clipped() {
	video_tracking *trk = (play_is_rec ? &vt_rec : &vt_play);
	signed long long target_frame = time_to_target_frame();
	if (target_frame > trk->video_total_frames) target_frame = trk->video_total_frames;
	if (target_frame > trk->video_index.size()) target_frame = trk->video_index.size();
	if (target_frame < 0LL) target_frame = 0LL;
	return target_frame;
}

/* take internal state and apply to Play/Pause/Record UI elements */
void update_ui_controls() {
	/* User cannot use Record button in "No Input" and "File playback" modes */
	gtk_widget_set_sensitive(GTK_WIDGET(main_window_toolbar_record), CurrentInput > VIEW_INPUT_FILE);
	gtk_widget_set_sensitive(GTK_WIDGET(main_window_control_record), CurrentInput > VIEW_INPUT_FILE);

	/* User cannot use play/pause/stop in "No Input" */
	gtk_widget_set_sensitive(GTK_WIDGET(main_window_toolbar_play), CurrentInput > VIEW_INPUT_OFF);
	gtk_widget_set_sensitive(GTK_WIDGET(main_window_control_play), CurrentInput > VIEW_INPUT_OFF);

	gtk_widget_set_sensitive(GTK_WIDGET(main_window_toolbar_pause), CurrentInput > VIEW_INPUT_OFF);
	gtk_widget_set_sensitive(GTK_WIDGET(main_window_control_pause), CurrentInput > VIEW_INPUT_OFF);

	/* make sure the buttons reflect the Recording state */
	gtk_check_menu_item_set_active_notoggle (GTK_CHECK_MENU_ITEM(main_window_control_record), CurrentInputObj()->Recording);
	gtk_toggle_tool_button_set_active_notoggle (GTK_TOGGLE_TOOL_BUTTON(main_window_toolbar_record), CurrentInputObj()->Recording);

	/* Play/Pause = Radio buttons. Only need to set active one and the others will deactivate */
	if (CurrentInputObj()->Playing) {
		if (CurrentInputObj()->Paused) {
			gtk_check_menu_item_set_active_notoggle (GTK_CHECK_MENU_ITEM(main_window_control_pause), TRUE);
			gtk_toggle_tool_button_set_active_notoggle (GTK_TOGGLE_TOOL_BUTTON(main_window_toolbar_pause), TRUE);
		}
		else {
			gtk_check_menu_item_set_active_notoggle (GTK_CHECK_MENU_ITEM(main_window_control_play), TRUE);
			gtk_toggle_tool_button_set_active_notoggle (GTK_TOGGLE_TOOL_BUTTON(main_window_toolbar_play), TRUE);
		}
	}
	else {
		gtk_check_menu_item_set_active_notoggle (GTK_CHECK_MENU_ITEM(main_window_control_stop), TRUE);
		gtk_toggle_tool_button_set_active_notoggle (GTK_TOGGLE_TOOL_BUTTON(main_window_toolbar_stop), TRUE);
	}

	if (CurrentInputObj()->user_ar_n == 4 && CurrentInputObj()->user_ar_d == 3)
		gtk_radio_action_set_current_value (GTK_RADIO_ACTION(main_window_view_aspect_action), 1);
	else if (CurrentInputObj()->user_ar_n == 16 && CurrentInputObj()->user_ar_d == 9)
		gtk_radio_action_set_current_value (GTK_RADIO_ACTION(main_window_view_aspect_action), 2);
	else if (CurrentInputObj()->user_ar_n == -2 && CurrentInputObj()->user_ar_d == -2)
		gtk_radio_action_set_current_value (GTK_RADIO_ACTION(main_window_view_aspect_action), -1);
	else
		gtk_radio_action_set_current_value (GTK_RADIO_ACTION(main_window_view_aspect_action), 0);
}

void update_ui_inputs() {
	int x;

	x = gtk_radio_action_get_current_value (GTK_RADIO_ACTION(main_window_view_input_action));
	if (x != CurrentInput) gtk_radio_action_set_current_value (GTK_RADIO_ACTION(main_window_view_input_action), CurrentInput);

	x = gtk_radio_action_get_current_value (GTK_RADIO_ACTION(main_window_toolbar_input_action));
	if (x != CurrentInput) gtk_radio_action_set_current_value (GTK_RADIO_ACTION(main_window_toolbar_input_action), CurrentInput);
}

static bool update_vars_from_input_dialog();
static void update_input_dialog_from_vars();

static bool update_vars_from_audio_dialog();
static void update_audio_dialog_from_vars();
void generate_non_video_frame();

void update_ui() {
	update_ui_controls();
	update_ui_inputs();
}

void ui_notification_log_init() {
	if (ui_notification_log == NULL) {
		ui_notification_log = g_slist_alloc ();
	}
}

void ui_notification_log_free() {
	if (ui_notification_log != NULL) {
		for (GSList *p = ui_notification_log;p;p=g_slist_next(p)) {
			char *d = (char*)(p->data);
			if (d) g_free(d);
		}

		g_slist_free(ui_notification_log);
		ui_notification_log = NULL;
	}
}

void ui_notification(GtkMessageType typ,const char *text,...) {
	va_list va;

	va_start(va,text);

	gchar *msg = g_strdup_vprintf(text,va);

	gtk_label_set_text (GTK_LABEL (messagelabel), msg);
#ifdef GTK_HAS_INFO_BAR
	gtk_info_bar_set_message_type (GTK_INFO_BAR (infobar), typ); /* FIXME: When did this appear in the GTK+? */
	gtk_widget_show (infobar);
#endif

	if (ui_notification_log != NULL)
		ui_notification_log = g_slist_append (ui_notification_log, (gpointer)msg);
	else
		g_free (msg);

	va_end(va);
}

static void on_file_capture_open(GtkAction *action, void *p)
{
	GtkFileFilter *filter = gtk_file_filter_new();
	GtkWidget *dialog = gtk_file_chooser_dialog_new("Select capture file",
		GTK_WINDOW(main_window),GTK_FILE_CHOOSER_ACTION_OPEN,
		GTK_STOCK_CANCEL,GTK_RESPONSE_CANCEL,
		GTK_STOCK_OPEN,GTK_RESPONSE_ACCEPT,
		NULL);

	gtk_file_filter_add_pattern(filter, "*.avi");
	gtk_file_filter_add_pattern(filter, "*.ts");
	gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(dialog), filter); /* CONFIRM: This takes ownership of the filter, we don't have to free it. Right? */

	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		/* NOTE: load_external_avi_for_play() may call switch_input() if it sees that AVI already in use */
		bool switchtofile = (CurrentInput != VIEW_INPUT_FILE);
		char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
		Input[VIEW_INPUT_FILE]->load_external_avi_for_play(filename);
		g_free(filename);

		if (switchtofile) {
			switch_input(VIEW_INPUT_FILE);
			update_ui();
		}
	}

	gtk_widget_destroy(dialog);
}

static void on_not_yet_implemented(GtkAction *action, void *p)
{
	ui_notification(GTK_MESSAGE_WARNING, "Command not yet implemented");
}

void InputManager::start_recording() {
	char msg[2048],*n;
	struct stat st;
	int rd,ack=0;

	/* Already recording -> do nothing */
	if (Recording) return;
	/* Off and File inputs -> no recording -> do nothing */
	if (index == VIEW_INPUT_OFF || index == VIEW_INPUT_FILE) return;

	/* wait for the socket */
	while (socket_fd < 0) {
		if (cap_pid < 0) return;
		usleep(20000);
		idle();
	}

	/* flush socket input */
	while (idle_socket());

	if (Playing) {
		close_play_avi();
		vt_play.clear();
		vt_play = vt_rec;
		vt_play.video_total_frames = vt_play.video_index.size();
		play_avi = capture_avi;
	}
	else {
		close_play_avi();
		vt_play.clear();
		play_avi = "";
	}
	close_capture_avi();
	vt_rec.clear();
	capture_avi = "";

	/* send the command to do so */
	socket_command("record-on");
	/* the capture program will send a packet back indicating success or failure.
	 * it will also tell us the name of the AVI file, which is needed to provide
	 * random access playback */
	do {
		rd = recv(socket_fd,msg,sizeof(msg),0);
		if (rd == 0) return;
		msg[rd] = 0;
		n = strchr(msg,'\n');
		if (n) *n++ = 0; /* ASCIIZ snip */

		if (!strcmp(msg,"OK") && n != NULL) {
			int width = -1,height = -1;
			int raten = -1,rated = -1;
			double start_time = -1;
			char *name,*value;
			string vidcodec;

			ack = 1;
			while (*n != 0) {
				char *nn = strchr(n,'\n');
				if (nn) *nn++ = 0;
				else nn = n + strlen(n);

				while (*n == ' ') n++;
				name = n;

				char *equ = strchr(n,':');
				if (equ) {
					*equ++ = 0;
					value = equ;
				}
				else {
					value = NULL;
				}

//				fprintf(stderr,"   '%s' = '%s'\n",name,value);

				if (!strcasecmp(name,"file"))
					capture_avi = value;
				else if (!strcasecmp(name,"vidcodec"))
					vidcodec = value;
				else if (!strcasecmp(name,"vidrate")) {
					char *p = value;
					while (*p == ' ') p++;
					raten = strtol(p,&p,10);
					while (*p == ' ' || *p == ':' || *p == '/') p++;
					rated = strtol(p,&p,10);
					if (raten < 1 || rated < 1) rated = raten = -1;
				}
				else if (!strcasecmp(name,"vidsize")) {
					char *p = value;
					while (*p == ' ') p++;
					width = strtol(p,&p,10);
					while (*p == ' ' || *p == ':' || *p == '/' || *p == 'x') p++;
					height = strtol(p,&p,10);
					if (width < 32 || height < 32) width = height = -1;
				}
				else if (!strcasecmp(name,"StartTime")) {
					start_time = atof(value);
				}

				n = nn;
			}

			fprintf(stderr,"So: Video %s at %dx%d rate %d/%d starting at %.3f\n",
				vidcodec.c_str(),
				width,height,raten,rated,start_time);

			/* make sure the file actually exists */
			if (!(stat(capture_avi.c_str(),&st) == 0 && S_ISREG(st.st_mode))) {
				fprintf(stderr,"Recording program said '%s' but that's bull\n",
					capture_avi.c_str());

				capture_avi = "";
				ack = 0;
			}

			if (width < 1) width = 320;
			if (height < 1) height = 240;
			if (raten < 1) raten = 1;
			if (rated < 1) rated = 1;
			if (start_time < 0) start_time = NOW;

			vt_rec.video_width = width;
			vt_rec.video_height = height;
			vt_rec.video_rate_n = raten;
			vt_rec.video_rate_d = rated;
			vt_rec.video_current_frame = -1LL;
			vt_rec.video_total_frames = 0LL;
			vt_rec.video_first_frame = -1LL;
			vt_rec.start_time = start_time;
			assert(capture_avi_fd < 0);

			capture_avi_fd = open(capture_avi.c_str(),O_RDONLY);
			if (capture_avi_fd < 0)
				fprintf(stderr,"Recording program said it uses '%s' but I was unable to open the AVI. Playback will not be available. Error: %s\n",capture_avi.c_str(),strerror(errno));

			break;
		}
		else if (!strcmp(msg,"FAILED")) {
			ack = 0;
			break;
		}
	} while (1);

	play_is_rec = (play_avi != "" && capture_avi != "" && play_avi == capture_avi);

	if (ack) {
		string tmp = string(CurrentInputObj()->osd_name) + "\nRec: ";
		const char *s = strrchr(CurrentInputObj()->capture_avi.c_str(),'/');
		if (s != NULL) s++;
		else s = CurrentInputObj()->capture_avi.c_str();
		tmp += s;
		osd_set_text_big(tmp.c_str());
	}
	else {
		string tmp = string(CurrentInputObj()->osd_name) + "\nRec *FAILED*";
		osd_set_text_big(tmp.c_str());

	}

	if (ack) {
		fprintf(stderr,"Recording ack, file is '%s'\n",capture_avi.c_str());
		Recording = true;
	}
}

void InputManager::stop_recording() {
	if (!Recording) return;

	{
		string tmp = string(CurrentInputObj()->osd_name) + "\nStop Rec: ";
		const char *s = strrchr(CurrentInputObj()->capture_avi.c_str(),'/');
		if (s != NULL) s++;
		else s = CurrentInputObj()->capture_avi.c_str();
		tmp += s;
		osd_set_text_big(tmp.c_str());
	}

	/* TODO: Wait for socket to respond */
	/* send the command to do so */
	socket_command("record-off");
	/* TODO: Read back response indicating whether recording started, and what capture file */

	/* if the playback AVI is the capture AVI, and the user isn't playing, then reset the playback pointer */
	if (!Playing && play_is_rec) {
		vt_rec.video_current_frame = -1LL;
	}

	Recording = false;
}

void InputManager::start_playback() {
	if (Playing && !Paused) return;
	if (index == VIEW_INPUT_OFF) return;

	playback_speed_n = 1;
	playback_speed_d = 1;

	if (Playing) {
		if (Paused) {
			video_tracking *trk = (play_is_rec ? &vt_rec : &vt_play);

			if (index == VIEW_INPUT_FILE && (trk->video_current_frame+1) >= trk->video_total_frames) {
				trk->video_current_frame = -1LL; /* force rescan */
				playback_base_frame = 0LL; /* restart from beginning */
				playback_base_time = NOW;
			}
			else {
				trk->video_current_frame = -1LL; /* force rescan */
				playback_base_time = NOW;
			}
		}
	}
	else {
		if (play_avi == "") {
			fprintf(stderr,"Play file not selected, using what you are recording/have recorded now\n");
			close_play_avi();
			vt_play.clear();
			play_avi = capture_avi;
		}

		if (play_avi == "") {
			if (index == VIEW_INPUT_FILE) {
				string tmp = string(CurrentInputObj()->osd_name) + "\nPlay *FAILED*\nUse File -> Open to choose";
				osd_set_text_big(tmp.c_str());
			}
			else {
				string tmp = string(CurrentInputObj()->osd_name) + "\nPlay *FAILED* Nothing to play ";
				osd_set_text_big(tmp.c_str());
			}

			return;
		}

		update_now_time();
		play_is_rec = (play_avi == capture_avi);
		if (play_is_rec) fprintf(stderr,"Playback file is what you are recording now\n");

		if (Recording) {
			/* if recording, then playback will always begin just before where you were recording.
			 * just as on VHS, if you hit STOP then PLAY, you ended up right where you were just recording. */
			if (!play_is_rec) {
				close_play_avi();
				vt_play.clear();
				play_is_rec = true;
				play_avi = capture_avi;
			}

			/* trigger reset decoding */
			vt_rec.video_current_frame = -1LL;
			playback_base_frame = vt_rec.video_total_frames - 2LL;
			playback_base_time = NOW;
		}
		else {
			/* use the "current frame" pointer to enable random access across the input's recordings */
			if (play_is_rec) {
				playback_base_frame = vt_rec.video_current_frame;
				playback_base_time = NOW;

				/* the user might hit "play" again to replay the file */
				if (playback_base_frame >= vt_rec.video_total_frames) {
					playback_base_frame = 0;
					vt_rec.video_current_frame = -1LL;
				}
			}
			else {
				playback_base_frame = vt_play.video_current_frame;
				playback_base_time = NOW;
			}
		}

		/* current_frame might be -1LL if nothing played yet */
		if (playback_base_frame == -1LL)
			playback_base_frame = 0LL;
	}

	{
		string tmp = string(CurrentInputObj()->osd_name) + "\nPlay: ";
		const char *s = strrchr(CurrentInputObj()->play_avi.c_str(),'/');
		if (s != NULL) s++;
		else s = CurrentInputObj()->play_avi.c_str();
		tmp += s;
		osd_set_text_big(tmp.c_str());
	}

	Playing = true;
	Paused = false;

	if (video_should_redraw_t < 0) video_should_redraw_t = NOW + 0.2;
	if (do_video_source(CurrentInputObj(),true))
		client_area_redraw_source_frame();
}

void InputManager::pause_playback() {
	if (!Playing) {
		start_playback();
		if (!Playing) return;
	}

	{
		string tmp = string(CurrentInputObj()->osd_name) + "\nPause: ";
		const char *s = strrchr(CurrentInputObj()->play_avi.c_str(),'/');
		if (s != NULL) s++;
		else s = CurrentInputObj()->play_avi.c_str();
		tmp += s;
		osd_set_text_big(tmp.c_str());
	}

	if (!Paused) {
		update_now_time();
		playback_base_frame = time_to_target_frame_clipped();
		playback_base_time = NOW;
	}

	Paused = true;

	playback_speed_n = 1;
	playback_speed_d = 1;

	if (video_should_redraw_t < 0) video_should_redraw_t = NOW + 0.2;
	if (do_video_source(CurrentInputObj(),true))
		client_area_redraw_source_frame();
}

void InputManager::stop_playback() {
	if (!Playing) return;

	{
		string tmp = string(CurrentInputObj()->osd_name) + "\nStop: ";
		const char *s = strrchr(CurrentInputObj()->play_avi.c_str(),'/');
		if (s != NULL) s++;
		else s = CurrentInputObj()->play_avi.c_str();
		tmp += s;

		if (CurrentInputObj()->Recording) {
			const char *s = strrchr(CurrentInputObj()->capture_avi.c_str(),'/');
			if (s != NULL) s++;
			else s = CurrentInputObj()->capture_avi.c_str();
			tmp += string("\nRec: ") + s;
		}

		osd_set_text_big(tmp.c_str());
	}

	update_now_time();
	playback_base_frame = time_to_target_frame_clipped();
	playback_base_time = NOW;

	playback_speed_n = 1;
	playback_speed_d = 1;

	Playing = false;
	Paused = false;

	/* File input: this should rewind back to the beginning of the file */
	if (index == VIEW_INPUT_FILE) {
		vt_play.video_current_frame = -1LL;
		playback_base_frame = 0LL;
	}

	if (video_should_redraw_t < 0) video_should_redraw_t = NOW + 0.2;
	if (do_video_source(CurrentInputObj(),true))
		client_area_redraw_source_frame();
}

void InputManager::single_step(int frames) {
	if (!Playing) {
		start_playback();
		if (!Playing) return;
	}

	Paused = true;
}

/* SMPTE colorbars (8-bit Y Cb Cr values) */
static unsigned char smpte_bars[8*3] = {
	235,128,128,	/* white */
	162,44,142,	/* yellow */
	131,156,44,	/* cyan */
	112,72,58,	/* green */
	84,184,198,	/* magenta */
	65,100,212,	/* red */
	35,212,114,	/* blue */
	16,128,128	/* black */
};

static inline uint32_t yuv_to_rgb24(unsigned char y,unsigned char cb,unsigned char cr) {
	const int yd = 219;
	const int cd = (1000 * 219) / 255;
	const int rv = ((1403 * 256) / cd);
	const int gu = ((-344 * 256) / cd);
	const int gv = ((-714 * 256) / cd);
	const int bu = ((1770 * 256) / cd);
	const int ym = (255 * 256) / yd;
	int yb = ((int)y - 16) * ym;
	int r = (yb + (((int)cr - 128) * rv)) >> 8;
	int g = (yb + (((int)cb - 128) * gu) + (((int)cr - 128) * gv)) >> 8;
	int b = (yb + (((int)cb - 128) * bu)) >> 8;
	if (r < 0) r = 0;
	else if (r > 255) r = 255;
	if (g < 0) g = 0;
	else if (g > 255) g = 255;
	if (b < 0) b = 0;
	else if (b > 255) b = 255;
	return b + (g << 8) + (r << 16);
}

void plane8_hinterp(unsigned char *d,size_t dc,unsigned char *s,size_t step) {
	size_t xf = 0;

	do {
		*d++ = (unsigned char)((((size_t)s[0] * (0x10000 - xf)) + ((size_t)s[1] * xf)) >> 16UL);
		xf += step;
		s += xf >> 16;
		xf &= 0xFFFFUL;
	} while (--dc != 0UL);
}

static unsigned char clip512_32[512] = {0};
static unsigned char clip512_64[512] = {0};
static unsigned char clip512[512] = {0};

/* YV12 planar -> RGB16 (or BGR16) 5:5:5.
 * High performance integer code */
void yv12_to_client_area_rgb16_555(unsigned char *Y,size_t Ystride,unsigned char *U,size_t Ustride,unsigned char *V,size_t Vstride,unsigned char yshr) {
	unsigned char temp_Y[2048],temp_U[1024],temp_V[1024];
	uint16_t *dst = (uint16_t*)((unsigned char*)client_area_image->data +
		(client_area_video_y * client_area_image->bytes_per_line) +
		(client_area_video_x * 2)); /* <- start it at the "active" portion of the bitmap */
	uint16_t dcolor;
	unsigned int x,y,w,h,prev_y=-1;
	unsigned int xstep;

	/* coefficients */
	const int yd = 219;
	const int cd = (1000 * 219) / 255;
	const int rv = ((1403 * 256) / cd);
	const int gu = ((-344 * 256) / cd);
	const int gv = ((-714 * 256) / cd);
	const int bu = ((1770 * 256) / cd);
	const int ym = (255 * 256) / yd;

	if (clip512_32[0] == 0) {
		for (x=0;x < 512;x++) {
			y = x - 128;
			clip512_32[x] = ((int)y < 0) ? 0 : ((y > 255) ? 63 : (y>>2));
		}
	}

	w = client_area_video_w & (~1); /* <- optimization */
	if (w < 2) return;
	h = client_area_video_h;
	/* TODO: Alternate render pattern for interlaced video */
	for (y=0;y < h;y++) {
		uint16_t *dstrow = (uint16_t*)((char*)dst + (y * client_area_image->bytes_per_line));
		unsigned int scaled_y = (y * source_image_height) / client_area_video_h;
		/* remember that part of our rendering involves scaling the video.
		 * being CPU based the best we can offer is nearest-neighbor interpolation */
		unsigned char *sY = Y + ( scaled_y        * Ystride);
		unsigned char *sU = U + ((scaled_y>>yshr) * Ustride);
		unsigned char *sV = V + ((scaled_y>>yshr) * Vstride);

		if (scaled_y != prev_y) {
			xstep = (source_image_width * 0x10000) / client_area_video_w;
			plane8_hinterp(temp_Y/*dest*/,w/*dest*/,sY/*source*/,xstep);
			plane8_hinterp(temp_U/*dest*/,(w>>1)/*dest*/,sU/*source*/,xstep);
			plane8_hinterp(temp_V/*dest*/,(w>>1)/*dest*/,sV/*source*/,xstep);
			prev_y = scaled_y;
		}

		sY = temp_Y;
		sU = temp_U;
		sV = temp_V;
		if (client_area_image->blue_mask == 0x1F) { /* (typical) ARGB */
			for (x=0;x < w;x += 2) {
				int yb = ((int)(*sY++) - 16) * ym;
				unsigned char cb = *sU++,cr = *sV++;
				{
					int r = ((yb + (((int)cr - 128) * rv)) >> 8) + 128;
					int g = ((yb + (((int)cb - 128) * gu) + (((int)cr - 128) * gv)) >> 8) + 128;
					int b = ((yb + (((int)cb - 128) * bu)) >> 8) + 128;
					*dstrow++ = clip512_32[b] | (clip512_32[g] << 5) | (clip512_32[r] << 10);
				}

				yb = ((int)(*sY++) - 16) * ym;
				{
					int r = ((yb + (((int)cr - 128) * rv)) >> 8) + 128;
					int g = ((yb + (((int)cb - 128) * gu) + (((int)cr - 128) * gv)) >> 8) + 128;
					int b = ((yb + (((int)cb - 128) * bu)) >> 8) + 128;
					*dstrow++ = clip512_32[b] | (clip512_32[g] << 5) | (clip512_32[r] << 10);
				}
			}
		}
		else { /* (also-typical) ABGR */
			for (x=0;x < w;x += 2) {
				int yb = ((int)(*sY++) - 16) * ym;
				unsigned char cb = *sU++,cr = *sV++;
				{
					int r = ((yb + (((int)cr - 128) * rv)) >> 8) + 128;
					int g = ((yb + (((int)cb - 128) * gu) + (((int)cr - 128) * gv)) >> 8) + 128;
					int b = ((yb + (((int)cb - 128) * bu)) >> 8) + 128;
					*dstrow++ = clip512_32[r] | (clip512_32[g] << 5) | (clip512_32[b] << 10);
				}

				yb = ((int)(*sY++) - 16) * ym;
				{
					int r = ((yb + (((int)cr - 128) * rv)) >> 8) + 128;
					int g = ((yb + (((int)cb - 128) * gu) + (((int)cr - 128) * gv)) >> 8) + 128;
					int b = ((yb + (((int)cb - 128) * bu)) >> 8) + 128;
					*dstrow++ = clip512_32[r] | (clip512_32[g] << 5) | (clip512_32[b] << 10);
				}
			}
		}
	}
}

/* YV12 planar -> RGB16 (or BGR16) 5:6:5.
 * High performance integer code */
void yv12_to_client_area_rgb16_565(unsigned char *Y,size_t Ystride,unsigned char *U,size_t Ustride,unsigned char *V,size_t Vstride,unsigned char yshr) {
	unsigned char temp_Y[2048],temp_U[1024],temp_V[1024];
	uint16_t *dst = (uint16_t*)((unsigned char*)client_area_image->data +
		(client_area_video_y * client_area_image->bytes_per_line) +
		(client_area_video_x * 2)); /* <- start it at the "active" portion of the bitmap */
	uint16_t dcolor;
	unsigned int x,y,w,h,prev_y=-1;
	unsigned int xstep;

	/* coefficients */
	const int yd = 219;
	const int cd = (1000 * 219) / 255;
	const int rv = ((1403 * 256) / cd);
	const int gu = ((-344 * 256) / cd);
	const int gv = ((-714 * 256) / cd);
	const int bu = ((1770 * 256) / cd);
	const int ym = (255 * 256) / yd;

	if (clip512_64[0] == 0) {
		for (x=0;x < 512;x++) {
			y = x - 128;
			clip512_64[x] = ((int)y < 0) ? 0 : ((y > 255) ? 63 : (y>>2));
		}
	}

	w = client_area_video_w & (~1); /* <- optimization */
	if (w < 2) return;
	h = client_area_video_h;
	/* TODO: Alternate render pattern for interlaced video */
	for (y=0;y < h;y++) {
		uint16_t *dstrow = (uint16_t*)((char*)dst + (y * client_area_image->bytes_per_line));
		unsigned int scaled_y = (y * source_image_height) / client_area_video_h;
		/* remember that part of our rendering involves scaling the video.
		 * being CPU based the best we can offer is nearest-neighbor interpolation */
		unsigned char *sY = Y + ( scaled_y        * Ystride);
		unsigned char *sU = U + ((scaled_y>>yshr) * Ustride);
		unsigned char *sV = V + ((scaled_y>>yshr) * Vstride);

		if (scaled_y != prev_y) {
			xstep = (source_image_width * 0x10000) / client_area_video_w;
			plane8_hinterp(temp_Y/*dest*/,w/*dest*/,sY/*source*/,xstep);
			plane8_hinterp(temp_U/*dest*/,(w>>1)/*dest*/,sU/*source*/,xstep);
			plane8_hinterp(temp_V/*dest*/,(w>>1)/*dest*/,sV/*source*/,xstep);
			prev_y = scaled_y;
		}

		sY = temp_Y;
		sU = temp_U;
		sV = temp_V;
		if (client_area_image->blue_mask == 0x1F) { /* (typical) ARGB */
			for (x=0;x < w;x += 2) {
				int yb = ((int)(*sY++) - 16) * ym;
				unsigned char cb = *sU++,cr = *sV++;
				{
					int r = ((yb + (((int)cr - 128) * rv)) >> 8) + 128;
					int g = ((yb + (((int)cb - 128) * gu) + (((int)cr - 128) * gv)) >> 8) + 128;
					int b = ((yb + (((int)cb - 128) * bu)) >> 8) + 128;
					*dstrow++ = (clip512_64[b] >> 1) | (clip512_64[g] << 5) | ((clip512_64[r] << 10) & 0xF800);
				}

				yb = ((int)(*sY++) - 16) * ym;
				{
					int r = ((yb + (((int)cr - 128) * rv)) >> 8) + 128;
					int g = ((yb + (((int)cb - 128) * gu) + (((int)cr - 128) * gv)) >> 8) + 128;
					int b = ((yb + (((int)cb - 128) * bu)) >> 8) + 128;
					*dstrow++ = (clip512_64[b] >> 1) | (clip512_64[g] << 5) | ((clip512_64[r] << 10) & 0xF800);
				}
			}
		}
		else { /* (also-typical) ABGR */
			for (x=0;x < w;x += 2) {
				int yb = ((int)(*sY++) - 16) * ym;
				unsigned char cb = *sU++,cr = *sV++;
				{
					int r = ((yb + (((int)cr - 128) * rv)) >> 8) + 128;
					int g = ((yb + (((int)cb - 128) * gu) + (((int)cr - 128) * gv)) >> 8) + 128;
					int b = ((yb + (((int)cb - 128) * bu)) >> 8) + 128;
					*dstrow++ = (clip512_64[r] >> 1) | (clip512_64[g] << 5) | ((clip512_64[b] << 10) & 0xF800);
				}

				yb = ((int)(*sY++) - 16) * ym;
				{
					int r = ((yb + (((int)cr - 128) * rv)) >> 8) + 128;
					int g = ((yb + (((int)cb - 128) * gu) + (((int)cr - 128) * gv)) >> 8) + 128;
					int b = ((yb + (((int)cb - 128) * bu)) >> 8) + 128;
					*dstrow++ = (clip512_64[r] >> 1) | (clip512_64[g] << 5) | ((clip512_64[b] << 10) & 0xF800);
				}
			}
		}
	}
}

/* YV12 planar -> RGB24 (or BGR24).
 * High performance integer code */
void yv12_to_client_area_rgb24(unsigned char *Y,size_t Ystride,unsigned char *U,size_t Ustride,unsigned char *V,size_t Vstride,unsigned char yshr) {
	unsigned char temp_Y[2048],temp_U[1024],temp_V[1024];
	unsigned char *dst = ((unsigned char*)client_area_image->data +
		(client_area_video_y * client_area_image->bytes_per_line) +
		(client_area_video_x * 3)); /* <- start it at the "active" portion of the bitmap */
	uint32_t dcolor;
	unsigned int x,y,w,h,prev_y=-1;
	unsigned int xstep;

	/* coefficients */
	const int yd = 219;
	const int cd = (1000 * 219) / 255;
	const int rv = ((1403 * 256) / cd);
	const int gu = ((-344 * 256) / cd);
	const int gv = ((-714 * 256) / cd);
	const int bu = ((1770 * 256) / cd);
	const int ym = (255 * 256) / yd;

	if (clip512[0] == 0) {
		for (x=0;x < 512;x++) {
			y = x - 128;
			clip512[x] = ((int)y < 0) ? 0 : ((y > 255) ? 255 : y);
		}
	}

	w = client_area_video_w & (~1); /* <- optimization */
	if (w < 2) return;
	h = client_area_video_h;
	/* TODO: Alternate render pattern for interlaced video */
	for (y=0;y < h;y++) {
		unsigned char *dstrow = (unsigned char*)dst + (y * client_area_image->bytes_per_line);
		unsigned int scaled_y = (y * source_image_height) / client_area_video_h;
		/* remember that part of our rendering involves scaling the video.
		 * being CPU based the best we can offer is nearest-neighbor interpolation */
		unsigned char *sY = Y + ( scaled_y        * Ystride);
		unsigned char *sU = U + ((scaled_y>>yshr) * Ustride);
		unsigned char *sV = V + ((scaled_y>>yshr) * Vstride);

		if (scaled_y != prev_y) {
			xstep = (source_image_width * 0x10000) / client_area_video_w;
			plane8_hinterp(temp_Y/*dest*/,w/*dest*/,sY/*source*/,xstep);
			plane8_hinterp(temp_U/*dest*/,(w>>1)/*dest*/,sU/*source*/,xstep);
			plane8_hinterp(temp_V/*dest*/,(w>>1)/*dest*/,sV/*source*/,xstep);
			prev_y = scaled_y;
		}

		sY = temp_Y;
		sU = temp_U;
		sV = temp_V;
		if (client_area_image->blue_mask == 0xFF) { /* (typical) ARGB */
			for (x=0;x < w;x += 2) {
				int yb = ((int)(*sY++) - 16) * ym;
				unsigned char cb = *sU++,cr = *sV++;
				{
					int r = ((yb + (((int)cr - 128) * rv)) >> 8) + 128;
					int g = ((yb + (((int)cb - 128) * gu) + (((int)cr - 128) * gv)) >> 8) + 128;
					int b = ((yb + (((int)cb - 128) * bu)) >> 8) + 128;
					*dstrow++ = b;
					*dstrow++ = g;
					*dstrow++ = r;
				}

				yb = ((int)(*sY++) - 16) * ym;
				{
					int r = ((yb + (((int)cr - 128) * rv)) >> 8) + 128;
					int g = ((yb + (((int)cb - 128) * gu) + (((int)cr - 128) * gv)) >> 8) + 128;
					int b = ((yb + (((int)cb - 128) * bu)) >> 8) + 128;
					*dstrow++ = b;
					*dstrow++ = g;
					*dstrow++ = r;
				}
			}
		}
		else { /* (also-typical) ABGR */
			for (x=0;x < w;x += 2) {
				int yb = ((int)(*sY++) - 16) * ym;
				unsigned char cb = *sU++,cr = *sV++;
				{
					int r = ((yb + (((int)cr - 128) * rv)) >> 8) + 128;
					int g = ((yb + (((int)cb - 128) * gu) + (((int)cr - 128) * gv)) >> 8) + 128;
					int b = ((yb + (((int)cb - 128) * bu)) >> 8) + 128;
					*dstrow++ = r;
					*dstrow++ = g;
					*dstrow++ = b;
				}

				yb = ((int)(*sY++) - 16) * ym;
				{
					int r = ((yb + (((int)cr - 128) * rv)) >> 8) + 128;
					int g = ((yb + (((int)cb - 128) * gu) + (((int)cr - 128) * gv)) >> 8) + 128;
					int b = ((yb + (((int)cb - 128) * bu)) >> 8) + 128;
					*dstrow++ = r;
					*dstrow++ = g;
					*dstrow++ = b;
				}
			}
		}
	}
}

/* YV12 planar -> RGB32 (or BGR32).
 * High performance integer code */
void yv12_to_client_area_rgb32(unsigned char *Y,size_t Ystride,unsigned char *U,size_t Ustride,unsigned char *V,size_t Vstride,unsigned char yshr) {
	unsigned char temp_Y[2048],temp_U[1024],temp_V[1024];
	uint32_t *dst = (uint32_t*)((unsigned char*)client_area_image->data +
		(client_area_video_y * client_area_image->bytes_per_line) +
		(client_area_video_x * 4)); /* <- start it at the "active" portion of the bitmap */
	uint32_t dcolor;
	unsigned int x,y,w,h,prev_y=-1;
	unsigned int xstep;

	/* coefficients */
	const int yd = 219;
	const int cd = (1000 * 219) / 255;
	const int rv = ((1403 * 256) / cd);
	const int gu = ((-344 * 256) / cd);
	const int gv = ((-714 * 256) / cd);
	const int bu = ((1770 * 256) / cd);
	const int ym = (255 * 256) / yd;

	if (clip512[0] == 0) {
		for (x=0;x < 512;x++) {
			y = x - 128;
			clip512[x] = ((int)y < 0) ? 0 : ((y > 255) ? 255 : y);
		}
	}

	w = client_area_video_w & (~1); /* <- optimization */
	if (w < 2) return;
	h = client_area_video_h;
	/* TODO: Alternate render pattern for interlaced video */
	for (y=0;y < h;y++) {
		uint32_t *dstrow = (uint32_t*)((char*)dst + (y * client_area_image->bytes_per_line));
		unsigned int scaled_y = (y * source_image_height) / client_area_video_h;
		/* remember that part of our rendering involves scaling the video.
		 * being CPU based the best we can offer is nearest-neighbor interpolation */
		unsigned char *sY = Y + ( scaled_y        * Ystride);
		unsigned char *sU = U + ((scaled_y>>yshr) * Ustride);
		unsigned char *sV = V + ((scaled_y>>yshr) * Vstride);

		if (scaled_y != prev_y) {
			xstep = (source_image_width * 0x10000) / client_area_video_w;
			plane8_hinterp(temp_Y/*dest*/,w/*dest*/,sY/*source*/,xstep);
			plane8_hinterp(temp_U/*dest*/,(w>>1)/*dest*/,sU/*source*/,xstep);
			plane8_hinterp(temp_V/*dest*/,(w>>1)/*dest*/,sV/*source*/,xstep);
			prev_y = scaled_y;
		}

		sY = temp_Y;
		sU = temp_U;
		sV = temp_V;
		if (client_area_image->blue_mask == 0xFF) { /* (typical) ARGB */
			for (x=0;x < w;x += 2) {
				int yb = ((int)(*sY++) - 16) * ym;
				unsigned char cb = *sU++,cr = *sV++;
				{
					int r = ((yb + (((int)cr - 128) * rv)) >> 8) + 128;
					int g = ((yb + (((int)cb - 128) * gu) + (((int)cr - 128) * gv)) >> 8) + 128;
					int b = ((yb + (((int)cb - 128) * bu)) >> 8) + 128;
					*dstrow++ = clip512[b] | (clip512[g] << 8) | (clip512[r] << 16);
				}

				yb = ((int)(*sY++) - 16) * ym;
				{
					int r = ((yb + (((int)cr - 128) * rv)) >> 8) + 128;
					int g = ((yb + (((int)cb - 128) * gu) + (((int)cr - 128) * gv)) >> 8) + 128;
					int b = ((yb + (((int)cb - 128) * bu)) >> 8) + 128;
					*dstrow++ = clip512[b] | (clip512[g] << 8) | (clip512[r] << 16);
				}
			}
		}
		else { /* (also-typical) ABGR */
			for (x=0;x < w;x += 2) {
				int yb = ((int)(*sY++) - 16) * ym;
				unsigned char cb = *sU++,cr = *sV++;
				{
					int r = ((yb + (((int)cr - 128) * rv)) >> 8) + 128;
					int g = ((yb + (((int)cb - 128) * gu) + (((int)cr - 128) * gv)) >> 8) + 128;
					int b = ((yb + (((int)cb - 128) * bu)) >> 8) + 128;
					*dstrow++ = clip512[r] | (clip512[g] << 8) | (clip512[b] << 16);
				}

				yb = ((int)(*sY++) - 16) * ym;
				{
					int r = ((yb + (((int)cr - 128) * rv)) >> 8) + 128;
					int g = ((yb + (((int)cb - 128) * gu) + (((int)cr - 128) * gv)) >> 8) + 128;
					int b = ((yb + (((int)cb - 128) * bu)) >> 8) + 128;
					*dstrow++ = clip512[r] | (clip512[g] << 8) | (clip512[b] << 16);
				}
			}
		}
	}
}

void gui_status(const char *msg);

bool put_live_frame_on_screen(InputManager *input,bool force_redraw/*TODO*/) {
	bool ret = false;
	unsigned int yshr = 0;
	uint32_t slot,offset,generation,width,height,stride,y;
	volatile struct live_shm_header *xx =
		(volatile struct live_shm_header*)(input->shmem);

	if (input->shmem == NULL || input->shmem_width < 1 || input->shmem_height < 1 || input->shmem_slots == 0)
		return ret;

	if (xx->header == LIVE_SHM_HEADER_UPDATING || xx->slots != input->shmem_slots ||
		xx->width != input->shmem_width || xx->height != input->shmem_height) {
		return ret;
	}
	else if (xx->header != LIVE_SHM_HEADER) {
		input->shmem_out = 0;
		input->shmem_width = 0;
		input->shmem_height = 0;
		input->shmem_slots = 0;
		return ret;
	}

	slot = xx->in;
	if (slot < 0 || slot >= input->shmem_slots)
		return ret;
	if (input->shmem_out == slot)
		return ret;

	width = xx->width;
	height = xx->height;
	stride = xx->stride;
	offset = xx->map[input->shmem_out].offset;
	generation = xx->map[input->shmem_out].generation;
	if (++input->shmem_out >= input->shmem_slots)
		input->shmem_out = 0;

	if (xx->color_fmt == LIVE_COLOR_FMT_YUV422)
		yshr = 0;
	else if (xx->color_fmt == LIVE_COLOR_FMT_YUV420)
		yshr = 1;
	else
		return ret;

	if ((offset+xx->frame_size) > input->shmem_size)
		return ret;
	if (width > 2048 || height > 2048)
		return ret;

	unsigned char *Y = (unsigned char*)input->shmem + offset;
	unsigned char *U = Y + (stride * height);
	if ((U+((stride/2)*(height>>yshr))) > ((unsigned char*)input->shmem + input->shmem_size)) return ret;
	unsigned char *V = U + ((stride/2)*(height>>yshr));
	if ((V+((stride/2)*(height>>yshr))) > ((unsigned char*)input->shmem + input->shmem_size)) return ret;

	client_area_check_source_size(width,height);
	if (client_area_xvimage) {
		assert(source_image_width >= width);
		assert(source_image_height >= height);

		size_t dYstride = client_area_xvimage->pitches[client_area_y_plane_index];
		unsigned char *dY = (unsigned char*)client_area_xvimage->data + client_area_xvimage->offsets[client_area_y_plane_index];

		size_t dUstride = client_area_xvimage->pitches[client_area_u_plane_index];
		unsigned char *dU = (unsigned char*)client_area_xvimage->data + client_area_xvimage->offsets[client_area_u_plane_index];

		size_t dVstride = client_area_xvimage->pitches[client_area_v_plane_index];
		unsigned char *dV = (unsigned char*)client_area_xvimage->data + client_area_xvimage->offsets[client_area_v_plane_index];

		for (y=0;y < height;y++)
			memcpy(dY+(y*dYstride),Y+(y*stride),min((size_t)stride,dYstride));

		for (y=0;y < height;y++)
			memcpy(dU+((y>>1)*dUstride),U+((y>>yshr)*(stride>>1)),min((size_t)(stride>>1),dUstride));

		for (y=0;y < height;y++)
			memcpy(dV+((y>>1)*dVstride),V+((y>>yshr)*(stride>>1)),min((size_t)(stride>>1),dVstride));

		ret = true;
	}
	else if (client_area_image) {
		if (client_area_image->bits_per_pixel == 32) {
			yv12_to_client_area_rgb32(Y,stride,U,stride>>1,V,stride>>1,yshr);
			ret = true;
		}
		else if (client_area_image->bits_per_pixel == 24) {
			yv12_to_client_area_rgb24(Y,stride,U,stride>>1,V,stride>>1,yshr);
			ret = true;
		}
		else if (client_area_image->bits_per_pixel == 16) {
			/* it depends: is this 5:5:5 RGB or 5:6:5 RGB? */
			if (client_area_image->green_mask == (0x3F << 5))
				yv12_to_client_area_rgb16_565(Y,stride,U,stride>>1,V,stride>>1,yshr);
			else
				yv12_to_client_area_rgb16_555(Y,stride,U,stride>>1,V,stride>>1,yshr);

			ret = true;
		}
	}

	std::string status;

	if (!input->Playing && input->vt_rec.video_rate_n > 0UL && input->vt_rec.video_rate_d > 0UL) {
		char tmp[256];
		const char *name;
		unsigned int H,M,S,mS;
		unsigned long long tm;
		unsigned int tH,tM,tS,tmS;

		if (input->play_is_rec)
			name = strrchr(input->capture_avi.c_str(),'/');
		else
			name = strrchr(input->play_avi.c_str(),'/');

		if (name == NULL) name = "";
		else if (*name == '/') name++;

		tm = ((max(input->vt_rec.video_current_frame,0LL) * 1000LL) * input->vt_rec.video_rate_d) / input->vt_rec.video_rate_n;
		mS = (unsigned int)(tm % 1000LL);
		S = (unsigned int)((tm / 1000LL) % 60ULL);
		M = (unsigned int)((tm / 1000LL / 60LL) % 60ULL);
		H = (unsigned int)(tm / 1000LL / 3600LL);

		tm = ((input->vt_rec.video_total_frames * 1000LL) * input->vt_rec.video_rate_d) / input->vt_rec.video_rate_n;
		tmS = (unsigned int)(tm % 1000LL);
		tS = (unsigned int)((tm / 1000LL) % 60ULL);
		tM = (unsigned int)((tm / 1000LL / 60LL) % 60ULL);
		tH = (unsigned int)(tm / 1000LL / 3600LL);

		snprintf(tmp,sizeof(tmp),"Recorded %u:%02u:%02u.%03u (playback at %u:%02u:%02u.%03u) %s",tH,tM,tS,tmS,H,M,S,mS,name);
		status = tmp;
	}

	if (!input->Playing) {
		char tmp[256];
		char *w = tmp;

		w += sprintf(w,"%u-ch %uHz audio ",
			(unsigned int)xx->audio_channels,
			(unsigned int)xx->audio_rate);

		for (unsigned int ch=0;ch < 4 && ch < (unsigned int)xx->audio_channels;ch++) {
			w += sprintf(w,"ch%u:max=%%%03u avg=%%%03u ",ch+1,
				(xx->map[input->shmem_out].audio_max_level[ch]*100)/32767,
				(xx->map[input->shmem_out].audio_avg_level[ch]*100)/32767);
		}

		status += tmp;
	}

	gui_status(status.c_str());
	return ret;
}

void generate_non_video_frame() {
	unsigned char *row,*p,a,b,c,d;
	unsigned int x,y,w,h,cc,aa;

	if (client_area_xvimage) {
		unsigned char *Y = (unsigned char*)client_area_xvimage->data + client_area_xvimage->offsets[client_area_y_plane_index];
		unsigned char *U = (unsigned char*)client_area_xvimage->data + client_area_xvimage->offsets[client_area_u_plane_index];
		unsigned char *V = (unsigned char*)client_area_xvimage->data + client_area_xvimage->offsets[client_area_v_plane_index];
		size_t Ystride = client_area_xvimage->pitches[client_area_y_plane_index];
		size_t Ustride = client_area_xvimage->pitches[client_area_u_plane_index];
		size_t Vstride = client_area_xvimage->pitches[client_area_v_plane_index];
		h = (unsigned int)client_area_xvimage->height;
		w = (unsigned int)client_area_xvimage->width;

		/* generate colorbars */
		x = 0;
		for (cc=1;cc <= 8;cc++) {
			y = ((cc * w) / 8) & (~1);
			a = smpte_bars[(cc-1)*3];
			b = smpte_bars[(cc-1)*3 + 1];
			c = smpte_bars[(cc-1)*3 + 2];
			while (x < y) {
				Y[x+0] = a;
				Y[x+1] = a;
				U[x>>1] = b;
				V[x>>1] = c;
				x += 2;
			}
		}
		for (y=1;y < (int)h;y++)
			memcpy(Y+(Ystride*y),Y,Ystride);
		for (y=1;y < (int)(h/2);y++) {
			memcpy(U+(Ustride*y),U,Ustride);
			memcpy(V+(Vstride*y),V,Vstride);
		}
	}
	else if (client_area_image) {
		size_t stride = client_area_image->bytes_per_line;
		size_t visible_x=0,visible_y=0,visible,visible_row_end,visible_row;

		/* draw in the computed visible area, not the whole frame */
		h = (unsigned int)client_area_video_h;
		w = (unsigned int)client_area_video_w;
		visible_row = (client_area_video_y * stride);
		visible_row_end = visible_row + (stride * h);

		if (client_area_video_y != 0)
			memset(client_area_image->data,0,stride * client_area_video_y);
		if ((client_area_video_y+client_area_video_h) < client_area_height)
			memset(client_area_image->data+((client_area_video_y+client_area_video_h)*stride),0,stride *
					(client_area_height-(client_area_video_y+client_area_video_h)));

		if (client_area_image->bits_per_pixel == 32) {
			row = (unsigned char*)client_area_image->data + visible_row;

			/* black padding */
			for (x=0;x < client_area_video_x;x++) {
				*row++ = 0;
				*row++ = 0;
				*row++ = 0;
				*row++ = 0;
			}

			/* generate colorbars */
			x = 0;
			p = row;
			for (cc=1;cc <= 8;cc++) {
				a = smpte_bars[(cc-1)*3];
				b = smpte_bars[(cc-1)*3 + 1];
				c = smpte_bars[(cc-1)*3 + 2];
				y = yuv_to_rgb24(a,b,c);
				if (client_area_image->blue_mask == 0xFF) {
					a = y & 0xFF;
					b = y >> 8;
					c = y >> 16;
					d = y >> 24;
				}
				else {
					c = y & 0xFF;
					b = y >> 8;
					a = y >> 16;
					d = y >> 24;
				}
				y = (cc * w) / 8;
				while (x < y) {
					*p++ = a;
					*p++ = b;
					*p++ = c;
					*p++ = d;
					x++;
				}
			}

			x += client_area_video_x;
			while (x < client_area_width) {
				*p++ = 0;
				*p++ = 0;
				*p++ = 0;
				*p++ = 0;
				x++;
			}

			for (y=1;y < (int)h;y++)
				memcpy(client_area_image->data+visible_row+(y*stride),client_area_image->data+visible_row,stride);
		}
		else if (client_area_image->bits_per_pixel == 24) {
			row = (unsigned char*)client_area_image->data + visible_row;

			/* black padding */
			for (x=0;x < client_area_video_x;x++) {
				*row++ = 0;
				*row++ = 0;
				*row++ = 0;
			}

			/* generate colorbars */
			x = 0;
			p = row;
			for (cc=1;cc <= 8;cc++) {
				a = smpte_bars[(cc-1)*3];
				b = smpte_bars[(cc-1)*3 + 1];
				c = smpte_bars[(cc-1)*3 + 2];
				y = yuv_to_rgb24(a,b,c);
				if (client_area_image->blue_mask == 0xFF) {
					a = y & 0xFF;
					b = y >> 8;
					c = y >> 16;
				}
				else {
					c = y & 0xFF;
					b = y >> 8;
					a = y >> 16;
				}
				y = (cc * w) / 8;
				while (x < y) {
					*p++ = a;
					*p++ = b;
					*p++ = c;
					x++;
				}
			}

			x += client_area_video_x;
			while (x < client_area_width) {
				*p++ = 0;
				*p++ = 0;
				*p++ = 0;
				x++;
			}

			for (y=1;y < (int)h;y++)
				memcpy(client_area_image->data+visible_row+(y*stride),client_area_image->data+visible_row,stride);
		}
		else if (client_area_image->bits_per_pixel == 16) {
			row = (unsigned char*)client_area_image->data + visible_row;

			/* black padding */
			for (x=0;x < client_area_video_x;x++) {
				*row++ = 0;
				*row++ = 0;
			}

			/* generate colorbars */
			x = 0;
			p = row;
			for (cc=1;cc <= 8;cc++) {
				a = smpte_bars[(cc-1)*3];
				b = smpte_bars[(cc-1)*3 + 1];
				c = smpte_bars[(cc-1)*3 + 2];
				y = yuv_to_rgb24(a,b,c);
				if (client_area_image->green_mask == (0x3F << 5)) { /* 5:6:5 */
					if (client_area_image->blue_mask == 0x1F)
						aa = ((y & 0xF8) >> 3) | (((y & 0xFC00) >> (8+2)) << 5) | (((y & 0xF80000) >> (16+3)) << 11);
					else
						aa = (((y & 0xF8) >> 3) << 11) | (((y & 0xFC00) >> (8+2)) << 5) | ((y & 0xF80000) >> (16+3));
				}
				else { /* 5:5:5 */
					if (client_area_image->blue_mask == 0x1F)
						aa = ((y & 0xF8) >> 3) | (((y & 0xF800) >> (8+3)) << 5) | (((y & 0xF80000) >> (16+3)) << 10);
					else
						aa = (((y & 0xF8) >> 3) << 10) | (((y & 0xF800) >> (8+3)) << 5) | ((y & 0xF80000) >> (16+3));
				}

				y = (cc * w) / 8;
				while (x < y) {
					*p++ = aa;
					*p++ = aa >> 8;
					x++;
				}
			}

			x += client_area_video_x;
			while (x < client_area_width) {
				*p++ = 0;
				*p++ = 0;
				x++;
			}

			for (y=1;y < (int)h;y++)
				memcpy(client_area_image->data+visible_row+(y*stride),client_area_image->data+visible_row,stride);
		}

	}
}

void vga_font_draw_xv(unsigned int x,unsigned int y,unsigned char c,unsigned int dsh) {
	size_t dYstride = client_area_xvimage->pitches[client_area_y_plane_index];
	unsigned char *dY = (unsigned char*)client_area_xvimage->data + client_area_xvimage->offsets[client_area_y_plane_index];
	size_t dUstride = client_area_xvimage->pitches[client_area_u_plane_index];
	unsigned char *dU = (unsigned char*)client_area_xvimage->data + client_area_xvimage->offsets[client_area_u_plane_index];
	size_t dVstride = client_area_xvimage->pitches[client_area_v_plane_index];
	unsigned char *dV = (unsigned char*)client_area_xvimage->data + client_area_xvimage->offsets[client_area_v_plane_index];
	unsigned char *bmp = vga_80x25_font + ((unsigned int)c * 16);
	unsigned int brp,br,brn,msk,msk2;
	unsigned int row,col;

	if (dsh == 1) {
		br = 0;
		brn = *bmp++;
		for (row=0;row < 32;row += 2) {
			brp = br;
			br = brn;
			brn = *bmp++;

			unsigned char *drawY = dY + (dYstride * (row+y)) + x;
			unsigned char *drawU = dU + (dUstride * ((row+y)>>1)) + (x>>1);
			unsigned char *drawV = dV + (dVstride * ((row+y)>>1)) + (x>>1);
			for (col=0,msk=0x100,msk2=(0x80|0x100|0x200);col < 10;col++,msk>>=1,msk2>>=1) {
				if (br & msk) drawY[0] = drawY[1] = drawY[dYstride] = drawY[dYstride+1] = 235;
				else if ((brp|br|brn)&msk2) drawY[0] = drawY[1] = drawY[dYstride] = drawY[dYstride+1] = 16;
				drawY += 2;

				/* chroma */
				if ((br&(msk|msk2))|((brp|brn)&msk2)) *drawU++ = *drawV++ = 128;
				else drawU++,drawV++;
			}
		}
	}
	else {
		br = 0;
		brn = *bmp++;
		for (row=0;row < 16;row++) {
			brp = br;
			br = brn;
			brn = *bmp++;

			unsigned char *drawY = dY + (dYstride * (row+y)) + x;
			unsigned char *drawU = dU + (dUstride * ((row+y)>>1)) + (x>>1);
			unsigned char *drawV = dV + (dVstride * ((row+y)>>1)) + (x>>1);
			for (col=0,msk=0x100,msk2=(0x80|0x100|0x200);col < 10;col++,msk>>=1,msk2>>=1) {
				if (br & msk) *drawY++ = 235;
				else if ((brp|br|brn)&msk2) *drawY++ = 16;
				else drawY++;

				if (((col|(row+y))&1) == 0) { /* chroma */
					if ((br&(msk|msk2))|((brp|brn)&msk2)) *drawU++ = *drawV++ = 128;
					else drawU++,drawV++;
				}
			}
		}
	}
}

void vga_font_draw_rgb(unsigned int x,unsigned int y,unsigned char c,unsigned int dsh) {
	unsigned char *bmp = vga_80x25_font + ((unsigned int)c * 16);
	unsigned int brp,br,brn,msk,msk2;
	unsigned int row,col;

	if (client_area_image->bits_per_pixel == 32) {
		const uint32_t white = 0xFFFFFF,black = 0;

		br = 0;
		brn = *bmp++;
		for (row=0;row < (16<<dsh);row++) {
			if ((row&((1<<dsh)-1)) == 0) {
				brp = br;
				br = brn;
				brn = *bmp++;
			}

			uint32_t *draw = ((uint32_t*)((char*)client_area_image->data+((y+row)*client_area_image->bytes_per_line)))+x;
			for (col=0,msk=0x100,msk2=(0x80|0x100|0x200);col < 10;col++,msk>>=1,msk2>>=1) {
				unsigned char rep = 1<<dsh;

				do {
					if (br & msk) *draw++ = white;
					else if ((brp|br|brn)&msk2) *draw++ = black;
					else draw++;
				} while (--rep != 0);
			}
		}
	}
	else if (client_area_image->bits_per_pixel == 24) {
	}
	else if (client_area_image->bits_per_pixel == 16) {
	}
}

void vga_font_draw(unsigned int x,unsigned int y,unsigned char c,unsigned int dsh) {
	if (client_area_xvideo)
		vga_font_draw_xv(x,y,c,dsh);
	else
		vga_font_draw_rgb(x,y,c,dsh);
}

/* draw the "on-screen display" text */
void draw_osd() {
	const char *src = osd_string.c_str();
	int x,y,draw_x,draw_y,w,h,ox,oy,dsh,mw;
	char c;

	if (osd_state != OSD_OFF && user_enable_osd) {
		if (client_area_xvideo) {
			w = source_image_width;
			h = source_image_height;
			ox = oy = 0;
			mw = w;
		}
		else {
			w = client_area_video_w;
			h = client_area_video_h;
			ox = client_area_video_x;
			oy = client_area_video_y;
			mw = client_area_width-ox;
		}

		/* large text if OSD_BIG, or the display area is so large the small version would be unreadable */
		dsh = ((osd_state == OSD_BIG) || (w > 800 && h > 600)) ? 1 : 0;

		/* decide where to put it */
		x = (w / 10); /* 10% 10% from upper left hand corner */
		y = (h / 10);

		draw_x = x;
		draw_y = y;
		while ((c = *src++) != 0) { /* scan ASCIIZ string and draw it out left-to-right */
			/* abort if we're beyond the screen, unless you want memory corruption or a segfault */
			if ((draw_y+(16<<dsh)) > h) break;

			if (c == '\n') {
				draw_x = x;
				draw_y += 16<<dsh;
			}
			else if (c >= 32 || c < 0) {
				if ((draw_x+(8<<dsh)) < mw) {
					vga_font_draw(draw_x+ox,draw_y+oy,c,dsh);
					draw_x += 8<<dsh;
				}
			}
		}
	}
}

static unsigned char temp_avi_read[2*1024*1024];
static bool temp_avi_frame_valid = false;
static AVFrame *temp_avi_frame = NULL;

bool put_temp_avi_frame_on_screen(InputManager *input) {
	bool ret = false;

	if (temp_avi_frame != NULL && temp_avi_frame->width != 0 && temp_avi_frame->height != 0) {
		if (input->video_avcodec_ctx->pix_fmt == AV_PIX_FMT_YUV420P) {
			client_area_check_source_size(temp_avi_frame->width,temp_avi_frame->height);

			if (client_area_xvimage) {
				assert(source_image_width >= temp_avi_frame->width);
				assert(source_image_height >= temp_avi_frame->height);
				unsigned int y;

				size_t dYstride = client_area_xvimage->pitches[client_area_y_plane_index];
				unsigned char *dY = (unsigned char*)client_area_xvimage->data + client_area_xvimage->offsets[client_area_y_plane_index];

				size_t dUstride = client_area_xvimage->pitches[client_area_u_plane_index];
				unsigned char *dU = (unsigned char*)client_area_xvimage->data + client_area_xvimage->offsets[client_area_u_plane_index];

				size_t dVstride = client_area_xvimage->pitches[client_area_v_plane_index];
				unsigned char *dV = (unsigned char*)client_area_xvimage->data + client_area_xvimage->offsets[client_area_v_plane_index];

				for (y=0;y < temp_avi_frame->height;y++)
					memcpy(dY+(y*dYstride),temp_avi_frame->data[0]+(y*temp_avi_frame->linesize[0]),min((size_t)temp_avi_frame->linesize[0],dYstride));

				for (y=0;y < (temp_avi_frame->height/2);y++)
					memcpy(dU+(y*dUstride),temp_avi_frame->data[1]+(y*temp_avi_frame->linesize[1]),min((size_t)temp_avi_frame->linesize[1],dUstride));

				for (y=0;y < (temp_avi_frame->height/2);y++)
					memcpy(dV+(y*dVstride),temp_avi_frame->data[2]+(y*temp_avi_frame->linesize[2]),min((size_t)temp_avi_frame->linesize[2],dVstride));

				ret = true;
			}
			else if (client_area_image) {
				if (client_area_image->bits_per_pixel == 32) {
					yv12_to_client_area_rgb32(temp_avi_frame->data[0],temp_avi_frame->linesize[0],
							temp_avi_frame->data[1],temp_avi_frame->linesize[1],
							temp_avi_frame->data[2],temp_avi_frame->linesize[2],1);
					ret = true;
				}
				else if (client_area_image->bits_per_pixel == 24) {
					yv12_to_client_area_rgb24(temp_avi_frame->data[0],temp_avi_frame->linesize[0],
							temp_avi_frame->data[1],temp_avi_frame->linesize[1],
							temp_avi_frame->data[2],temp_avi_frame->linesize[2],1);
					ret = true;
				}
				else if (client_area_image->bits_per_pixel == 16) {

					/* it depends: is this 5:5:5 RGB or 5:6:5 RGB? */
					if (client_area_image->green_mask == (0x3F << 5))
						yv12_to_client_area_rgb16_565(temp_avi_frame->data[0],temp_avi_frame->linesize[0],
								temp_avi_frame->data[1],temp_avi_frame->linesize[1],
								temp_avi_frame->data[2],temp_avi_frame->linesize[2],1);
					else
						yv12_to_client_area_rgb16_555(temp_avi_frame->data[0],temp_avi_frame->linesize[0],
								temp_avi_frame->data[1],temp_avi_frame->linesize[1],
								temp_avi_frame->data[2],temp_avi_frame->linesize[2],1);

					ret = true;
				}
			}
		}
		else if (input->video_avcodec_ctx->pix_fmt == AV_PIX_FMT_YUV422P) {
			client_area_check_source_size(temp_avi_frame->width,temp_avi_frame->height);

			if (client_area_xvimage) {
				assert(source_image_width >= temp_avi_frame->width);
				assert(source_image_height >= temp_avi_frame->height);
				unsigned int y;

				size_t dYstride = client_area_xvimage->pitches[client_area_y_plane_index];
				unsigned char *dY = (unsigned char*)client_area_xvimage->data + client_area_xvimage->offsets[client_area_y_plane_index];

				size_t dUstride = client_area_xvimage->pitches[client_area_u_plane_index];
				unsigned char *dU = (unsigned char*)client_area_xvimage->data + client_area_xvimage->offsets[client_area_u_plane_index];

				size_t dVstride = client_area_xvimage->pitches[client_area_v_plane_index];
				unsigned char *dV = (unsigned char*)client_area_xvimage->data + client_area_xvimage->offsets[client_area_v_plane_index];

				for (y=0;y < temp_avi_frame->height;y++)
					memcpy(dY+(y*dYstride),temp_avi_frame->data[0]+(y*temp_avi_frame->linesize[0]),min((size_t)temp_avi_frame->linesize[0],dYstride));

				for (y=0;y < temp_avi_frame->height;y++)
					memcpy(dU+((y>>1)*dUstride),temp_avi_frame->data[1]+(y*temp_avi_frame->linesize[1]),min((size_t)temp_avi_frame->linesize[1],dUstride));

				for (y=0;y < temp_avi_frame->height;y++)
					memcpy(dV+((y>>1)*dVstride),temp_avi_frame->data[2]+(y*temp_avi_frame->linesize[2]),min((size_t)temp_avi_frame->linesize[2],dVstride));

				ret = true;
			}
			else if (client_area_image) {
				if (client_area_image->bits_per_pixel == 32) {
					yv12_to_client_area_rgb32(temp_avi_frame->data[0],temp_avi_frame->linesize[0],
							temp_avi_frame->data[1],temp_avi_frame->linesize[1],
							temp_avi_frame->data[2],temp_avi_frame->linesize[2],0);
					ret = true;
				}
				else if (client_area_image->bits_per_pixel == 24) {
					yv12_to_client_area_rgb24(temp_avi_frame->data[0],temp_avi_frame->linesize[0],
							temp_avi_frame->data[1],temp_avi_frame->linesize[1],
							temp_avi_frame->data[2],temp_avi_frame->linesize[2],0);
					ret = true;
				}
				else if (client_area_image->bits_per_pixel == 16) {

					/* it depends: is this 5:5:5 RGB or 5:6:5 RGB? */
					if (client_area_image->green_mask == (0x3F << 5))
						yv12_to_client_area_rgb16_565(temp_avi_frame->data[0],temp_avi_frame->linesize[0],
								temp_avi_frame->data[1],temp_avi_frame->linesize[1],
								temp_avi_frame->data[2],temp_avi_frame->linesize[2],0);
					else
						yv12_to_client_area_rgb16_555(temp_avi_frame->data[0],temp_avi_frame->linesize[0],
								temp_avi_frame->data[1],temp_avi_frame->linesize[1],
								temp_avi_frame->data[2],temp_avi_frame->linesize[2],0);

					ret = true;
				}
			}
		}
	}

	return ret;
}

void InputManager::codec_check() {
	if (video_avcodec == NULL) {
		/* TODO: Support other codecs, if the AVI capture program is using them */
		close_avcodec();
		video_avcodec = avcodec_find_decoder(AV_CODEC_ID_H264);
		if (video_avcodec != NULL) {
			video_avcodec_ctx = avcodec_alloc_context3(video_avcodec);
			if (video_avcodec_ctx != NULL) {
				avcodec_get_context_defaults3(video_avcodec_ctx,video_avcodec);
				video_avcodec_ctx->thread_count = how_many_cpus();
				video_avcodec_ctx->thread_type = FF_THREAD_SLICE;
				/* NTS: do NOT set FF_THREAD_FRAME because this code cannot handle it,
				 *      it causes FFMPEG to buffer frames according to the number of threads,
				 *      and delay them that much, when this code assumes that there is
				 *      no delay. This code should be updated to properly handle delayed frames,
				 *      so that eventually we can capture with B-frames and still allow
				 *      random access. */
//				video_avcodec_ctx->thread_type |= FF_THREAD_FRAME;
				video_avcodec_ctx->flags2 |= CODEC_FLAG2_FAST;
				if (avcodec_open2(video_avcodec_ctx,video_avcodec,NULL)) {
					fprintf(stderr,"Failed to open H264 codec\n");
					av_free(video_avcodec_ctx);
					video_avcodec_ctx = NULL;
					video_avcodec = NULL;
				}
				else {
					fprintf(stderr,"Video codec opened\n");
				}
			}
			else {
				fprintf(stderr,"Cannot alloc ffmpeg context\n");
			}
		}
		else {
			fprintf(stderr,"FFMPEG could not locate the MPEG-4 decoder\n");
		}
	}
}

bool put_play_frame_on_screen(InputManager *input,bool force_redraw) {
	signed long long target_frame,actual_target_frame,scan;
	size_t patience = 1000;
	bool ret = false;

	update_now_time();
	if (input->Playing || input->index == VIEW_INPUT_FILE) {
		video_tracking *trk = (input->play_is_rec ? &input->vt_rec : &input->vt_play);
		int fd;

		if (trk->video_rate_n == 0 || trk->video_rate_d == 0)
			return false;

		if (input->play_is_rec) {
			if (input->capture_avi_fd < 0 && input->capture_avi != "") {
				input->capture_avi_fd = open(input->capture_avi.c_str(),O_RDONLY);
				fprintf(stderr,"PLAY==REC reopening capture '%s'\n",input->capture_avi.c_str());
			}

			fd = input->capture_avi_fd;
		}
		else {
			if (input->play_avi_fd < 0 && input->play_avi != "") {
				input->play_avi_fd = open(input->play_avi.c_str(),O_RDONLY);
				fprintf(stderr,"PLAY!=REC reopening play '%s'\n",input->play_avi.c_str());
			}

			fd = input->play_avi_fd;
		}

		input->codec_check();
		actual_target_frame = target_frame = input->time_to_target_frame();
		if (trk->video_first_frame >= 0LL && target_frame < trk->video_first_frame)
			target_frame = trk->video_first_frame;
		else if (target_frame < 0LL)
			target_frame = 0LL;

		if (force_redraw)
			fprintf(stderr,"forceredraw %lld != %lld of %lld\n",
				trk->video_current_frame,target_frame,trk->video_total_frames);

		if (target_frame != trk->video_current_frame || (input->play_is_rec && input->Recording)) {
			char tmp[256];
			const char *name;
			unsigned int H,M,S,mS;
			unsigned long long tm;
			unsigned int tH,tM,tS,tmS;

			assert(trk->video_rate_n != 0);
			assert(trk->video_rate_d != 0);

			if (input->play_is_rec)
				name = strrchr(input->capture_avi.c_str(),'/');
			else
				name = strrchr(input->play_avi.c_str(),'/');

			if (name == NULL) name = "";
			else if (*name == '/') name++;

			tm = ((target_frame * 1000LL) * trk->video_rate_d) / trk->video_rate_n;
			mS = (unsigned int)(tm % 1000LL);
			S = (unsigned int)((tm / 1000LL) % 60ULL);
			M = (unsigned int)((tm / 1000LL / 60LL) % 60ULL);
			H = (unsigned int)(tm / 1000LL / 3600LL);

			tm = ((trk->video_total_frames * 1000LL) * trk->video_rate_d) / trk->video_rate_n;
			tmS = (unsigned int)(tm % 1000LL);
			tS = (unsigned int)((tm / 1000LL) % 60ULL);
			tM = (unsigned int)((tm / 1000LL / 60LL) % 60ULL);
			tH = (unsigned int)(tm / 1000LL / 3600LL);

			snprintf(tmp,sizeof(tmp),"Playing %u:%02u:%02u.%03u out of %u:%02u:%02u.%03u %s",H,M,S,mS,tH,tM,tS,tmS,name);
			gui_status(tmp);
		}

		if (actual_target_frame < 0 && input->playback_speed_n < 0) {
			fprintf(stderr,"frame<=0 backwards\n");
			if (input->index == VIEW_INPUT_FILE) {
				input->pause_playback();
				update_ui_controls();
			}
			else if (input->play_is_rec) {
				input->pause_playback();
				update_ui_controls();
			}
			else {
				/* TODO: Jump to (chronologically) previous AVI */
				input->pause_playback();
				update_ui_controls();
			}
		}
		else if (actual_target_frame > trk->video_total_frames && input->playback_speed_n > 0 && !(input->Recording && input->play_is_rec && input->playback_speed_n == 1 && input->playback_speed_d == 1)) {
			fprintf(stderr,"frame>=total\n");
			/* on the file input source, just stop at the end. the play start code will restart back
			 * to the beginning if the user presses play */
			if (input->index == VIEW_INPUT_FILE) {
				input->pause_playback();
				update_ui_controls();
			}
			/* if the playback file IS the capture file, and we're recording, switch back to "stop" mode */
			else if (input->play_is_rec) {
				input->stop_playback();
				update_ui_controls();
				fprintf(stderr,"PLAY==REC, hit the end, stopping\n");
			}
			else { /* else, switch to the (chronologically) next AVI file, or to "stop" if no more */
				/* we exausted captured AVIs, jump to the AVI capturing NOW */
				if (input->capture_avi != "" && input->play_avi != input->capture_avi) {
					/* to capture AVI */
					input->play_avi = "";
					input->close_play_avi();
					input->vt_play.clear();
					input->play_avi = input->capture_avi;
					input->play_is_rec = true;
					input->vt_rec.video_current_frame = -1LL;
					input->playback_base_frame = 0LL;
					input->playback_base_time = NOW;

					{
						string tmp = string(CurrentInputObj()->osd_name) + "\nPlay: ";
						const char *s = strrchr(CurrentInputObj()->play_avi.c_str(),'/');
						if (s != NULL) s++;
						else s = CurrentInputObj()->play_avi.c_str();
						tmp += s;
						osd_set_text_big(tmp.c_str());
					}
				}
				/* or just stop entirely */
				else {
					input->play_avi = "";
					input->close_play_avi();
					input->vt_play.clear();
					input->stop_playback();
					fprintf(stderr,"Switching back to stop mode\n");
				}
				update_ui_controls();
			}
		}
		/* TODO: You can do better than this: buffer frames up to a limit, and if asked to play backwards, just replay them instead of wasting the decoder's time */
		else if (((trk->video_current_frame < target_frame && input->playback_speed_n > 0) ||
			((trk->video_current_frame > target_frame || trk->video_current_frame < 0LL) && input->playback_speed_n < 0)) &&
			trk->video_current_frame < trk->video_total_frames &&
			trk->video_current_frame < (signed long long)trk->video_index.size() &&
			trk->video_total_frames > 0LL && trk->video_current_frame != target_frame) {

			if (target_frame >= (signed long long)trk->video_index.size())
				target_frame = (signed long long)trk->video_index.size() - 1LL;

			if (target_frame < trk->video_current_frame || target_frame > (trk->video_current_frame+60UL) || input->playback_speed_n < 0) {
				/* jump to target, scan back to keyframe */
				scan = target_frame;
				if (scan >= trk->video_index.size()) scan = trk->video_index.size() - 1;
				while (scan > 0LL && patience != 0 && scan < trk->video_index.size() &&
					trk->video_index[scan].keyframe == 0) {
					patience--;
					scan--;
				}

				trk->video_current_frame = scan - 1LL;
			}

			/* then decode forward */
			temp_avi_frame_valid = false;
			while (trk->video_current_frame < target_frame && patience != 0) {
				if (trk->video_current_frame < 0LL)
					trk->video_current_frame = 0LL;
				else
					trk->video_current_frame++;

				struct video_tracking::index_entry &ie =
					trk->video_index[trk->video_current_frame];

				/* locate the chunk, read, and decode */
//				fprintf(stderr,"[%lld/%lld] sz=%lu @ %lld\n",trk->video_current_frame,target_frame,ie.size,ie.offset);
				if (ie.size != 0UL && ie.offset != 0ULL && (ie.size+1024) < sizeof(temp_avi_read)) {
					int rd,gotit=0;

					lseek(fd,(signed long long)ie.offset,SEEK_SET);
					rd = read(fd,temp_avi_read,ie.size);
					if (rd < 0) rd = 0;
					assert((rd+1024) < sizeof(temp_avi_read));
					memset(temp_avi_read+rd,0,1023);

					if (temp_avi_frame == NULL)
						temp_avi_frame = av_frame_alloc();
					if (temp_avi_frame != NULL && input->video_avcodec_ctx != NULL) {
						AVPacket pkt={0};
						pkt.data = temp_avi_read;
						pkt.size = rd;
						pkt.dts = pkt.dts = trk->video_current_frame;

						if (avcodec_decode_video2(input->video_avcodec_ctx,temp_avi_frame,&gotit,&pkt) > 0 && gotit != 0) {
							temp_avi_frame_valid = true;
							ret = true;
						}
					}
				}

				patience--;
			}
		}

		if (ret || force_redraw) {
			if (ret=put_temp_avi_frame_on_screen(input))
				client_area_redraw_source_frame();
		}
	}

	return ret;
}

bool do_video_source(InputManager *input,bool force_redraw) {
	bool ret;

	if (input->Playing || input->index == VIEW_INPUT_FILE) ret = put_play_frame_on_screen(input,force_redraw);
	else ret = put_live_frame_on_screen(input,force_redraw);

	if (ret) draw_osd();

	return ret;
}

void switch_input(int x) {
	if (CurrentInput != x) {
		if (CurrentInputObj()->Playing)
			CurrentInputObj()->pause_playback();

		/* FIXME: This is for the audio dialog box. we should make this a more general "on switch" hook call system */
		{
			bool reopen;

			reopen = update_vars_from_audio_dialog();
			if (reopen) {
				if (pthread_mutex_lock(&global_mutex) == 0) {
					CurrentInputObj()->reopen_input();
					pthread_mutex_unlock(&global_mutex);
				}
			}
		}

		CurrentInputObj()->onActivate(0);
		CurrentInput = x;
		CurrentInputObj()->onActivate(1);

		/* and the audio dialog might want to update from the new input */
		update_audio_dialog_from_vars();
		update_input_dialog_from_vars();

		if (pthread_mutex_lock(&global_mutex) == 0) {
			client_area_get_aspect_from_current_input();
			client_area_update_rects_again();
			if (video_should_redraw_t < 0)
				video_should_redraw_t = NOW + 0.25;

			{
				string tmp = string(CurrentInputObj()->osd_name);
				if (CurrentInputObj()->Recording) {
					const char *s = strrchr(CurrentInputObj()->capture_avi.c_str(),'/');
					if (s != NULL) s++;
					else s = CurrentInputObj()->capture_avi.c_str();
					tmp += string("\nRec: ") + string(s);
				}
				if (CurrentInputObj()->Playing) {
					if (CurrentInputObj()->Paused)
						tmp += string("\nPause: ");
					else
						tmp += string("\nPlay: ");

					const char *s = strrchr(CurrentInputObj()->play_avi.c_str(),'/');
					if (s != NULL) s++;
					else s = CurrentInputObj()->play_avi.c_str();
					tmp += string(s);
				}

				osd_set_text_big(tmp.c_str());
			}

			if (!do_video_source(CurrentInputObj(),true)) {
				generate_non_video_frame();
				draw_osd();
			}
			client_area_redraw_source_frame();

			pthread_mutex_unlock(&global_mutex);
		}
	}
}

int ui_ask_stop_recording() {
	int response;
	GtkWidget *dialog;

	dialog = gtk_message_dialog_new (GTK_WINDOW (main_window),
			GTK_DIALOG_DESTROY_WITH_PARENT,
			GTK_MESSAGE_QUESTION,
			GTK_BUTTONS_YES_NO,
			"Stop recording?");

	gtk_widget_show (dialog);
	response = gtk_dialog_run (GTK_DIALOG(dialog));
	gtk_widget_destroy (dialog);

	return (response == GTK_RESPONSE_YES);
}

string getcwd_cpp() {
	char tmp[PATH_MAX];

	tmp[0] = 0;
	getcwd(tmp,PATH_MAX);
	return tmp;
}

bool InputManager::reopen_input() {
	bool restart_process = (cap_pid >= 0)?true:false;
	bool ok = true;

	close_capture_avi();
	shutdown_process();
	if (restart_process) start_process();

	return ok;
}

bool InputManager::start_process() {
	struct stat st;
	struct sockaddr_un un;
	char tmp[256],param_1[64],param_2[64];
	const char *argv[256];
	string cwd;
	int argc;

	if (cap_pid >= 0)
		return true;
	if (video_index < 0)
		return false;

	cwd = getcwd_cpp();

	/* picked the shared memory segment. the program will create it and we
	 * will map it when it appears */
	assert(shmem_fd < 0);
	assert(shmem == NULL);
	sprintf(tmp,"videocap.%lu.input.%lu",
		(unsigned long)getpid(),
		(unsigned long)index);
	shmem_name = tmp;

	/* create the socket */
	sprintf(tmp,"/.capture.socket.%lu",(unsigned long)index);
	socket_path = cwd + tmp;
	assert(socket_svr_fd < 0);
	socket_svr_fd = socket(AF_UNIX,SOCK_SEQPACKET,0);
	if (socket_svr_fd < 0) {
		fprintf(stderr,"Cannot create server socket\n");
		goto fail;
	}
	socket_fd = -1; /* this will be assigned when the client process connects */

	/* bind the socket, then listen */
	memset(&un,0,sizeof(un));
	un.sun_family = AF_UNIX;
	if ((socket_path.length()+2) >= sizeof(un.sun_path)) {
		fprintf(stderr,"Path too long\n");
		goto fail;
	}
	strcpy(un.sun_path,socket_path.c_str());
	if (stat(socket_path.c_str(),&st) == 0 && S_ISSOCK(st.st_mode))
		unlink(socket_path.c_str());
	if (bind(socket_svr_fd,(struct sockaddr*)(&un),sizeof(un))) {
		fprintf(stderr,"Cannot bind socket, %s\n",strerror(errno));
		goto fail;
	}
	if (listen(socket_svr_fd, 2) < 0) {
		fprintf(stderr,"Cannot make socket listen\n");
		goto fail;
	}

	/* all right start the process */
	argc = 0;
	argv[argc++] = V4L_CAP_PROGRAM;

	argv[argc++] = "--path";
	argv[argc++] = cwd.c_str();

	argv[argc++] = "--shm";
	argv[argc++] = shmem_name.c_str();

	argv[argc++] = "--socket";
	argv[argc++] = socket_path.c_str();

	sprintf(param_1,"%u",video_index);
	argv[argc++] = "--index";
	argv[argc++] = param_1;

	argv[argc++] = "--auto-open-avi";
	argv[argc++] = "0";

	argv[argc++] = "--auto-open-v4l";
	argv[argc++] = "1";

	if (enable_vbi) {
		argv[argc++] = "--vbi";
	}

	if (enable_audio) {
		argv[argc++] = "--audio-device";
		argv[argc++] = audio_device.empty() ? "auto" : audio_device.c_str();
	}

	if (!input_device.empty()) {
		argv[argc++] = "--input-device";
		argv[argc++] = input_device.c_str();
	}

	if (!input_standard.empty()) {
		argv[argc++] = "--input-standard";
		argv[argc++] = input_standard.c_str();
	}

	if (!vcrhack.empty()) {
		argv[argc++] = "--vcr-hack";
		argv[argc++] = vcrhack.c_str();
	}

	if (!input_codec.empty()) {
		argv[argc++] = "--codec";
		argv[argc++] = input_codec.c_str();
	}

	char param_fps[64];
	if (capture_fps > 0) {
		sprintf(param_fps,"%.3f",capture_fps);
		argv[argc++] = "--fps";
		argv[argc++] = param_fps;
	}

	char param_w[64];
	if (capture_width > 0) {
		sprintf(param_w,"%d",capture_width);
		argv[argc++] = "--width";
		argv[argc++] = param_w;
	}

	char param_h[64];
	if (capture_height > 0) {
		sprintf(param_h,"%d",capture_height);
		argv[argc++] = "--height";
		argv[argc++] = param_h;
	}

	char crop_l[64],crop_t[64],crop_w[64],crop_h[64];
	if (crop_bounds)
		argv[argc++] = "--cropbound";
	else if (crop_defrect)
		argv[argc++] = "--cropdef";
	else {
		if (crop_left != CROP_DEFAULT) {
			sprintf(crop_l,"%d",(int)crop_left);
			argv[argc++] = "--cx";
			argv[argc++] = crop_l;
		}
		if (crop_top != CROP_DEFAULT) {
			sprintf(crop_t,"%d",(int)crop_top);
			argv[argc++] = "--cy";
			argv[argc++] = crop_t;
		}
		if (crop_left != CROP_DEFAULT) {
			sprintf(crop_w,"%d",(int)crop_width);
			argv[argc++] = "--cw";
			argv[argc++] = crop_w;
		}
		if (crop_left != CROP_DEFAULT) {
			sprintf(crop_h,"%d",(int)crop_height);
			argv[argc++] = "--ch";
			argv[argc++] = crop_h;
		}
	}

	argv[argc++] = "--fpre";
	argv[argc++] = file_prefix;

	argv[argc  ] = NULL;

	/* OK go */
	cap_pid = fork();
	if (cap_pid < 0) {
		fprintf(stderr,"Fork failed\n");
		return false;
	}
	else if (cap_pid == 0) { /* child process */
		execv(argv[0],(char *const *)argv);
		_exit(1);
	}

	/* parent process */
	return true;
fail:	shutdown_process();
	return false;
}

void InputManager::idle() {
	struct stat st;
	int status;

	if (cap_pid >= 0) {
		/* catch the shutdown of the process */
		if (waitpid(cap_pid,&status,WNOHANG) == cap_pid) {
			/* the process shutdown by itself. reap the process */
			fprintf(stderr,"Capture process suddenly shutdown (status=0x%08X)\n",status);

			cap_pid = -1;
			close_socket();
			close_shmem();

			stop_recording();
			update_ui_controls();
		}
		else {
			/* has the process connected to our socket yet? */
			if (socket_svr_fd >= 0) {
				if (socket_fd < 0) {
					socket_fd = accept(socket_svr_fd,NULL,NULL);
					if (socket_fd >= 0) {
						fprintf(stderr,"Connected to capture program\n");
					}
				}
			}

			/* has the shared memory segment appeared? */
			if (shmem_fd < 0) {
				if ((shmem_fd = shm_open(shmem_name.c_str(),O_RDONLY,0)) >= 0) {
					st.st_size = 0;
					fstat(shmem_fd,&st);
					shmem_size = st.st_size;
					if (shmem_size < (1*1024*1024)) {
						close(shmem_fd);
						shmem_fd = -1;
					}
					else {
						shmem_out = 0;
						shmem = mmap(NULL,shmem_size,PROT_READ,MAP_SHARED,shmem_fd,0);
						if (shmem == MAP_FAILED) {
							close(shmem_fd);
							shmem_fd = -1;
							fprintf(stderr,"Shared memory segment appeared, but cannot map, sz=%lu %s\n",
								(unsigned long)shmem_size,
								strerror(errno));
						}
						else {
							fprintf(stderr,"Shared memory segment appeared (%u)\n",(unsigned)shmem_size);
						}
					}
				}
				else {
					fprintf(stderr,"Failed to open shmem %s\n",shmem_name.c_str());
				}
			}

			if (shmem != NULL) {
				volatile struct live_shm_header *xx =
					(volatile struct live_shm_header*)shmem;

				if (xx->header == LIVE_SHM_HEADER) {
					if (xx->slots != shmem_slots ||
						xx->width != shmem_width ||
						xx->height != shmem_height) {
						shmem_slots = xx->slots;
						shmem_width = xx->width;
						shmem_height = xx->height;
						shmem_out = 0;
					}
				}
			}
		}
	}

	/* OSD management */
	if (osd_next_state_t >= 0.0 && NOW >= osd_next_state_t) {
		osd_next_state_t = -1;
		osd_next_state_cb();
	}

	/* messages from the capture program */
	idle_socket();
}

bool InputManager::idle_socket() {
	bool ret = false;

	if (cap_pid >= 0 && socket_fd >= 0) {
		char *n;
		int rd = recv(socket_fd,sock_msg_tmp,sizeof(sock_msg_tmp)-1,MSG_DONTWAIT);
		if (rd > 0) {
			sock_msg_tmp[rd] = 0;
			ret = true;

			n = strchr(sock_msg_tmp,'\n');
			if (n) *n++ = 0; /* ASCIIZ snip */

			if (!strcmp(sock_msg_tmp,"INDEX-UPDATE") && n != NULL) {
				signed long long frame = -1LL,base = -1LL;
				signed long length = -1LL;
				bool keyframe = true;
				char *name,*value;

				while (*n != 0) {
					char *nn = strchr(n,'\n');
					if (nn) *nn++ = 0;
					else nn = n + strlen(n);

					while (*n == ' ') n++;
					name = n;

					char *equ = strchr(n,':');
					if (equ) {
						*equ++ = 0;
						value = equ;
					}
					else {
						value = NULL;
					}

//					fprintf(stderr,"   '%s' = '%s'\n",name,value);

					if (!strcasecmp(name,"frame"))
						frame = strtoll(value,NULL,0);
					else if (!strcasecmp(name,"base"))
						base = strtoll(value,NULL,0);
					else if (!strcasecmp(name,"length"))
						length = strtol(value,NULL,0);
					else if (!strcasecmp(name,"key"))
						keyframe = (bool)(atoi(value) > 0);

					n = nn;
				}

				if (frame >= (vt_rec.video_index.size()+10000LL)) {
					printf("Ingoring index entry, frame is too far into the future\n");
				}
				else if (frame >= 0LL && base >= 0LL) {
					struct video_tracking::index_entry entry = {0};

					while ((size_t)frame > vt_rec.video_index.size()) {
						vt_rec.video_index.push_back(entry);
					}

					if (vt_rec.video_first_frame == -1LL && keyframe)
						vt_rec.video_first_frame = frame;

					entry.offset = base;
					entry.size = length;
					entry.keyframe = keyframe;
					vt_rec.video_index.push_back(entry);
//					printf("Index[%lld] base=%lld len=%ld key=%u\n",
//						frame,base,length,keyframe);

					if (vt_rec.video_total_frames < frame)
						vt_rec.video_total_frames = frame;
				}
			}
			else {
				fprintf(stderr,"Unknown msg '%s'\n",sock_msg_tmp);
			}
		}
	}

	return ret;
}

void InputManager::shutdown_process() {
	if (cap_pid >= 0) {
		kill(cap_pid,SIGTERM);
		while (waitpid(cap_pid,NULL,0) != cap_pid) usleep(1000);
		cap_pid = -1;
	}

	close_socket();
	close_shmem();
}

void InputManager::close_socket() {
	if (socket_path != "") {
		unlink(socket_path.c_str());
		socket_path = "";
	}
	if (socket_svr_fd >= 0) {
		close(socket_svr_fd);
		socket_svr_fd = -1;
	}
	if (socket_fd >= 0) {
		close(socket_fd);
		socket_fd = -1;
	}
}

void InputManager::close_shmem() {
	if (shmem_fd >= 0) close(shmem_fd);
	shmem_fd = -1;

	if (shmem != NULL) munmap(shmem,shmem_size);
	shmem = NULL;

	if (shmem_name != "") shm_unlink(shmem_name.c_str());
	shmem_name = "";

	shmem_height = 0;
	shmem_width = 0;
	shmem_slots = 0;
	shmem_size = 0;
	shmem_out = 0;
}

InputManager::InputManager(int input_index) {
	video_avcodec_ctx = NULL;
	video_avcodec = NULL;
	play_is_rec = false;
	capture_avi_fd = -1;
	play_avi_fd = -1;
	Paused = false;
	Playing = false;
	Recording = false;
	osd_name[0] = 0;
	file_prefix[0] = 0;
	index = input_index;
	user_ar_n = user_ar_d = -1;
	capture_width = 0;
	capture_height = 0;
    capture_fps = 0;
	crop_defrect = false;
	crop_bounds = false;
	crop_left = crop_top = crop_width = crop_height = CROP_DEFAULT;
	enable_audio = false;
	enable_vbi = false;
	audio_device.clear();
	video_index = -1;
	source_ar_n = 4;
	source_ar_d = 3;
	cap_pid = -1;
	socket_path = "";
	socket_svr_fd = -1;
	socket_fd = -1;
	shmem_name = "";
	shmem_size = 0;
	shmem_out = 0;
	shmem_slots = 0;
	shmem_width = 0;
	shmem_height = 0;
	shmem_fd = -1;
	shmem = NULL;
}

void InputManager::close_capture_avi() {
	if (capture_avi_fd >= 0) {
		close(capture_avi_fd);
		capture_avi_fd = -1;
	}
}

void InputManager::close_play_avi() {
	if (play_avi_fd >= 0) {
		close(play_avi_fd);
		play_avi_fd = -1;
	}
}

void InputManager::close_avcodec() {
	if (video_avcodec_ctx) {
		avcodec_close(video_avcodec_ctx);
		av_free(video_avcodec_ctx);
		video_avcodec_ctx = NULL;
	}
	video_avcodec = NULL;
}

InputManager::~InputManager() {
	close_avcodec();
	close_capture_avi();
	close_play_avi();
	shutdown_process();
	close_socket();
	close_shmem();
}

void InputManager::socket_command(const char *msg) {
	int r,l;

	if (socket_fd >= 0) {
		l = (int)strlen(msg);
		do {
			r = send(socket_fd,msg,l,0);
		} while (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
		if (r == 0)
			fprintf(stderr,"Socket disconnect\n");
		else if (r < 0 && !(errno == EAGAIN || errno == EWOULDBLOCK))
			fprintf(stderr,"Socket error %s\n",strerror(errno));
		else if (r != l)
			fprintf(stderr,"Socket incomplete write (%d < %d)\n",r,l);
	}
}

void InputManager::onActivate(bool on) {
	if (on) {
		if (cap_pid >= 0) { /* if the process already started */
			if (socket_fd >= 0) {
				socket_command("capture-on");
			}
		}
		else if (video_index >= 0) {
			if (!start_process()) {
				fprintf(stderr,"Failed to start process\n");
				return;
			}
		}

		/* encourage our playback loop to re-decode to the temp_avi_frame */
		video_tracking *trk = (play_is_rec ? &vt_rec : &vt_play);
		trk->video_current_frame = -1LL;
	}
	else if (!Recording) {
		if (cap_pid >= 0 && socket_fd >= 0) {
			socket_command("capture-off");
		}
		else {
			shutdown_process();
			close_socket();
			close_shmem();
		}
	}
}

void InputManager::onRecord(bool on) {
	if (on != Recording) {
		if (on) start_recording();
		else if (ui_ask_stop_recording()) stop_recording();
		update_ui_controls();
	}
}

/******************************************************************************
 * gui_status
 *
 * Takes the string constant and puts it into the status bar at the bottom of
 * the main window. Replaces any previous text.
 *
 * Inputs: msg = const char * ascii string
 * Outputs: none
 * Returns: none
 * Scope: Global, process-wide
 * Global variables used: statusbar
 *
 * Notes: the GTK+ libraries consider it acceptable to call statusbar_pop()
 *        when nothing is on the stack. underflows are not an error.
 ******************************************************************************/
void gui_status(const char *msg) {
	gtk_statusbar_pop(GTK_STATUSBAR(statusbar), 0);
	gtk_statusbar_push(GTK_STATUSBAR(statusbar), 0, msg);
}

static void on_main_window_view_osd(GtkMenuItem *menuitem,const char *ui_mgr_path) {
	bool sel = (bool)gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM(main_window_view_osd));
	if (sel != user_enable_osd) {
		user_enable_osd = sel;
		fprintf(stderr,"OSD turned %s\n",user_enable_osd?"on":"off");
		gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM(main_window_view_osd),user_enable_osd);
	}
}

/* ----------------------- RECORD -------------------------------*/
static void on_main_window_control_record(GtkMenuItem *menuitem,const char *ui_mgr_path) {
	bool sel = (bool)gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM(main_window_control_record));
	CurrentInputObj()->onRecord(sel);
}

static void on_main_window_record(GtkMenuItem *menuitem,gpointer user_data) {
	bool sel = (bool)gtk_toggle_tool_button_get_active (GTK_TOGGLE_TOOL_BUTTON(GTK_TOOL_BUTTON(main_window_toolbar_record)));
	CurrentInputObj()->onRecord(sel);
}

/* ----------------------- PLAY -------------------------------*/
static void on_main_window_control_play(GtkMenuItem *menuitem,const char *ui_mgr_path) {
	bool sel = (bool)gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM(main_window_control_play));
	if (sel) { /* WARNING: Do not attempt to code UI changed on de-selection. update_ui_controls() causes toggle events and you risk funny behavior! */
		CurrentInputObj()->start_playback();
		update_ui_controls();
	}
}

static void on_main_window_play(GtkMenuItem *menuitem,gpointer user_data) {
	bool sel = (bool)gtk_toggle_tool_button_get_active (GTK_TOGGLE_TOOL_BUTTON(GTK_TOOL_BUTTON(main_window_toolbar_play)));
	if (sel) { /* WARNING: Do not attempt to code UI changed on de-selection. update_ui_controls() causes toggle events and you risk funny behavior! */
		CurrentInputObj()->start_playback();
		update_ui_controls();
	}
}

/* ----------------------- PAUSE -------------------------------*/
static void on_main_window_control_pause(GtkMenuItem *menuitem,const char *ui_mgr_path) {
	bool sel = (bool)gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM(main_window_control_pause));
	if (sel) { /* WARNING: Do not attempt to code UI changed on de-selection. update_ui_controls() causes toggle events and you risk funny behavior! */
		if (!CurrentInputObj()->Playing || !CurrentInputObj()->Paused) CurrentInputObj()->pause_playback();
		update_ui_controls();
	}
}

static void on_main_window_pause(GtkMenuItem *menuitem,gpointer user_data) {
	bool sel = (bool)gtk_toggle_tool_button_get_active (GTK_TOGGLE_TOOL_BUTTON(GTK_TOOL_BUTTON(main_window_toolbar_pause)));
	if (sel) { /* WARNING: Do not attempt to code UI changed on de-selection. update_ui_controls() causes toggle events and you risk funny behavior! */
		if (!CurrentInputObj()->Playing || !CurrentInputObj()->Paused) CurrentInputObj()->pause_playback();
		update_ui_controls();
	}
}

/* ----------------------- STOP -------------------------------*/
static void on_main_window_control_stop(GtkMenuItem *menuitem,const char *ui_mgr_path) {
	bool sel = (bool)gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM(main_window_control_stop));
	if (sel) { /* WARNING: Do not attempt to code UI changed on de-selection. update_ui_controls() causes toggle events and you risk funny behavior! */
		if (CurrentInputObj()->Playing) {
			CurrentInputObj()->stop_playback();
			update_ui_controls();
		}
	}
}

static void on_main_window_stop(GtkMenuItem *menuitem,gpointer user_data) {
	bool sel = (bool)gtk_toggle_tool_button_get_active (GTK_TOGGLE_TOOL_BUTTON(GTK_TOOL_BUTTON(main_window_toolbar_stop)));
	if (sel) { /* WARNING: Do not attempt to code UI changed on de-selection. update_ui_controls() causes toggle events and you risk funny behavior! */
		if (CurrentInputObj()->Playing) {
			CurrentInputObj()->stop_playback();
			update_ui_controls();
		}
	}
}

/* -------------------------- VIEWS -> TOOLBARS -> TOOLBAR ----------------------- */
static void on_main_window_view_toolbars_toolbar(GtkMenuItem *menuitem,const char *ui_mgr_path) {
	GtkWidget *x = gtk_ui_manager_get_widget (main_window_ui_mgr, ui_mgr_path);
	if (x == NULL) {
		g_error("Unable to retrieve widget '%s' via UI manager",ui_mgr_path);
		return;
	}

	GtkWidget *obj = gtk_ui_manager_get_widget (main_window_ui_mgr, "/ToolBar");
	if (obj == NULL) {
		g_error("Unable to retrieve ToolBar widget");
		return;
	}

	if (cfg_show_toolbar = gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM(x)))
		gtk_widget_show (obj);
	else
		gtk_widget_hide (obj);
}

/* -------------------------- VIEWS -> TOOLBARS -> METADATA ----------------------- */
static void on_main_window_view_toolbars_metadata(GtkMenuItem *menuitem,const char *ui_mgr_path) {
	GtkWidget *x = gtk_ui_manager_get_widget (main_window_ui_mgr, ui_mgr_path);
	if (x == NULL) {
		g_error("Unable to retrieve widget '%s' via UI manager",ui_mgr_path);
		return;
	}

	if (cfg_show_metadata = gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM(x)))
		gtk_widget_show (main_window_metadata);
	else
		gtk_widget_hide (main_window_metadata);
}

/* -------------------------- VIEWS -> TOOLBARS -> STATUS ----------------------- */
static void on_main_window_view_toolbars_status(GtkMenuItem *menuitem,const char *ui_mgr_path) {
	GtkWidget *x = gtk_ui_manager_get_widget (main_window_ui_mgr, ui_mgr_path);
	if (x == NULL) {
		g_error("Unable to retrieve widget '%s' via UI manager",ui_mgr_path);
		return;
	}

	if (cfg_show_status = gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM(x)))
		gtk_widget_show (main_window_status);
	else
		gtk_widget_hide (main_window_status);
}

void client_area_get_aspect_from_current_input() {
	if (CurrentInputObj()->user_ar_n > 0 && CurrentInputObj()->user_ar_d > 0) {
		client_area_aspect_n = CurrentInputObj()->user_ar_n;
		client_area_aspect_d = CurrentInputObj()->user_ar_d;
	}
	else if (CurrentInputObj()->user_ar_n == -2) {
		client_area_aspect_n = -1;
		client_area_aspect_d = -1;
	}
	else {
		client_area_aspect_n = CurrentInputObj()->source_ar_n;
		client_area_aspect_d = CurrentInputObj()->source_ar_d;
	}
}

/* ----------------------- VIEW -> ASPECT -> ... -------------------------------*/
static void on_main_window_view_aspect_ratio_select(GtkMenuItem *menuitem,gpointer user_data) { /* View -> Input select */
	int arn = -1,ard = -1;
	int x = gtk_radio_action_get_current_value (GTK_RADIO_ACTION(main_window_view_aspect_action));

	if (x == -1)
		{ arn = -2; ard = -2; }
	else if (x == 1)
		{ arn = 4; ard = 3; }
	else if (x == 2)
		{ arn = 16; ard = 9; }

	if (CurrentInputObj()->user_ar_n != arn || CurrentInputObj()->user_ar_d != ard) {
		CurrentInputObj()->user_ar_n = arn;
		CurrentInputObj()->user_ar_d = ard;
		client_area_get_aspect_from_current_input();
		client_area_update_rects_again();
		if (!client_area_xvideo) {
			if (!do_video_source(CurrentInputObj(),true)) {
				generate_non_video_frame();
				draw_osd();
			}
		}
		if (video_should_redraw_t < 0) video_should_redraw_t = NOW + 0.5;
		update_ui();
	}
}

/* ----------------------- VIEW -> INPUT -> ... -------------------------------*/
static void on_main_window_view_input_select(GtkMenuItem *menuitem,gpointer user_data) { /* View -> Input select */
	int x = gtk_radio_action_get_current_value (GTK_RADIO_ACTION(main_window_view_input_action));
	if (x != CurrentInput) {
		switch_input(x);
		update_ui();
	}
}

static void on_main_window_input_select(GtkMenuItem *menuitem,gpointer user_data) { /* Toolbar select */
	int x = gtk_radio_action_get_current_value (GTK_RADIO_ACTION(main_window_toolbar_input_action));
	if (x != CurrentInput) {
		switch_input(x);
		update_ui();
	}
}



/*----------DELETEME-------------*/
static void on_main_window_device_select(GtkMenuItem *menuitem,gpointer user_data) {
}

static void on_main_window_menu_with_audio(GtkMenuItem *menuitem,gpointer user_data) {
}









static void on_main_window_file_destroy(GtkMenuItem *menuitem,gpointer user_data) {
	secondary_thread_shutdown();
	gtk_main_quit();
}

static void on_main_window_file_quit(GtkMenuItem *menuitem,gpointer user_data) {
	int response;
	GtkWidget *dialog;

	dialog = gtk_message_dialog_new (GTK_WINDOW (main_window),
			GTK_DIALOG_DESTROY_WITH_PARENT,
			GTK_MESSAGE_QUESTION,
			GTK_BUTTONS_YES_NO,
			"Are you sure you wish to quit?");

	gtk_widget_show (dialog);
	response = gtk_dialog_run (GTK_DIALOG(dialog));
	gtk_widget_destroy (dialog);

	if (response == GTK_RESPONSE_YES)
		gtk_main_quit();
}

static gint v4l_vcrhack_dropdown_populate_and_select(GtkWidget *listbox,GtkListStore *list) {
	static const char *modelist[] = {
		"2-line VCR hack",
		"4",
		"6",
		"8",
		"10",
		"12",
		"14",
		"16",
		"18",
		"20",
		"32",
		"48",
		"64",
		"80",
		"128",
		"160",
		"192",
		"240",
		"300",
		"350",
		NULL
	};
	void **hints,**n;
	GtkTreeIter iter;
	gint active = -1;
	int count = 0;
	int index;

	gtk_list_store_append(list, &iter);
	gtk_list_store_set(list, &iter, /*column*/0, "", -1);
	if (active < 0 && CurrentInputObj()->vcrhack == "") active = 0;
	count++;

	for (index=0;modelist[index] != NULL;index++) {
		gtk_list_store_append(list, &iter);
		gtk_list_store_set(list, &iter, /*column*/0, modelist[index], -1);
		if (active < 0 && CurrentInputObj()->vcrhack == modelist[index]) active = count;
		count++;
	}

	return active;
}

static gint v4l_codec_dropdown_populate_and_select(GtkWidget *listbox,GtkListStore *list) {
	static const char *modelist[] = {
		"h264",
		"h264-422",
		NULL
	};
	void **hints,**n;
	GtkTreeIter iter;
	gint active = -1;
	int count = 0;
	int index;

	gtk_list_store_append(list, &iter);
	gtk_list_store_set(list, &iter, /*column*/0, "", -1);
	if (active < 0 && CurrentInputObj()->input_codec == "") active = 0;
	count++;

	for (index=0;modelist[index] != NULL;index++) {
		gtk_list_store_append(list, &iter);
		gtk_list_store_set(list, &iter, /*column*/0, modelist[index], -1);
		if (active < 0 && CurrentInputObj()->input_codec == modelist[index]) active = count;
		count++;
	}

	return active;
}

static gint v4l_capres_dropdown_populate_and_select(GtkWidget *listbox,GtkListStore *list) {
	static const char *modelist[] = {
		"160x120",
		"240x180",
		"320x240",
		"352x288",
		"352x480",
		"480x480",
		"480x576",
		"576x480",
		"576x576",
		"640x480",
		"640x576",
		"720x480",
		"720x576",
		"800x600",
		"1024x768",
		"1280x720",
		"1920x1080",
		NULL
	};
	void **hints,**n;
	GtkTreeIter iter;
	gint active = -1;
	int count = 0;
	char tmp[64];
	int index;

	gtk_list_store_append(list, &iter);
	gtk_list_store_set(list, &iter, /*column*/0, "", -1);
	if (active < 0 && (CurrentInputObj()->capture_width == 0 || CurrentInputObj()->capture_height == 0)) active = 0;
	count++;

	sprintf(tmp,"%dx%d",CurrentInputObj()->capture_width,CurrentInputObj()->capture_height);
	for (index=0;modelist[index] != NULL;index++) {
		gtk_list_store_append(list, &iter);
		gtk_list_store_set(list, &iter, /*column*/0, modelist[index], -1);
		if (active < 0 && !strcmp(tmp,modelist[index])) active = count;
		count++;
	}

	return active;
}

static gint v4l_capfps_dropdown_populate_and_select(GtkWidget *listbox,GtkListStore *list) {
	static const char *modelist[] = {
		"1",
		"2",
		"5",
		"10",
		"15",
		"20",
		"24",
		"25",
		"29.97",
		"30",
		"40",
		"50",
		"59.94",
		"60",
		"70",
		"75",
		"80",
		"90",
		"100",
		"119.88",
		"120",
		NULL
	};
	void **hints,**n;
	GtkTreeIter iter;
	gint active = -1;
	int count = 0;
	int index;

	gtk_list_store_append(list, &iter);
	gtk_list_store_set(list, &iter, /*column*/0, "", -1);
	if (active < 0 && (CurrentInputObj()->capture_fps <= 0)) active = 0;
	count++;

	for (index=0;modelist[index] != NULL;index++) {
		gtk_list_store_append(list, &iter);
		gtk_list_store_set(list, &iter, /*column*/0, modelist[index], -1);
		if (active < 0 && atof(modelist[index]) == CurrentInputObj()->capture_fps) active = count;
		count++;
	}

	return active;
}

static gint v4l_standard_dropdown_populate_and_select(GtkWidget *listbox,GtkListStore *list) {
	void **hints,**n;
	GtkTreeIter iter;
	gint active = -1;
	int count = 0;
	char tmp[64];
	int fd;

	gtk_list_store_append(list, &iter);
	gtk_list_store_set(list, &iter, /*column*/0, "", -1);
	if (active < 0 && CurrentInputObj()->input_standard == "") active = 0;
	count++;

	if (CurrentInputObj()->video_index >= 0) {
		sprintf(tmp,"/dev/video%u",CurrentInputObj()->video_index);
		fd = open(tmp,O_RDONLY);
		if (fd < 0) fprintf(stderr,"Failed to open %s, %s\n",tmp,strerror(errno));
	}
	else {
		fd = -1;
	}

	if (fd >= 0) {
		struct v4l2_standard v4l2_std;
		unsigned int index;

		for (index=0;index < 10000;index++) {
			memset(&v4l2_std,0,sizeof(v4l2_std));
			v4l2_std.index = index;
			if (ioctl(fd,VIDIOC_ENUMSTD,&v4l2_std) == 0) {
				fprintf(stderr," [%u] '%s'\n",index,(const char*)v4l2_std.name);

				gtk_list_store_append(list, &iter);
				gtk_list_store_set(list, &iter, /*column*/0, (const char*)v4l2_std.name, -1);
				if (active < 0 && CurrentInputObj()->input_standard == (const char*)v4l2_std.name) active = count;
				count++;
			}
		}

		close(fd);
		fd = -1;
	}

	return active;
}

static gint v4l_input_dropdown_populate_and_select(GtkWidget *listbox,GtkListStore *list) {
	void **hints,**n;
	GtkTreeIter iter;
	gint active = -1;
	int count = 0;
	char tmp[64];
	int fd;

	gtk_list_store_append(list, &iter);
	gtk_list_store_set(list, &iter, /*column*/0, "", -1);
	if (active < 0 && CurrentInputObj()->input_device == "") active = 0;
	count++;

	if (CurrentInputObj()->video_index >= 0) {
		sprintf(tmp,"/dev/video%u",CurrentInputObj()->video_index);
		fd = open(tmp,O_RDONLY);
		if (fd < 0) fprintf(stderr,"Failed to open %s, %s\n",tmp,strerror(errno));
	}
	else {
		fd = -1;
	}

	if (fd >= 0) {
		struct v4l2_input v4l2_input;
		unsigned int index;

		for (index=0;index < 10000;index++) {
			memset(&v4l2_input,0,sizeof(v4l2_input));
			v4l2_input.index = index;
			if (ioctl(fd,VIDIOC_ENUMINPUT,&v4l2_input) == 0) {
				fprintf(stderr," [%u] '%s'\n",index,(const char*)v4l2_input.name);

				gtk_list_store_append(list, &iter);
				gtk_list_store_set(list, &iter, /*column*/0, (const char*)v4l2_input.name, -1);
				if (active < 0 && CurrentInputObj()->input_device == (const char*)v4l2_input.name) active = count;
				count++;
			}
		}

		close(fd);
		fd = -1;
	}

	return active;
}

static gint alsa_dropdown_populate_and_select(GtkWidget *listbox,GtkListStore *list) {
	void **hints,**n;
	GtkTreeIter iter;
	gint active = -1;
	int count = 0;

	gtk_list_store_append(list, &iter);
	gtk_list_store_set(list, &iter, /*column*/0, "", -1);
	if (active < 0 && CurrentInputObj()->audio_device == "") active = 0;
	count++;

	if (snd_device_name_hint(-1,"pcm",&hints) == 0) {
		n = hints;
		while (*n != NULL) {
			char *name = snd_device_name_get_hint(*n, "NAME");
			if (name == NULL) {
				n++;
				continue;
			}

			char *desc = snd_device_name_get_hint(*n, "DESC");
			if (desc != NULL) {
				/* please chop the string at the '\n' */
				char *x = strchr(desc,'\n');
				if (x != NULL) *x = 0;
			}

			char *ioid = snd_device_name_get_hint(*n, "IOID");

			if (ioid != NULL) {
				if (!strcasecmp(ioid,"Output")) {
					free(name);
					n++;
					continue;
				}
			}

			fprintf(stderr," '%s' is '%s'\n",name,ioid != NULL ? ioid : "I/O");

			gtk_list_store_append(list, &iter);
			gtk_list_store_set(list, &iter, /*column*/0, name, /*column*/1, desc?desc:"", -1);
			if (active < 0 && CurrentInputObj()->audio_device == name) active = count;
			count++;

			if (ioid != NULL) free(ioid);
			if (desc != NULL) free(desc);
			free(name);

			n++;
		}

		snd_device_name_free_hint(hints);
	}
	else {
		fprintf(stderr,"WARNING: Unable to enumerate ALSA\n");
	}

	return active;
}

static void update_audio_dialog_from_vars() {
	GtkTreeModel *model;
	char tmp[1024];
	gint active;

	if (audio_dialog == NULL) return;

	sprintf(tmp,"Audio settings for %s",CurrentInputObj()->osd_name);
	gtk_window_set_title (GTK_WINDOW(audio_dialog), tmp);

	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(audio_dialog_enable), CurrentInputObj()->enable_audio);

	model = gtk_combo_box_get_model (GTK_COMBO_BOX(audio_dialog_device));
	if (model != NULL) {
		gtk_list_store_clear (GTK_LIST_STORE(model));
		gtk_combo_box_set_model (GTK_COMBO_BOX(audio_dialog_device), model);
	}

	model = GTK_TREE_MODEL(gtk_list_store_new (2, G_TYPE_STRING, G_TYPE_STRING));
	assert(model != NULL);
	active = alsa_dropdown_populate_and_select (audio_dialog_device, GTK_LIST_STORE(model));
	gtk_combo_box_set_model (GTK_COMBO_BOX(audio_dialog_device), model);

	gtk_combo_box_set_active (GTK_COMBO_BOX(audio_dialog_device), active);
}

static void update_input_dialog_from_vars() {
	GtkTreeModel *model;
	char tmp[1024];
	gint active;

	if (input_dialog == NULL) return;

	sprintf(tmp,"Input settings for %s",CurrentInputObj()->osd_name);
	gtk_window_set_title (GTK_WINDOW(input_dialog), tmp);

	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(input_dialog_vbi_enable), CurrentInputObj()->enable_vbi);
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(input_dialog_def_crop), CurrentInputObj()->crop_defrect);
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(input_dialog_bounds_crop), CurrentInputObj()->crop_bounds);

	/* input select */
	model = gtk_combo_box_get_model (GTK_COMBO_BOX(input_dialog_device));
	if (model != NULL) {
		gtk_list_store_clear (GTK_LIST_STORE(model));
		gtk_combo_box_set_model (GTK_COMBO_BOX(input_dialog_device), model);
	}

	model = GTK_TREE_MODEL(gtk_list_store_new (2, G_TYPE_STRING, G_TYPE_STRING));
	assert(model != NULL);
	active = v4l_input_dropdown_populate_and_select (input_dialog_device, GTK_LIST_STORE(model));
	gtk_combo_box_set_model (GTK_COMBO_BOX(input_dialog_device), model);

	gtk_combo_box_set_active (GTK_COMBO_BOX(input_dialog_device), active);

	/* standard select */
	model = gtk_combo_box_get_model (GTK_COMBO_BOX(input_dialog_standard));
	if (model != NULL) {
		gtk_list_store_clear (GTK_LIST_STORE(model));
		gtk_combo_box_set_model (GTK_COMBO_BOX(input_dialog_standard), model);
	}

	model = GTK_TREE_MODEL(gtk_list_store_new (2, G_TYPE_STRING, G_TYPE_STRING));
	assert(model != NULL);
	active = v4l_standard_dropdown_populate_and_select (input_dialog_standard, GTK_LIST_STORE(model));
	gtk_combo_box_set_model (GTK_COMBO_BOX(input_dialog_standard), model);

	gtk_combo_box_set_active (GTK_COMBO_BOX(input_dialog_standard), active);

	/* capture res select */
	model = gtk_combo_box_get_model (GTK_COMBO_BOX(input_dialog_capres));
	if (model != NULL) {
		gtk_list_store_clear (GTK_LIST_STORE(model));
		gtk_combo_box_set_model (GTK_COMBO_BOX(input_dialog_capres), model);
	}

	model = GTK_TREE_MODEL(gtk_list_store_new (2, G_TYPE_STRING, G_TYPE_STRING));
	assert(model != NULL);
	active = v4l_capres_dropdown_populate_and_select (input_dialog_capres, GTK_LIST_STORE(model));
	gtk_combo_box_set_model (GTK_COMBO_BOX(input_dialog_capres), model);

	gtk_combo_box_set_active (GTK_COMBO_BOX(input_dialog_capres), active);

	/* capture fps select */
	model = gtk_combo_box_get_model (GTK_COMBO_BOX(input_dialog_capfps));
	if (model != NULL) {
		gtk_list_store_clear (GTK_LIST_STORE(model));
		gtk_combo_box_set_model (GTK_COMBO_BOX(input_dialog_capfps), model);
	}

	model = GTK_TREE_MODEL(gtk_list_store_new (2, G_TYPE_STRING, G_TYPE_STRING));
	assert(model != NULL);
	active = v4l_capfps_dropdown_populate_and_select (input_dialog_capfps, GTK_LIST_STORE(model));
	gtk_combo_box_set_model (GTK_COMBO_BOX(input_dialog_capfps), model);

	gtk_combo_box_set_active (GTK_COMBO_BOX(input_dialog_capfps), active);

	/* codec select */
	model = gtk_combo_box_get_model (GTK_COMBO_BOX(input_dialog_codec));
	if (model != NULL) {
		gtk_list_store_clear (GTK_LIST_STORE(model));
		gtk_combo_box_set_model (GTK_COMBO_BOX(input_dialog_codec), model);
	}

	model = GTK_TREE_MODEL(gtk_list_store_new (2, G_TYPE_STRING, G_TYPE_STRING));
	assert(model != NULL);
	active = v4l_codec_dropdown_populate_and_select (input_dialog_codec, GTK_LIST_STORE(model));
	gtk_combo_box_set_model (GTK_COMBO_BOX(input_dialog_codec), model);

	gtk_combo_box_set_active (GTK_COMBO_BOX(input_dialog_codec), active);

	/* vcr hack */
	model = gtk_combo_box_get_model (GTK_COMBO_BOX(input_dialog_vcrhack));
	if (model != NULL) {
		gtk_list_store_clear (GTK_LIST_STORE(model));
		gtk_combo_box_set_model (GTK_COMBO_BOX(input_dialog_vcrhack), model);
	}

	model = GTK_TREE_MODEL(gtk_list_store_new (2, G_TYPE_STRING, G_TYPE_STRING));
	assert(model != NULL);
	active = v4l_vcrhack_dropdown_populate_and_select (input_dialog_vcrhack, GTK_LIST_STORE(model));
	gtk_combo_box_set_model (GTK_COMBO_BOX(input_dialog_vcrhack), model);

	gtk_combo_box_set_active (GTK_COMBO_BOX(input_dialog_vcrhack), active);
}

static void update_x_dialog_from_vars() {
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(x_dialog_shm), use_x_shm);
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(x_dialog_xvideo), use_xvideo);
}

static bool update_vars_from_input_dialog() {
	bool tmp,do_reopen=false;
	bool do_restart=false;
	GtkTreeModel *model;
	gint active;

	if (input_dialog == NULL) return false;

	tmp = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON(input_dialog_vbi_enable)) > 0;
	if (tmp != CurrentInputObj()->enable_vbi) do_reopen = true;
	CurrentInputObj()->enable_vbi = tmp;

	tmp = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON(input_dialog_def_crop)) > 0;
	if (tmp != CurrentInputObj()->crop_defrect) do_reopen = true;
	CurrentInputObj()->crop_defrect = tmp;

	tmp = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON(input_dialog_bounds_crop)) > 0;
	if (tmp != CurrentInputObj()->crop_bounds) do_reopen = true;
	CurrentInputObj()->crop_bounds = tmp;

	/* input */
	active = gtk_combo_box_get_active (GTK_COMBO_BOX(input_dialog_device));
	model = gtk_combo_box_get_model (GTK_COMBO_BOX(input_dialog_device));

	std::string old_input_device = CurrentInputObj()->input_device;

	CurrentInputObj()->input_device.clear();
	if (model != NULL && active >= 0) {
		GtkTreeIter iter;
		char *str;
		GValue v;

		memset(&v,0,sizeof(v));
		if (gtk_tree_model_iter_nth_child(model,&iter,NULL,active) == TRUE) {
			gtk_tree_model_get_value(model,&iter,0,&v);
			str = (char*)g_value_get_string(&v);
			if (str) CurrentInputObj()->input_device = str;
			fprintf(stderr,"updated input dev is '%s'\n",str);
		}

		g_value_unset(&v);
	}

	if (old_input_device != CurrentInputObj()->input_device)
		do_reopen = true;

	/* standard */
	active = gtk_combo_box_get_active (GTK_COMBO_BOX(input_dialog_standard));
	model = gtk_combo_box_get_model (GTK_COMBO_BOX(input_dialog_standard));

	std::string old_input_standard = CurrentInputObj()->input_standard;

	CurrentInputObj()->input_standard.clear();
	if (model != NULL && active >= 0) {
		GtkTreeIter iter;
		char *str;
		GValue v;

		memset(&v,0,sizeof(v));
		if (gtk_tree_model_iter_nth_child(model,&iter,NULL,active) == TRUE) {
			gtk_tree_model_get_value(model,&iter,0,&v);
			str = (char*)g_value_get_string(&v);
			if (str) CurrentInputObj()->input_standard = str;
			fprintf(stderr,"updated input std is '%s'\n",str);
		}

		g_value_unset(&v);
	}

	if (old_input_standard != CurrentInputObj()->input_standard)
		do_reopen = true;

	/* res */
	active = gtk_combo_box_get_active (GTK_COMBO_BOX(input_dialog_capres));
	model = gtk_combo_box_get_model (GTK_COMBO_BOX(input_dialog_capres));

	int old_capw = CurrentInputObj()->capture_width;
	int old_caph = CurrentInputObj()->capture_height;

	CurrentInputObj()->capture_width = -1;
	CurrentInputObj()->capture_height = -1;
	if (model != NULL && active >= 0) {
		GtkTreeIter iter;
		char *str;
		GValue v;

		memset(&v,0,sizeof(v));
		if (gtk_tree_model_iter_nth_child(model,&iter,NULL,active) == TRUE) {
			gtk_tree_model_get_value(model,&iter,0,&v);
			str = (char*)g_value_get_string(&v);

			if (isdigit(*str)) {
				CurrentInputObj()->capture_width = (int)strtoul(str,&str,10);
				if (*str == 'x') str++;
				CurrentInputObj()->capture_height = (int)strtoul(str,&str,10);
			}
		}

		g_value_unset(&v);
	}

	if (old_capw != CurrentInputObj()->capture_width || old_caph != CurrentInputObj()->capture_height)
		do_reopen = true;

    /* frame rate */
	active = gtk_combo_box_get_active (GTK_COMBO_BOX(input_dialog_capfps));
	model = gtk_combo_box_get_model (GTK_COMBO_BOX(input_dialog_capfps));

    double old_capf = CurrentInputObj()->capture_fps;

    CurrentInputObj()->capture_fps = -1;
    if (model != NULL && active >= 0) {
		GtkTreeIter iter;
		char *str;
		GValue v;

		memset(&v,0,sizeof(v));
		if (gtk_tree_model_iter_nth_child(model,&iter,NULL,active) == TRUE) {
			gtk_tree_model_get_value(model,&iter,0,&v);
			str = (char*)g_value_get_string(&v);

			if (isdigit(*str)) {
				CurrentInputObj()->capture_fps = atof(str);
			}
		}

		g_value_unset(&v);
	}

	if (old_capf != CurrentInputObj()->capture_fps)
		do_reopen = true;

	/* codec */
	active = gtk_combo_box_get_active (GTK_COMBO_BOX(input_dialog_codec));
	model = gtk_combo_box_get_model (GTK_COMBO_BOX(input_dialog_codec));

	std::string old_input_codec = CurrentInputObj()->input_codec;

	CurrentInputObj()->input_codec.clear();
	if (model != NULL && active >= 0) {
		GtkTreeIter iter;
		char *str;
		GValue v;

		memset(&v,0,sizeof(v));
		if (gtk_tree_model_iter_nth_child(model,&iter,NULL,active) == TRUE) {
			gtk_tree_model_get_value(model,&iter,0,&v);
			str = (char*)g_value_get_string(&v);
			if (str) CurrentInputObj()->input_codec = str;
			fprintf(stderr,"updated input codec is '%s'\n",str);
		}

		g_value_unset(&v);
	}

	if (old_input_codec != CurrentInputObj()->input_codec)
		do_reopen = true;

	/* vcrhack */
	active = gtk_combo_box_get_active (GTK_COMBO_BOX(input_dialog_vcrhack));
	model = gtk_combo_box_get_model (GTK_COMBO_BOX(input_dialog_vcrhack));

	std::string old_vcrhack = CurrentInputObj()->vcrhack;

	CurrentInputObj()->vcrhack.clear();
	if (model != NULL && active >= 0) {
		GtkTreeIter iter;
		char *str;
		GValue v;

		memset(&v,0,sizeof(v));
		if (gtk_tree_model_iter_nth_child(model,&iter,NULL,active) == TRUE) {
			gtk_tree_model_get_value(model,&iter,0,&v);
			str = (char*)g_value_get_string(&v);
			if (str) CurrentInputObj()->vcrhack = str;
			fprintf(stderr,"updated input codec is '%s'\n",str);
		}

		g_value_unset(&v);
	}

	if (old_vcrhack != CurrentInputObj()->vcrhack)
		do_reopen = true;

	/* reopen */
	if (do_reopen)
		CurrentInputObj()->reopen_input();

	return do_reopen;
}

static bool update_vars_from_audio_dialog() {
	bool tmp,do_reopen=false;
	bool do_restart=false;
	GtkTreeModel *model;
	gint active;

	if (audio_dialog == NULL) return false;

	tmp = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON(audio_dialog_enable)) > 0;
	if (tmp != CurrentInputObj()->enable_audio) do_reopen = true;
	CurrentInputObj()->enable_audio = tmp;

	active = gtk_combo_box_get_active (GTK_COMBO_BOX(audio_dialog_device));
	model = gtk_combo_box_get_model (GTK_COMBO_BOX(audio_dialog_device));

	std::string old_audio_device = CurrentInputObj()->audio_device;

	CurrentInputObj()->audio_device.clear();
	if (model != NULL && active >= 0) {
		GtkTreeIter iter;
		char *str;
		GValue v;

		memset(&v,0,sizeof(v));
		if (gtk_tree_model_iter_nth_child(model,&iter,NULL,active) == TRUE) {
			gtk_tree_model_get_value(model,&iter,0,&v);
			str = (char*)g_value_get_string(&v);
			if (str) CurrentInputObj()->audio_device = str;
			fprintf(stderr,"updated audio dev is '%s'\n",str);
		}

		g_value_unset(&v);
	}

	if (old_audio_device != CurrentInputObj()->audio_device)
		do_reopen = true;

	if (do_reopen)
		CurrentInputObj()->reopen_input();

	return do_reopen;
}

static bool update_vars_from_x_dialog() {
	bool tmp,do_reopen=false;

	tmp = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON(x_dialog_shm)) > 0;
	if (tmp != use_x_shm) do_reopen = true;
	use_x_shm = tmp;

	tmp = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON(x_dialog_xvideo)) > 0;
	if (tmp != use_xvideo) do_reopen = true;
	use_xvideo = tmp;

	return do_reopen;
}

static void update_ip_dialog_from_vars() {
	char tmp[64];

	gtk_entry_set_text (GTK_ENTRY(ip_dialog_ip_addr), ip_in_addr.c_str());

	sprintf(tmp,"%d",ip_in_port);
	gtk_entry_set_text (GTK_ENTRY(ip_dialog_ip_port), tmp);

	gtk_combo_box_set_active (GTK_COMBO_BOX(ip_dialog_protocol), ip_in_proto);
}

static void update_vars_from_ip_dialog() {
	const char *p;

	p = (const char*)(gtk_entry_get_text (GTK_ENTRY(ip_dialog_ip_addr)));
	if (*p == 0) p = "239.1.1.2";
	ip_in_addr = p;

	ip_in_port = atoi((const char*)(gtk_entry_get_text (GTK_ENTRY(ip_dialog_ip_port))));
	if (ip_in_port < 1) ip_in_port = 1;
	else if (ip_in_port > 65534) ip_in_port = 65534;

	ip_in_proto = gtk_combo_box_get_active (GTK_COMBO_BOX(ip_dialog_protocol));
}

static void on_audio_dialog_response(GtkAction *action, gint response_id, gpointer user)
{
	/* FIXME: Uhhhh so okay, the "OK" button sends the "Apply" response? */
	if (response_id == GTK_RESPONSE_OK || response_id == GTK_RESPONSE_APPLY) {
		bool reopen;

		reopen = update_vars_from_audio_dialog();
		if (reopen) {
			if (pthread_mutex_lock(&global_mutex) == 0) {
				CurrentInputObj()->reopen_input();
				pthread_mutex_unlock(&global_mutex);
			}
		}
	}

	gtk_widget_destroy(audio_dialog);
	audio_dialog = NULL; /* GTKDialog is going to delete the widget regardless of what we do, so just fagheddaboutit */
}

static void on_audio_dialog_delete(GtkAction *action, GtkWidget *window)
{
	gtk_widget_destroy(audio_dialog);
	audio_dialog = NULL; /* GTKDialog is going to delete the widget regardless of what we do, so just fagheddaboutit */
}

static void on_x_dialog_response(GtkAction *action, gint response_id, gpointer user)
{
	/* FIXME: Uhhhh so okay, the "OK" button sends the "Apply" response? */
	if (response_id == GTK_RESPONSE_OK || response_id == GTK_RESPONSE_APPLY) {
		bool reopen;

		reopen = update_vars_from_x_dialog();
		if (reopen) {
			if (pthread_mutex_lock(&global_mutex) == 0) {
				client_area_free();
				client_area_alloc();
				if (!do_video_source(CurrentInputObj(),true)) {
					generate_non_video_frame();
					draw_osd();
				}
				client_area_redraw_source_frame();
				pthread_mutex_unlock(&global_mutex);
			}
		}
	}

	gtk_widget_destroy(x_dialog);
	x_dialog = NULL; /* GTKDialog is going to delete the widget regardless of what we do, so just fagheddaboutit */
}

static void on_x_dialog_delete(GtkAction *action, GtkWidget *window)
{
	gtk_widget_destroy(x_dialog);
	x_dialog = NULL; /* GTKDialog is going to delete the widget regardless of what we do, so just fagheddaboutit */
}

static void on_ip_dialog_response(GtkAction *action, gint response_id, gpointer user)
{
	/* FIXME: Uhhhh so okay, the "OK" button sends the "Apply" response? */
	if (response_id == GTK_RESPONSE_OK || response_id == GTK_RESPONSE_APPLY) {
		update_vars_from_ip_dialog();
	}

	gtk_widget_destroy(ip_dialog);
	ip_dialog = NULL; /* GTKDialog is going to delete the widget regardless of what we do, so just fagheddaboutit */
}

static void on_ip_dialog_delete(GtkAction *action, GtkWidget *window)
{
	gtk_widget_destroy(ip_dialog);
	ip_dialog = NULL; /* GTKDialog is going to delete the widget regardless of what we do, so just fagheddaboutit */
}

static void on_input_dialog_response(GtkAction *action, gint response_id, gpointer user)
{
	/* FIXME: Uhhhh so okay, the "OK" button sends the "Apply" response? */
	if (response_id == GTK_RESPONSE_OK || response_id == GTK_RESPONSE_APPLY) {
		bool reopen;

		reopen = update_vars_from_input_dialog();
		if (reopen) {
			if (pthread_mutex_lock(&global_mutex) == 0) {
				CurrentInputObj()->reopen_input();
				pthread_mutex_unlock(&global_mutex);
			}
		}
	}

	gtk_widget_destroy(input_dialog);
	input_dialog = NULL; /* GTKDialog is going to delete the widget regardless of what we do, so just fagheddaboutit */
}

static void on_input_dialog_delete(GtkAction *action, GtkWidget *window)
{
	gtk_widget_destroy(input_dialog);
	input_dialog = NULL; /* GTKDialog is going to delete the widget regardless of what we do, so just fagheddaboutit */
}

void create_input_dialog() {
	GtkWidget *vbox,*hbox,*label,*icon;

	assert(input_dialog == NULL);

	input_dialog = gtk_dialog_new_with_buttons ("Input capture settings", GTK_WINDOW(main_window), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_STOCK_OK, GTK_RESPONSE_APPLY, NULL);
	gtk_window_set_icon(GTK_WINDOW(input_dialog),application_icon_gdk_pixbuf);
	gtk_window_set_resizable(GTK_WINDOW(input_dialog), FALSE);

	vbox = gtk_vbox_new (FALSE, 5);
	gtk_box_pack_start (GTK_BOX(GTK_DIALOG(input_dialog)->vbox), vbox, FALSE, FALSE, 0);
	gtk_container_set_border_width (GTK_CONTAINER(vbox), 5);

	/* input select */
	hbox = gtk_hbox_new (FALSE, 0);

	input_dialog_device = gtk_combo_box_new ();
	gtk_container_add (GTK_CONTAINER(hbox), input_dialog_device);

	{
		GtkCellRenderer *cell;

		cell = gtk_cell_renderer_text_new();
		gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(input_dialog_device), cell, TRUE);
		gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(input_dialog_device), cell, "text", 0, NULL);
	}

	gtk_container_add (GTK_CONTAINER(vbox), hbox);

	/* std select */
	hbox = gtk_hbox_new (FALSE, 0);

	input_dialog_standard = gtk_combo_box_new ();
	gtk_container_add (GTK_CONTAINER(hbox), input_dialog_standard);

	{
		GtkCellRenderer *cell;

		cell = gtk_cell_renderer_text_new();
		gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(input_dialog_standard), cell, TRUE);
		gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(input_dialog_standard), cell, "text", 0, NULL);
	}

	gtk_container_add (GTK_CONTAINER(vbox), hbox);

	/* res select */
	hbox = gtk_hbox_new (FALSE, 0);

	input_dialog_capres = gtk_combo_box_new ();
	gtk_container_add (GTK_CONTAINER(hbox), input_dialog_capres);

	{
		GtkCellRenderer *cell;

		cell = gtk_cell_renderer_text_new();
		gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(input_dialog_capres), cell, TRUE);
		gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(input_dialog_capres), cell, "text", 0, NULL);
	}

	gtk_container_add (GTK_CONTAINER(vbox), hbox);

	/* fps select */
	hbox = gtk_hbox_new (FALSE, 0);

	input_dialog_capfps = gtk_combo_box_new ();
	gtk_container_add (GTK_CONTAINER(hbox), input_dialog_capfps);

	{
		GtkCellRenderer *cell;

		cell = gtk_cell_renderer_text_new();
		gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(input_dialog_capfps), cell, TRUE);
		gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(input_dialog_capfps), cell, "text", 0, NULL);
	}

	gtk_container_add (GTK_CONTAINER(vbox), hbox);

	/* codec select */
	hbox = gtk_hbox_new (FALSE, 0);

	input_dialog_codec = gtk_combo_box_new ();
	gtk_container_add (GTK_CONTAINER(hbox), input_dialog_codec);

	{
		GtkCellRenderer *cell;

		cell = gtk_cell_renderer_text_new();
		gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(input_dialog_codec), cell, TRUE);
		gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(input_dialog_codec), cell, "text", 0, NULL);
	}

	gtk_container_add (GTK_CONTAINER(vbox), hbox);

	/* vcr hack */
	hbox = gtk_hbox_new (FALSE, 0);

	input_dialog_vcrhack = gtk_combo_box_new ();
	gtk_container_add (GTK_CONTAINER(hbox), input_dialog_vcrhack);

	{
		GtkCellRenderer *cell;

		cell = gtk_cell_renderer_text_new();
		gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(input_dialog_vcrhack), cell, TRUE);
		gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(input_dialog_vcrhack), cell, "text", 0, NULL);
	}

	gtk_container_add (GTK_CONTAINER(vbox), hbox);

	/* Enable X SHM extension */
	hbox = gtk_hbox_new (FALSE, 0);

	input_dialog_vbi_enable = gtk_check_button_new_with_label ("Enable VBI capture");
	gtk_container_add (GTK_CONTAINER(hbox), input_dialog_vbi_enable);

	gtk_container_add (GTK_CONTAINER(vbox), hbox);


	hbox = gtk_hbox_new (FALSE, 0);

	input_dialog_def_crop = gtk_check_button_new_with_label ("Use default crop rectangle");
	gtk_container_add (GTK_CONTAINER(hbox), input_dialog_def_crop);

	gtk_container_add (GTK_CONTAINER(vbox), hbox);


	hbox = gtk_hbox_new (FALSE, 0);

	input_dialog_bounds_crop = gtk_check_button_new_with_label ("Use boundary crop rectangle");
	gtk_container_add (GTK_CONTAINER(hbox), input_dialog_bounds_crop);

	gtk_container_add (GTK_CONTAINER(vbox), hbox);

	/* done */
	g_signal_connect(input_dialog, "destroy",
			G_CALLBACK(on_input_dialog_delete),
			NULL);
	g_signal_connect(input_dialog, "delete-event",
			G_CALLBACK(on_input_dialog_delete),
			NULL);
	g_signal_connect(input_dialog, "response",
			G_CALLBACK(on_input_dialog_response),
			NULL);

	gtk_dialog_set_default_response (GTK_DIALOG(input_dialog), GTK_RESPONSE_APPLY);

	/* Friendly reminder */
	hbox = gtk_hbox_new (FALSE, 5);

	icon = gtk_image_new_from_stock (GTK_STOCK_DIALOG_WARNING,GTK_ICON_SIZE_SMALL_TOOLBAR);
	gtk_container_add (GTK_CONTAINER(hbox), icon);

	label = gtk_label_new ("These input settings apply only to the currently selected input.\nYou can leave the frame size field unset to use the capture card's default for best quality.");
	gtk_container_add (GTK_CONTAINER(hbox), label);

	gtk_container_add (GTK_CONTAINER(vbox), hbox);
}

void create_audio_dialog() {
	GtkWidget *vbox,*hbox,*label,*icon;

	assert(audio_dialog == NULL);

	audio_dialog = gtk_dialog_new_with_buttons ("Audio capture settings", GTK_WINDOW(main_window), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_STOCK_OK, GTK_RESPONSE_APPLY, NULL);
	gtk_window_set_icon(GTK_WINDOW(audio_dialog),application_icon_gdk_pixbuf);
	gtk_window_set_resizable(GTK_WINDOW(audio_dialog), FALSE);

	vbox = gtk_vbox_new (FALSE, 5);
	gtk_box_pack_start (GTK_BOX(GTK_DIALOG(audio_dialog)->vbox), vbox, FALSE, FALSE, 0);
	gtk_container_set_border_width (GTK_CONTAINER(vbox), 5);

	/* Enable X SHM extension */
	hbox = gtk_hbox_new (FALSE, 0);

	audio_dialog_enable = gtk_check_button_new_with_label ("Enable audio capture");
	gtk_container_add (GTK_CONTAINER(hbox), audio_dialog_enable);

	gtk_container_add (GTK_CONTAINER(vbox), hbox);

	/* audio select */
	hbox = gtk_hbox_new (FALSE, 0);

	audio_dialog_device = gtk_combo_box_new ();
	gtk_container_add (GTK_CONTAINER(hbox), audio_dialog_device);

	{
		GtkCellRenderer *cell;

		cell = gtk_cell_renderer_text_new();
		gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(audio_dialog_device), cell, TRUE);
		gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(audio_dialog_device), cell, "text", 0, NULL);

		cell = gtk_cell_renderer_text_new();
		gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(audio_dialog_device), cell, TRUE);
		gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(audio_dialog_device), cell, "text", 1, NULL);
	}

	gtk_container_add (GTK_CONTAINER(vbox), hbox);

	g_signal_connect(audio_dialog, "destroy",
			G_CALLBACK(on_audio_dialog_delete),
			NULL);
	g_signal_connect(audio_dialog, "delete-event",
			G_CALLBACK(on_audio_dialog_delete),
			NULL);
	g_signal_connect(audio_dialog, "response",
			G_CALLBACK(on_audio_dialog_response),
			NULL);

	gtk_dialog_set_default_response (GTK_DIALOG(audio_dialog), GTK_RESPONSE_APPLY);

	/* Friendly reminder */
	hbox = gtk_hbox_new (FALSE, 5);

	icon = gtk_image_new_from_stock (GTK_STOCK_DIALOG_WARNING,GTK_ICON_SIZE_SMALL_TOOLBAR);
	gtk_container_add (GTK_CONTAINER(hbox), icon);

	label = gtk_label_new ("These audio settings apply only to the currently selected input");
	gtk_container_add (GTK_CONTAINER(hbox), label);

	gtk_container_add (GTK_CONTAINER(vbox), hbox);
}

void create_x_dialog() {
	GtkWidget *vbox,*hbox,*label,*icon;

	assert(x_dialog == NULL);

	x_dialog = gtk_dialog_new_with_buttons ("X Video Display", GTK_WINDOW(main_window), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_STOCK_OK, GTK_RESPONSE_APPLY, NULL);
	gtk_window_set_icon(GTK_WINDOW(x_dialog),application_icon_gdk_pixbuf);
	gtk_window_set_resizable(GTK_WINDOW(x_dialog), FALSE);

	vbox = gtk_vbox_new (FALSE, 5);
	gtk_box_pack_start (GTK_BOX(GTK_DIALOG(x_dialog)->vbox), vbox, FALSE, FALSE, 0);
	gtk_container_set_border_width (GTK_CONTAINER(vbox), 5);

	/* Enable X SHM extension */
	hbox = gtk_hbox_new (FALSE, 0);

	x_dialog_shm = gtk_check_button_new_with_label ("Use SHM extension");
	gtk_container_add (GTK_CONTAINER(hbox), x_dialog_shm);

	x_dialog_xvideo = gtk_check_button_new_with_label ("Use XVideo extension");
	gtk_container_add (GTK_CONTAINER(hbox), x_dialog_xvideo);

	gtk_container_add (GTK_CONTAINER(vbox), hbox);

	g_signal_connect(x_dialog, "destroy",
			G_CALLBACK(on_x_dialog_delete),
			NULL);
	g_signal_connect(x_dialog, "delete-event",
			G_CALLBACK(on_x_dialog_delete),
			NULL);
	g_signal_connect(x_dialog, "response",
			G_CALLBACK(on_x_dialog_response),
			NULL);

	gtk_dialog_set_default_response (GTK_DIALOG(x_dialog), GTK_RESPONSE_APPLY);

	/* Friendly reminder */
	hbox = gtk_hbox_new (FALSE, 5);

	icon = gtk_image_new_from_stock (GTK_STOCK_DIALOG_WARNING,GTK_ICON_SIZE_SMALL_TOOLBAR);
	gtk_container_add (GTK_CONTAINER(hbox), icon);

	label = gtk_label_new ("Leave SHM and XVideo enabled for best system\nand application performance. If this program\ncauses problems with your video drivers,\ndisable XVideo first.");
	gtk_container_add (GTK_CONTAINER(hbox), label);

	gtk_container_add (GTK_CONTAINER(vbox), hbox);
}

void create_ip_dialog() {
	GtkWidget *vbox,*hbox,*label;

	assert(ip_dialog == NULL);

	ip_dialog = gtk_dialog_new_with_buttons ("IP Input", GTK_WINDOW(main_window), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_STOCK_OK, GTK_RESPONSE_APPLY, NULL);
	gtk_window_set_icon(GTK_WINDOW(ip_dialog),application_icon_gdk_pixbuf);
	gtk_window_set_resizable(GTK_WINDOW(ip_dialog), FALSE);

	vbox = gtk_vbox_new (FALSE, 5);
	gtk_box_pack_start (GTK_BOX(GTK_DIALOG(ip_dialog)->vbox), vbox, FALSE, FALSE, 0);
	gtk_container_set_border_width (GTK_CONTAINER(vbox), 5);

	/* IP address */
	hbox = gtk_hbox_new (FALSE, 5);

		label = gtk_label_new ("IP address:");
		gtk_container_add (GTK_CONTAINER(hbox), label);

		ip_dialog_ip_addr = gtk_entry_new ();
		gtk_container_add (GTK_CONTAINER(hbox), ip_dialog_ip_addr);

	gtk_container_add (GTK_CONTAINER(vbox), hbox);

	/* IP port */
	hbox = gtk_hbox_new (FALSE, 5);

		label = gtk_label_new ("Port number:");
		gtk_container_add (GTK_CONTAINER(hbox), label);

		ip_dialog_ip_port = gtk_entry_new ();
		gtk_container_add (GTK_CONTAINER(hbox), ip_dialog_ip_port);

	gtk_container_add (GTK_CONTAINER(vbox), hbox);

	g_signal_connect(ip_dialog, "destroy",
			G_CALLBACK(on_ip_dialog_delete),
			NULL);
	g_signal_connect(ip_dialog, "delete-event",
			G_CALLBACK(on_ip_dialog_delete),
			NULL);
	g_signal_connect(ip_dialog, "response",
			G_CALLBACK(on_ip_dialog_response),
			NULL);

	gtk_dialog_set_default_response (GTK_DIALOG(ip_dialog), GTK_RESPONSE_APPLY);

	/* IP port */
	hbox = gtk_hbox_new (FALSE, 5);

		label = gtk_label_new ("Protocol:");
		gtk_container_add (GTK_CONTAINER(hbox), label);

		ip_dialog_protocol = gtk_combo_box_new_text ();
		gtk_combo_box_append_text (GTK_COMBO_BOX(ip_dialog_protocol), "UDP");
		gtk_combo_box_append_text (GTK_COMBO_BOX(ip_dialog_protocol), "RTP");
		gtk_container_add (GTK_CONTAINER(hbox), ip_dialog_protocol);
		gtk_combo_box_set_active (GTK_COMBO_BOX(ip_dialog_protocol), IP_PROTO_UDP);

	gtk_container_add (GTK_CONTAINER(vbox), hbox);
}

static void on_configuration_ip_input(GtkMenuItem *menuitem,gpointer user_data) {
	if (ip_dialog == NULL) create_ip_dialog();
	if (ip_dialog == NULL) {
		g_error("Unable to initialize IP config window");
		return;
	}

	update_ip_dialog_from_vars();
	gtk_widget_show_all(ip_dialog);
	gtk_window_present(GTK_WINDOW(ip_dialog));
	gtk_widget_grab_focus(ip_dialog);
}

static void on_configuration_x(GtkMenuItem *menuitem,gpointer user_data) {
	if (x_dialog == NULL) create_x_dialog();
	if (x_dialog == NULL) {
		g_error("Unable to initialize X config window");
		return;
	}

	update_x_dialog_from_vars();
	gtk_widget_show_all(x_dialog);
	gtk_window_present(GTK_WINDOW(x_dialog));
	gtk_widget_grab_focus(x_dialog);
}

static void on_configuration_audio(GtkMenuItem *menuitem,gpointer user_data) {
	if (audio_dialog == NULL) create_audio_dialog();
	if (audio_dialog == NULL) {
		g_error("Unable to initialize audio config window");
		return;
	}

	update_audio_dialog_from_vars();
	gtk_widget_show_all(audio_dialog);
	gtk_window_present(GTK_WINDOW(audio_dialog));
	gtk_widget_grab_focus(audio_dialog);
}

static void on_configuration_input(GtkMenuItem *menuitem,gpointer user_data) {
	if (input_dialog == NULL) create_input_dialog();
	if (input_dialog == NULL) {
		g_error("Unable to initialize input config window");
		return;
	}

	update_input_dialog_from_vars();
	gtk_widget_show_all(input_dialog);
	gtk_window_present(GTK_WINDOW(input_dialog));
	gtk_widget_grab_focus(input_dialog);
}

unsigned int client_area_adapicount = 0;
XvAdaptorInfo *client_area_adapi = NULL;

void client_area_check_source_size(int w,int h) {
	if (client_area_xvideo) {
		if (client_area_xvimage == NULL) {
			client_area_free();
			client_area_alloc();
			return;
		}

		/* changing the client area does not require reiniting the bitmap.
		 * changing the source frame size does */
		if (source_image_width == w && source_image_height == h)
			return;

		client_area_free();
		source_image_width = w;
		source_image_height = h;
		client_area_alloc();
	}
	else {
		/* changing the source frame does not require initing the bitmap,
		 * since our software YUV -> RGB does the rescale. it only
		 * matters if the client area changes */
		source_image_width = w;
		source_image_height = h;
	}
}

void client_area_free_queryadapters() {
	if (client_area_adapi) {
		XvFreeAdaptorInfo(client_area_adapi);
		client_area_adapi = NULL;
	}
	client_area_adapicount = 0;
}

XvAdaptorInfo *client_area_query_adapi() {
	if (client_area_adapi == 0)
		XvQueryAdaptors(client_area_display,client_area_xid,
			&client_area_adapicount,&client_area_adapi);

	return client_area_adapi;
}

void client_area_free() {
	if (client_area_xvideo) {
		if (client_area_xvideo_port >= 0)
			XvUngrabPort(client_area_display,client_area_xvideo_port,CurrentTime);
	}
	if (client_area_shminfo.shmid != 0 && client_area_shm) {
		XShmDetach(client_area_display,&client_area_shminfo);
	}
	if (client_area_gc != 0) {
		XFreeGC(client_area_display,client_area_gc);
		client_area_gc = 0;
	}	
	if (client_area_xvimage != NULL) {
		/* FIXME: Does this free the image, or just the XvImage */
		XFree(client_area_xvimage);
		client_area_xvimage = NULL;
	}
	if (client_area_image != NULL) {
		XDestroyImage(client_area_image);
		client_area_image = NULL;
	}
	if (client_area_shminfo.shmid != 0 && client_area_shm) {
		shmdt(client_area_shminfo.shmaddr);
		client_area_shminfo.shmid = 0;
	}
	client_area_xvideo_port = -1;
	client_area_xvideo = 0;
	client_area_shm = 0;
}

/* sw x sh = source rectangle (video)
 * dw x dh = destination rectangle (client area)
 * ar_n/ar_d = aspect ratio.
 *
 * note that we really don't use sw x sh because the source is scaled to fit.
 * It could be 720 x 480 scaled with 4:3 aspect ratio scaled to fit a 800x400
 * client area. */
void client_area_update_rects(int sw,int sh,int dw,int dh,int ar_n,int ar_d) {
	int if_dw,if_dh;

	if (ar_n > 0 && ar_d > 0) {
		{
			double f_dw,f_dh;

			/* fit width to client area, recalculate height by aspect ratio */
			f_dw = dw;
			f_dh = ((double)(dw * ar_d)) / ar_n;
			/* if the height is too big for the client area, clip height and scale down width */
			if (f_dh > dh) {
				f_dw = (f_dw * dh) / f_dh;
				f_dh = dh;
			}

			if_dw = (int)floor(f_dw);
			if_dh = (int)floor(f_dh);
		}

		client_area_video_x = (dw - if_dw) / 2;
		client_area_video_w = if_dw;
		client_area_video_y = (dh - if_dh) / 2;
		client_area_video_h = if_dh;
	}
	else {
		client_area_video_x = 0;
		client_area_video_w = dw;
		client_area_video_y = 0;
		client_area_video_h = dh;
	}
}

void client_area_redraw_source_frame(bool total) {
	video_should_redraw_t = -1;

	if (client_area_xvideo) {
		if (client_area_xvideo_port > 0) {
			if (total) client_area_draw_overlay_borders();
			if (client_area_shm) {
				/* draw the overlay. X might draw the chroma key color in the destination rectangle
				 * as part of it. For areas outside the overlay, we need to draw over them ourself */
				XvShmPutImage(client_area_display,client_area_xvideo_port,client_area_xid,
					client_area_gc,client_area_xvimage,
					/* source rect */
					0,0,source_image_width,source_image_height,
					/* dest rect */
					client_area_video_x,client_area_video_y,
					client_area_video_w,client_area_video_h,0);
			}
			else {
				XvPutImage(client_area_display,client_area_xvideo_port,client_area_xid,
					client_area_gc,client_area_xvimage,
					/* source rect */
					0,0,source_image_width,source_image_height,
					/* dest rect */
					client_area_video_x,client_area_video_y,
					client_area_video_w,client_area_video_h);
			}
		}
	}
	else {
		if (client_area_width > 0 && client_area_height > 0 && client_area_display && client_area_gc && client_area_image) {
			if (client_area_shm)
				XShmPutImage(client_area_display,client_area_xid,client_area_gc,client_area_image,
					0,0,0,0,client_area_width,client_area_height,0);
			else
				XPutImage(client_area_display,client_area_xid,client_area_gc,client_area_image,
					0,0,0,0,client_area_width,client_area_height);
		}
	}
}

void client_area_update_rects_again() {
	client_area_update_rects(source_image_width,source_image_height,client_area_width,client_area_height,
		client_area_aspect_n,client_area_aspect_d);
}

bool client_area_alloc_xv_common() {
	XvImageFormatValues *ifmt,*ifmt_exam;
	int ifmt_count = 0;
	XvAdaptorInfo *xa;
	int fmt_id = -1;
	XvFormat *fmt;
	int port;
	int i;

	client_area_xvideo = 0;
	client_area_xvideo_port = -1;
	if ((xa = client_area_query_adapi()) == NULL)
		return false;
	if (xvideo_adapter >= client_area_adapicount)
		return false;

	xa += xvideo_adapter; /* go the adaptor requested in the config */
	fprintf(stderr,"Requested adaptor %u/%u: Ports %u-%u Type 0x%X %u formats '%s'\n",
		(unsigned int)xvideo_adapter,(unsigned int)client_area_adapicount,
		(unsigned int)xa->base_id,(unsigned int)(xa->base_id+xa->num_ports-1),
		(unsigned int)xa->type,(unsigned int)xa->num_formats,
		xa->name);

	if (!(xa->type & XvInputMask)) {
		/* ^ Vague? Kinda.
		 *   "XvInputMask indicates the adapter can put video into a drawable"
		 *   "XvOutputMask indicates the adapter can get video from a drawable */
		fprintf(stderr,"XvVideo adapter is not the right type\n");
		return false;
	}

	/* Ugh, again an example of X's inconsistent naming. These are not "formats" they
	 * are visuals and depths. */
	if (xa->num_formats == 0 || xa->formats == NULL) {
		fprintf(stderr,"XvVideo adapter does not list ANY formats?\n");
		return false;
	}

	/* just pick the first depth */
	fmt = xa->formats;
	client_area_depth = fmt->depth;
	client_area_visual_id = fmt->visual_id;

	for (port=xa->base_id;(fmt_id == -1 || client_area_xvideo_port == -1) && port < (xa->base_id+xa->num_ports);port++) {
		/* what formats do you do? */
		fmt_id = -1;
		ifmt_count = 0;
		fprintf(stderr,"Trying port %d\n",port);
		if ((ifmt = XvListImageFormats(client_area_display,(XvPortID)port,&ifmt_count)) != NULL) {
			for (i=0;i < ifmt_count;i++) {
				ifmt_exam = ifmt + i;

				/* we are searching for: Planar YUV (3 planes for Y, Cb, Cr) 8 bits/sample, top to bottom */
				if (ifmt_exam->type == XvYUV && ifmt_exam->format == XvPlanar && ifmt_exam->num_planes == 3 &&
					ifmt_exam->y_sample_bits == 8 && ifmt_exam->u_sample_bits == 8 &&
					ifmt_exam->v_sample_bits == 8 && ifmt_exam->scanline_order == XvTopToBottom) {
					fmt_id = ifmt_exam->id;
					fprintf(stderr,"Found Planar YUV (order: %c %c %c)\n",
						ifmt_exam->component_order[0],
						ifmt_exam->component_order[1],
						ifmt_exam->component_order[2]);

					/* Different video cards have different ideas on the U & V planar order.
					 * So we have to adjust ourself to match----unless you're the Blue Man Group */
					client_area_y_plane_index = client_area_u_plane_index = client_area_v_plane_index = -1;
					for (int j=0;j < 4;j++) {
						char c = ifmt_exam->component_order[j];
						if (c == 'Y')
							client_area_y_plane_index = j;
						if (c == 'U')
							client_area_u_plane_index = j;
						if (c == 'V')
							client_area_v_plane_index = j;
					}

					if (client_area_y_plane_index < 0 || client_area_u_plane_index < 0 ||
						client_area_v_plane_index < 0) {
						fprintf(stderr,"Not all planes I need are defined\n");
						continue;
					}

					/* try to grab it */
					if (XvGrabPort(client_area_display,(XvPortID)port,CurrentTime) != Success) {
						fprintf(stderr,"Cannot acquire port %u\n",port);
						continue;
					}

					client_area_xv_format = *ifmt_exam;
					client_area_xvideo_port = port;
					break;
				}
			}
			XFree(ifmt);
		}
	}

	if (fmt_id < 0) {
		fprintf(stderr,"None of the ports have a satisfactory format\n");
		return false;
	}
	if (client_area_xvideo_port < 0) {
		fprintf(stderr,"Unable to acquire any port\n");
		return false;
	}

	client_area_fmt_id = fmt_id;
	fprintf(stderr,"XVideo: using port %d format 0x%X\n",client_area_xvideo_port,fmt_id);
	return true;
}

bool client_area_alloc_xv_shm() {
	if (!client_area_alloc_xv_common())
		return false;

	client_area_shm = 1;
	client_area_xvideo = 1;
	memset(&client_area_shminfo,0,sizeof(client_area_shminfo));
	client_area_xvimage = XvShmCreateImage(client_area_display,client_area_xvideo_port,
		client_area_fmt_id,NULL,
		source_image_width,source_image_height,&client_area_shminfo);
	if (client_area_xvimage == NULL) {
		fprintf(stderr,"Xv SHM create failed\n");
		client_area_free();
		return false;
	}

	client_area_shminfo.shmid = shmget(IPC_PRIVATE,
		client_area_xvimage->data_size + 16,
		IPC_CREAT | 0777);
	if (client_area_shminfo.shmid < 0) {
		fprintf(stderr,"Xv SHM shmget failed\n");
		client_area_free();
		return false;
	}
	client_area_shminfo.shmaddr = client_area_xvimage->data = (char*)shmat(client_area_shminfo.shmid,0,0);
	if (client_area_shminfo.shmaddr == (char*)-1) {
		client_area_free();
		return false;
	}
	client_area_shminfo.readOnly = 0;
	XShmAttach(client_area_display,&client_area_shminfo);
	XSync(client_area_display, 0);
	shmctl(client_area_shminfo.shmid, IPC_RMID, 0);

	fprintf(stderr,"Chosen format: %d x %d (%d bytes) %d-plane (Y/U/V @%u/%u @%u/%u @%u/%u)\n",
		(int)client_area_xvimage->width,	(int)client_area_xvimage->height,
		(int)client_area_xvimage->data_size,	(int)client_area_xvimage->num_planes,
		(int)client_area_xvimage->offsets[0],	(int)client_area_xvimage->pitches[0],
		(int)client_area_xvimage->offsets[1],	(int)client_area_xvimage->pitches[1],
		(int)client_area_xvimage->offsets[2],	(int)client_area_xvimage->pitches[2]);

	client_area_gc = XCreateGC(client_area_display,(Drawable)client_area_xid,0UL,NULL);
	if (client_area_gc == NULL) {
		client_area_free();
		return false;
	}

	return true;
}

bool client_area_alloc_x_shm() {
	client_area_shm = 1;
	memset(&client_area_shminfo,0,sizeof(client_area_shminfo));
	client_area_image = XShmCreateImage(client_area_display,DefaultVisual(client_area_display,client_area_screen),
		DefaultDepth(client_area_display,client_area_screen),ZPixmap,NULL,&client_area_shminfo,
		client_area_width,client_area_height);
	if (client_area_image == NULL) {
		client_area_free();
		return false;
	}
	client_area_shminfo.shmid = shmget(IPC_PRIVATE,
		client_area_image->bytes_per_line * client_area_image->height,
		IPC_CREAT | 0777);
	if (client_area_shminfo.shmid < 0) {
		client_area_free();
		return false;
	}
	client_area_shminfo.shmaddr = client_area_image->data = (char*)shmat(client_area_shminfo.shmid,0,0);
	if (client_area_shminfo.shmaddr == (char*)-1) {
		client_area_free();
		return false;
	}
	client_area_shminfo.readOnly = 0;
	XShmAttach(client_area_display,&client_area_shminfo);
	XSync(client_area_display, 0);
	shmctl(client_area_shminfo.shmid, IPC_RMID, 0);
	client_area_gc = XCreateGC(client_area_display,(Drawable)client_area_xid,0UL,NULL);
	if (client_area_gc == NULL) {
		client_area_free();
		return false;
	}

	return true;
}

bool client_area_alloc_xv() {
	if (!client_area_alloc_xv_common())
		return false;

	client_area_shm = 0;
	client_area_xvideo = 1;
	client_area_xvimage = XvCreateImage(client_area_display,client_area_xvideo_port,
		client_area_fmt_id,NULL,
		source_image_width,source_image_height);
	if (client_area_xvimage == NULL) {
		fprintf(stderr,"Cannot create non-SHM image\n");
		client_area_free();
		return false;
	}

	fprintf(stderr,"Chosen format: %d x %d (%d bytes) %d-plane (Y/U/V @%u/%u @%u/%u @%u/%u)\n",
		(int)client_area_xvimage->width,	(int)client_area_xvimage->height,
		(int)client_area_xvimage->data_size,	(int)client_area_xvimage->num_planes,
		(int)client_area_xvimage->offsets[0],	(int)client_area_xvimage->pitches[0],
		(int)client_area_xvimage->offsets[1],	(int)client_area_xvimage->pitches[1],
		(int)client_area_xvimage->offsets[2],	(int)client_area_xvimage->pitches[2]);

	client_area_gc = XCreateGC(client_area_display,(Drawable)client_area_xid,0UL,NULL);
	if (client_area_gc == NULL) {
		fprintf(stderr,"Cannot alloc non-SHM GC\n");
		client_area_free();
		return false;
	}

	assert(client_area_xvimage->data == NULL);
	client_area_xvimage->data = (char*)malloc(client_area_xvimage->data_size + 16);
	if (client_area_xvimage->data == NULL) {
		fprintf(stderr,"Cannot malloc non-SHM image\n");
		client_area_free();
		return false;
	}

	return true;
}

bool client_area_alloc_x() {
	client_area_shm = 0;
	client_area_image = XCreateImage(client_area_display,DefaultVisual(client_area_display,client_area_screen),
		DefaultDepth(client_area_display,client_area_screen),ZPixmap,0,NULL,client_area_width,client_area_height,32,0);
	if (client_area_image == NULL) {
		client_area_free();
		return false;
	}
	client_area_gc = XCreateGC(client_area_display,(Drawable)client_area_xid,0UL,NULL);
	if (client_area_gc == NULL) {
		client_area_free();
		return false;
	}

	assert(client_area_image->data == NULL);
	assert(client_area_image->height != 0);
	assert(client_area_image->bytes_per_line != 0);
	client_area_image->data = (char*)malloc((client_area_image->bytes_per_line * client_area_image->height) + 16);
	if (client_area_image->data == NULL) {
		client_area_free();
		return false;
	}

	return true;
}

void client_area_alloc() {
	bool ok = false;

	client_area_shm = 0;
	client_area_xvideo = 0;

	if (!ok && use_xvideo) {
		if (!ok && use_x_shm) ok = client_area_alloc_xv_shm();
		if (!ok) ok = client_area_alloc_xv();
	}

	if (!ok && use_x_shm) ok = client_area_alloc_x_shm();
	if (!ok) ok = client_area_alloc_x();

	if (!ok) fprintf(stderr,"Cannot allocate any X video\n");
}

/* such as: resize, move, etc. */
static void on_main_window_configure_event(GtkWidget *widget,GdkEvent *event,gpointer user_data) {
	if (client_area_xvideo) {
		/* we need to know of these events if an actual hardware overlay is involved */
		int nw = -1,nh = -1,x = 0,y = 0,border = 0,depth = 0;
		Window root;

		/* NOTE: The params are actually defined as returning unsigned int, but, meh, same difference */
		XGetGeometry(client_area_display,client_area_xid,&root,&x,&y,
			(unsigned int*)(&nw),(unsigned int*)(&nh),
			(unsigned int*)(&border),(unsigned int*)(&depth));

		/* XVideo: we normally just emit the video as-is from the source since XVideo ports do the scaling
		 *         in hardware. */
		if (client_area_xvideo) {
			client_area_width = nw;
			client_area_height = nh;

			/* TODO: Aspect ratio fitting */
			if (client_area_xvideo_port > 0) {
				if (client_area_shm)
					XvShmPutImage(client_area_display,client_area_xvideo_port,client_area_xid,
						client_area_gc,client_area_xvimage,
						0,0,source_image_width,source_image_height,
						0,0,client_area_width,client_area_height,0);
				else
					XvPutImage(client_area_display,client_area_xvideo_port,client_area_xid,
						client_area_gc,client_area_xvimage,
						0,0,source_image_width,source_image_height,
						0,0,client_area_width,client_area_height);
			}
		}
	}
}

void client_area_draw_overlay_borders() {
	if (client_area_video_w >= client_area_width && client_area_video_h >= client_area_height)
		return;

	/* make sure the fill style is solid black */
	XSetForeground(client_area_display,client_area_gc,0);
	XSetFillStyle(client_area_display,client_area_gc,FillSolid);

	/* Top bar */
	if (client_area_video_y > 0)
		XFillRectangle(client_area_display,client_area_xid,client_area_gc,0,0,client_area_width,client_area_video_y);
	/* Bottom bar */
	if ((client_area_video_y+client_area_video_h) < client_area_height)
		XFillRectangle(client_area_display,client_area_xid,client_area_gc,0,client_area_video_y+client_area_video_h,
			client_area_width,client_area_height-(client_area_video_y+client_area_video_h));
	/* Left pillar */
	if (client_area_video_x > 0)
		XFillRectangle(client_area_display,client_area_xid,client_area_gc,0,client_area_video_y,
			client_area_video_x,client_area_video_h);
	/* Right pillar */
	if ((client_area_video_x+client_area_video_w) < client_area_width)
		XFillRectangle(client_area_display,client_area_xid,client_area_gc,
			client_area_video_x+client_area_video_w,client_area_video_y,
			client_area_width-(client_area_video_x+client_area_video_w),client_area_video_h);

}

static void on_main_window_video_expose(GtkWidget *widget,GdkEvent *event,gpointer user_data) {
	int nw = -1,nh = -1,x = 0,y = 0,border = 0,depth = 0;
	Window root;

	if (pthread_mutex_lock(&global_mutex) == 0) {
		/* NOTE: The params are actually defined as returning unsigned int, but, meh, same difference */
		XGetGeometry(client_area_display,client_area_xid,&root,&x,&y,
				(unsigned int*)(&nw),(unsigned int*)(&nh),
				(unsigned int*)(&border),(unsigned int*)(&depth));

		client_area_update_rects(source_image_width,source_image_height,nw,nh,
				client_area_aspect_n,client_area_aspect_d);

		/* XVideo: we normally just emit the video as-is from the source since XVideo ports do the scaling
		 *         in hardware for us. */
		if (client_area_xvideo) {
			client_area_width = nw;
			client_area_height = nh;

			if (client_area_xvideo_port > 0) {
				client_area_draw_overlay_borders();
				if (client_area_shm) {
					/* draw the overlay. X might draw the chroma key color in the destination rectangle
					 * as part of it. For areas outside the overlay, we need to draw over them ourself */
					XvShmPutImage(client_area_display,client_area_xvideo_port,client_area_xid,
							client_area_gc,client_area_xvimage,
							/* source rect */
							0,0,source_image_width,source_image_height,
							/* dest rect */
							client_area_video_x,client_area_video_y,
							client_area_video_w,client_area_video_h,0);
				}
				else {
					XvPutImage(client_area_display,client_area_xvideo_port,client_area_xid,
							client_area_gc,client_area_xvimage,
							/* source rect */
							0,0,source_image_width,source_image_height,
							/* dest rect */
							client_area_video_x,client_area_video_y,
							client_area_video_w,client_area_video_h);
				}
			}
		}
		else {
			if (client_area_width <= 0 || client_area_height <= 0) {
				client_area_width = nw;
				client_area_height = nh;
				client_area_alloc();
				generate_non_video_frame();
				draw_osd();
			}
			else if (nw != client_area_width || nh != client_area_height) {
				client_area_free();
				client_area_width = nw;
				client_area_height = nh;
				client_area_alloc();
				generate_non_video_frame();
				draw_osd();
			}

			if (client_area_width > 0 && client_area_height > 0 && client_area_display && client_area_gc && client_area_image) {
				if (!client_area_xvideo && CurrentInput >= VIEW_INPUT_FILE) {
					/* delay redrawing, the live feed or video playback will cause redraw anyway */
					if (video_should_redraw_t < 0)
						video_should_redraw_t = NOW + 0.1;
				}
				else {
					/* video output is still, redraw now */
					client_area_redraw_source_frame(true);
				}
			}
		}

		pthread_mutex_unlock(&global_mutex);
	}
}

static void on_configuration_save_config(GtkMenuItem *menuitem,gpointer user_data) {
	save_configuration();
}

static void activate_action (GtkAction *action)
{
	const gchar *name = gtk_action_get_name (action);
	const gchar *typename1 = G_OBJECT_TYPE_NAME (action);

	GtkWidget *dialog;

	dialog = gtk_message_dialog_new (GTK_WINDOW (main_window),
			GTK_DIALOG_DESTROY_WITH_PARENT,
			GTK_MESSAGE_INFO,
			GTK_BUTTONS_CLOSE,
			"You activated action: \"%s\" of type \"%s\"",
			name, typename1);

	/* Close dialog on user response */
	g_signal_connect (dialog,
			"response",
			G_CALLBACK (gtk_widget_destroy),
			NULL);

	gtk_widget_show (dialog);
}

static void activate_radio_action (GtkAction *action, GtkRadioAction *current)
{
	const gchar *name = gtk_action_get_name (GTK_ACTION (current));
	const gchar *typename1 = G_OBJECT_TYPE_NAME (GTK_ACTION (current));
	gboolean active = gtk_toggle_action_get_active (GTK_TOGGLE_ACTION (current));
	gint value = gtk_radio_action_get_current_value (GTK_RADIO_ACTION (current));

	if (active)
	{
		gchar *text;

		text = g_strdup_printf ("You activated radio action: \"%s\" of type \"%s\".\n"
				"Current value: %d",
				name, typename1, value);
		gtk_label_set_text (GTK_LABEL (messagelabel), text);
#ifdef GTK_HAS_INFO_BAR
		gtk_info_bar_set_message_type (GTK_INFO_BAR (infobar), (GtkMessageType)value); /* FIXME: When did this appear in the GTK+? */
		gtk_widget_show (infobar);
#endif
		g_free (text);
	}
}

static gboolean main_window_ticker(gpointer data) {
	update_now_time();
	if (pthread_mutex_trylock(&global_mutex) == 0) {
		int i;

		for (i=0;i < VIEW_INPUT_MAX;i++)
			Input[i]->idle();

		if (do_video_source(CurrentInputObj(),false))
			client_area_redraw_source_frame(false);

		/* the client_area_redraw_source_frame() function resets this timeout,
		 * so if we crossed it regardless, the video source isn't moving and
		 * we should put the colorbars up */
		if (video_should_redraw_t >= 0.0 && NOW >= video_should_redraw_t) {
			if (!do_video_source(CurrentInputObj(),true)) {
				generate_non_video_frame();
				draw_osd();
			}

			client_area_redraw_source_frame(true);
		}

		pthread_mutex_unlock(&global_mutex);
	}

	return true;
}

static void on_main_window_help_about(GtkAction *action, GtkWidget *window)
{
	static const char *authors[] = {
		"Jonathan Campbell",
		"Nathan Bosseler",
		NULL
	};

	static const char *documentors[] = {
		"<TODO>",
		NULL
	};

	static const char *license =
		"This program does not yet have a license.\n"
		"But it will soon someday...\n";

	gtk_show_about_dialog (GTK_WINDOW (main_window),
			"program-name", "Video project",
			"version", "v0.1",
			"copyright", "(C) 2011-2015 Jonathan Campbell",
			"license", license,
			"website", "http://hackipedia.org",
			"comments", "Video capture and archiving project",
			"authors", authors,
			"documenters", documentors,
			"logo", application_icon_gdk_pixbuf,
			"title", "About Video Project",
			NULL);
}

/******************************************************************************
 * GTK+ Action Assignment list.
 * Contrary to appearance each item becomes one full GtkActionEntry struct,
 * the unspecified fields are left NULL by the compiler.
 *
 * Reference:
 *
 * struct GtkActionEntry {
 *   const gchar     *name;			<- name of the action (the ID in the UI description XML)
 *   const gchar     *stock_id;                 <- stock ID or name of an icon from the icon theme
 *   const gchar     *label;                    <- label given to the action, or if NULL, label given by stock ID -> item in theme
 *   const gchar     *accelerator;              <- keyboard shortcut that can trigger the action
 *   const gchar     *tooltip;                  <- tooltip to show when hovering over whatever widget is assigned to the action
 *   GCallback        callback;                 <- callback function when action is activated
 * };
 ******************************************************************************/
static GtkActionEntry gtk_all_actions[] = {
/*	.name			.stock_id		.label
 *		.accelerator		.tooltip
 *			.callback */	
	{"FileMenu",		NULL,			"_File"},

	{"OpenMenu",		NULL,			"_Open"},

	{"ViewMenu",		NULL,			"_View"},

	{"ViewMenuAspectRatio",	NULL,			"_Aspect Ratio"},

	{"ViewMenuToolbars",	NULL,			"_Toolbars"},

	{"ViewMenuInput",	NULL,			"_Input"},

	{"ControlMenu",		NULL,			"_Control"},

	{"PreferencesMenu",	NULL,			"_Preferences"},

	{"ColorMenu",		NULL,			"_Color"},

	{"ShapeMenu",		NULL,			"_Shape"},

	{"HelpMenu",		NULL,			"_Help"},

	{"InputMenu",		NULL,			"_Input"},

	{"InputIPMenu",		NULL,			"_IP"},

	{"InputSelectMenu",	NULL,			"_Select"},

	{"InputAnalogMenu",	NULL,			"_Analog"},

	{"SourceMenu",		NULL,			"_Source"},

	{"ConfigMenu",		NULL,			"_Configuration"},

	{"OpenCapture",		GTK_STOCK_OPEN,		"_Open Capture",
		"<control>O",		"Open existing capture",
			G_CALLBACK (on_file_capture_open)},

	{"ConfigMenu_IP",	NULL,			"_IP input",
		"<control><shift>I",	"Configure IP input",
			G_CALLBACK (on_configuration_ip_input)},

	{"ConfigMenu_X",	NULL,			"_X Windows video display",
		"<control><shift>X",	"Configure how this program communicates with your X Windows desktop",
			G_CALLBACK (on_configuration_x)},

	{"ConfigMenu_Audio",	NULL,			"_Audio capture settings",
		"<control><shift>A",	"Configure audio capture associated with this input",
			G_CALLBACK (on_configuration_audio)},

	{"ConfigMenu_Input",	NULL,			"_Input capture settings",
		"<control><shift>I",	"Configure input capture associated with this input",
			G_CALLBACK (on_configuration_input)},

	{"ConfigMenu_Save",	NULL,			"_Save configuration",
		"<control><shift>S",	"Save configuration to local dir",
			G_CALLBACK (on_configuration_save_config)},

	{"Device Select",	NULL,			"_Device Select",
		"<control>D",		"Device Select",
			G_CALLBACK (on_main_window_device_select)},

	{"Quit",		GTK_STOCK_QUIT,		"_Quit",
		"<control>Q",		"Quit",
			G_CALLBACK (on_main_window_file_quit)},

	{"About",		NULL,			"_About",
		"<control>A",		"About",
			G_CALLBACK (on_main_window_help_about)},

	{"Logo",		"demo-gtk-logo",	NULL,
		NULL,			"GTK+",
			G_CALLBACK (activate_action)}
};

static const gchar *ui_info =
"<ui>"
"  <menubar name='MenuBar'>"
"    <menu action='FileMenu'>"
"      <menuitem action='OpenCapture'/>"
#if 0
"      <menuitem action='ConvertVideo'/>"
#endif
"      <separator/>"
"      <menuitem action='Quit'/>"
"    </menu>"
"    <menu action='ViewMenu'>"
"      <menu action='ViewMenuToolbars'>"
"	 <menuitem action='ViewMenuToolbars_Toolbar'/>"
"	 <menuitem action='ViewMenuToolbars_Metadata'/>"
"	 <menuitem action='ViewMenuToolbars_Status'/>"
"      </menu>"
"      <menu action='ViewMenuInput'>"
"	 <menuitem action='ViewMenuInput_None'/>"
"	 <menuitem action='ViewMenuInput_File'/>"
"	 <menuitem action='ViewMenuInput_Input1'/>"
"	 <menuitem action='ViewMenuInput_Input2'/>"
"	 <menuitem action='ViewMenuInput_Input3'/>"
"	 <menuitem action='ViewMenuInput_Input4'/>"
"	 <menuitem action='ViewMenuInput_InputIP'/>"
"      </menu>"
"      <menu action='ViewMenuAspectRatio'>"
"	 <menuitem action='ViewMenuAspectRatio_None'/>"
"	 <menuitem action='ViewMenuAspectRatio_4x3'/>"
"	 <menuitem action='ViewMenuAspectRatio_16x9'/>"
"	 <menuitem action='ViewMenuAspectRatio_Dont'/>"
"      </menu>"
"      <menuitem action='ViewMenuOSD'/>"
"    </menu>"
"    <menu action='ControlMenu'>"
"      <menuitem action='ControlMenu_Record'/>"
"      <separator/>"
"      <menuitem action='ControlMenu_Play'/>"
"      <menuitem action='ControlMenu_Pause'/>"
"      <menuitem action='ControlMenu_Stop'/>"
"    </menu>"
"    <menu action='ConfigMenu'>"
"      <menuitem action='ConfigMenu_IP'/>"
"      <separator/>"
"      <menuitem action='ConfigMenu_X'/>"
"      <separator/>"
"      <menuitem action='ConfigMenu_Audio'/>"
"      <separator/>"
"      <menuitem action='ConfigMenu_Input'/>"
"      <separator/>"
"      <menuitem action='ConfigMenu_Save'/>"
"    </menu>"
"    <menu action='HelpMenu'>"
"      <menuitem action='About'/>"
"    </menu>"
"  </menubar>"
"  <toolbar name='ToolBar'>"
"    <toolitem action='Record'/>"
"    <separator action='Sep1'/>"
"    <toolitem action='Play'/>"
"    <toolitem action='Pause'/>"
"    <toolitem action='Stop'/>"
"    <separator action='Sep2'/>"
"    <toolitem action='ViewInput_None'/>"
"    <toolitem action='ViewInput_File'/>"
"    <toolitem action='ViewInput_Input1'/>"
"    <toolitem action='ViewInput_Input2'/>"
"    <toolitem action='ViewInput_Input3'/>"
"    <toolitem action='ViewInput_Input4'/>"
"    <toolitem action='ViewInput_InputIP'/>"
"  </toolbar>"
"</ui>";

static gint on_video_key_press_event(GtkWidget *widget, GdkEventKey *ev) {
	/* A-Z and spacebar generate corresponding ASCII codes.
	 * Why the hell don't you GTK+ developers just say so?!? */
	if (ev->keyval == ' ') {
		/* Spacebar starts/pauses playback, Final Cut Pro style */
		if (CurrentInputObj()->Playing) {
			if (CurrentInputObj()->Paused)
				CurrentInputObj()->start_playback();
			else
				CurrentInputObj()->pause_playback();
		}
		/* if not playing, then start playing */
		else {
			CurrentInputObj()->start_playback();
		}

		update_ui();
	}
	else if (ev->keyval == GDK_KEY_Escape) {
		if (CurrentInputObj()->Playing) {
			CurrentInputObj()->stop_playback();
			update_ui();
		}
	}
	else if (ev->keyval == 'R') {
		if (!CurrentInputObj()->Recording)
			CurrentInputObj()->start_recording();
		else if (ui_ask_stop_recording())
			CurrentInputObj()->stop_recording();

		update_ui();

	}
	else if (ev->keyval == 'F') {
		if (CurrentInput != VIEW_INPUT_FILE) {
			switch_input(VIEW_INPUT_FILE);
			update_ui();
		}
	}
	else if (ev->keyval == 'I') {
		if (CurrentInput != VIEW_INPUT_IP) {
			switch_input(VIEW_INPUT_IP);
			update_ui();
		}
	}
	else if (ev->keyval >= '1' && ev->keyval <= '4') {
		int target = (ev->keyval - '1') + VIEW_INPUT_1;
		if (CurrentInput != target) {
			switch_input(target);
			update_ui();
		}
	}
	else if (ev->keyval == GDK_KEY_Left || ev->keyval == GDK_KEY_Right) {
		InputManager *in = CurrentInputObj();
		video_tracking *trk = (in->play_is_rec ? &in->vt_rec : &in->vt_play);

		if (in->Playing) {
			if (in->Paused) {
				unsigned int onesec = ((trk->video_rate_n + trk->video_rate_d - 1) / trk->video_rate_d);
				unsigned int step = 1;
				
				if ((ev->state & GDK_SHIFT_MASK) && (ev->state & GDK_CONTROL_MASK))
					step = onesec * 15;
				else if (ev->state & GDK_CONTROL_MASK)
					step = onesec * 5;
				else if (ev->state & GDK_SHIFT_MASK)
					step = onesec;

				/* TODO: This should be a member of the InputManager class */
				/* TODO: When we get to the beginning of the AVI, switch to the chronologically previous AVI */
				/* TODO: When we get to the end of the AVI, switch to the chronologically next AVI, or to "stop" mode */
				if (ev->keyval == GDK_KEY_Left) {
					if (in->playback_base_frame > trk->video_first_frame) {
						trk->video_current_frame = -1LL;
						in->playback_base_frame -= step;
						if (in->playback_base_frame < trk->video_first_frame)
							in->playback_base_frame = trk->video_first_frame;
					}
				}
				else if (ev->keyval == GDK_KEY_Right) {
					if (in->playback_base_frame < trk->video_total_frames) {
						in->playback_base_frame += step;
						if (in->playback_base_frame > trk->video_total_frames)
							in->playback_base_frame = trk->video_total_frames;
					}
				}
			}
			else {
				in->pause_playback();
				update_ui();
			}
		}
		else {
			in->pause_playback();
			update_ui();
		}

		trk->video_current_frame = -1LL;
	}
	else if (ev->keyval == '.') { /* un-shifted '>' key, increase speed */
		InputManager *in = CurrentInputObj();
		video_tracking *trk = (in->play_is_rec ? &in->vt_rec : &in->vt_play);

		if (in->Playing && !in->Paused) {
			in->increase_speed();
		}
		else {
			if (!in->Playing) {
				in->pause_playback();
			}
			else if (in->Paused) {
				in->start_playback();
				in->ChangeSpeed(1,4);
			}
			update_ui();
		}

		trk->video_current_frame = -1LL;
	}
	else if (ev->keyval == ',') { /* un-shifted '<' key, decrease speed */
		InputManager *in = CurrentInputObj();
		video_tracking *trk = (in->play_is_rec ? &in->vt_rec : &in->vt_play);

		if (in->Playing && !in->Paused) {
			in->decrease_speed();
		}
		else {
			if (!in->Playing) {
				in->pause_playback();
			}
			else if (in->Paused) {
				in->start_playback();
				in->ChangeSpeed(-1,1);
			}
			update_ui();
		}

		trk->video_current_frame = -1LL;
	}
	else {
		/* DEBUG */
		fprintf(stderr,"Keypress %X\n",ev->keyval);
	}
}

static gint on_video_button_press_event(GtkWidget *widget, GdkEventButton *ev) {
	gtk_widget_grab_focus (main_window_contents); /* Keep keyboard focus HERE please */
	fprintf(stderr,"Button event\n");
	return TRUE;
}

/******************************************************************************
 * init_main_window
 *
 * Create and initialize the main window and all widgets inside.
 *
 * Inputs: none
 * Outputs: none
 * Returns: success (0) or failure (-1)
 * Scope: Global, process-wide
 * Global variables used: application_icon_gdk_pixbuf
 * Global variables modified: main_window (FIXME: more)
 *
 * main_window
 *   +--- table
 *
 * table: [1 x 6]
 *   Not all widgets are visible at all times, but they are there.
 *
 *                                            Y
 *   +--------------------------------------+ 0
 *   | menu                                 |
 *   +--------------------------------------+ 1
 *   | toolbar                              |
 *   +--------------------------------------+ 2
 *   | infobar                              |
 *   +--------------------------------------+ 3
 *   | vertical GTK pane                    |
 *   +--------------------------------------+ 4
 *   | status bar                           |
 *   +--------------------------------------+ 5
 *
 * vertical GTK pane:
 *
 *   +--------------------------------------+
 *   | eventbox                             |
 *   +--------------------------------------+
 *   | contents info aka metadata           |
 *   +--------------------------------------+
 *
 *   infobar
 *     +-- messagelabel
 *
 *   eventbox
 *     +-- X-Window used for video playback
 *
 *   contents info
 *     +-- list view
 *          +-- column 1 & 2 (name: value)
 *
 * NOTES: The infobar is normally invisible until we choose to show a message,
 *        then it will pop down and consume some 20-30 pixels vertically until
 *        dismissed.
 *
 * TODO: Make it possible for the user to hide/show the listbox. Analog video
 *       capture does not carry positional information.
 * TODO: Make it possible to adjust the listbox using GTK paned widgets.
 ******************************************************************************/
static int init_main_window()
{
	GtkWidget *tmp1;
	GtkWidget *table;
	GtkWidget *contents;
	GtkWidget *contents_info;
	GtkWidget *sw;
	GtkWidget *tmp,*tmpi;
	GtkWidget *bar;
	GtkWidget *contents_info_list_view;
	GtkWidget *vpane;
	GSList *gslist;
	GtkListStore *contents_info_list_store;
	GtkActionGroup *action_group;
	GtkToggleAction *tog_act;
	GtkRadioAction *rad_act;
	GtkAction *open_action;
	GError *error = NULL;
	int i;

	/* Create the toplevel window
	*/

	main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW (main_window), "Video project");
	gtk_window_set_icon(GTK_WINDOW(main_window),application_icon_gdk_pixbuf);

	/* NULL window variable when window is closed */
	g_signal_connect(main_window, "destroy",
			G_CALLBACK(on_main_window_file_destroy),
			NULL);
	g_signal_connect(main_window, "delete-event",
			G_CALLBACK(on_main_window_file_quit),
			NULL);

	table = gtk_table_new(1, 5, FALSE);

	gtk_container_add(GTK_CONTAINER (main_window), table);

	/* Create the menubar and toolbar
	*/

	action_group = gtk_action_group_new ("AppWindowActions");
	gtk_action_group_add_actions(action_group,
			gtk_all_actions, G_N_ELEMENTS(gtk_all_actions),
			main_window);

	/* View -> Toolbars ... */
	tog_act = gtk_toggle_action_new ("ViewMenuToolbars_Toolbar", "_Toolbar", NULL, NULL);
	gtk_toggle_action_set_active (tog_act, cfg_show_toolbar);
	gtk_action_group_add_action (action_group, GTK_ACTION(tog_act));
	g_signal_connect(tog_act, "toggled",
			G_CALLBACK(on_main_window_view_toolbars_toolbar),
			(void*)("/MenuBar/ViewMenu/ViewMenuToolbars/ViewMenuToolbars_Toolbar"));

	tog_act = gtk_toggle_action_new ("ViewMenuToolbars_Metadata", "_Metadata", NULL, NULL);
	gtk_toggle_action_set_active (tog_act, cfg_show_metadata);
	gtk_action_group_add_action (action_group, GTK_ACTION(tog_act));
	g_signal_connect(tog_act, "toggled",
			G_CALLBACK(on_main_window_view_toolbars_metadata),
			(void*)("/MenuBar/ViewMenu/ViewMenuToolbars/ViewMenuToolbars_Metadata"));

	tog_act = gtk_toggle_action_new ("ViewMenuToolbars_Status", "_Status bar", NULL, NULL);
	gtk_toggle_action_set_active (tog_act, cfg_show_status);
	gtk_action_group_add_action (action_group, GTK_ACTION(tog_act));
	g_signal_connect(tog_act, "toggled",
			G_CALLBACK(on_main_window_view_toolbars_status),
			(void*)("/MenuBar/ViewMenu/ViewMenuToolbars/ViewMenuToolbars_Status"));

	/* Control menu */
	tog_act = gtk_toggle_action_new ("ControlMenu_Record", "_Record", NULL, NULL);
	gtk_action_group_add_action (action_group, GTK_ACTION(tog_act));
	g_signal_connect(tog_act, "toggled",
			G_CALLBACK(on_main_window_control_record),
			(void*)("/MenuBar/ControlMenu/ControlMenu_Record"));

	rad_act = gtk_radio_action_new ("ControlMenu_Play", "_Play", NULL, NULL, 1);
	gtk_radio_action_set_group (rad_act, NULL);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_control_play),
			(void*)("/MenuBar/ControlMenu/ControlMenu_Play"));

	rad_act = gtk_radio_action_new ("ControlMenu_Pause", "_Pause", NULL, NULL, 0);
	gtk_radio_action_set_group (rad_act, gslist);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_control_pause),
			(void*)("/MenuBar/ControlMenu/ControlMenu_Pause"));

	rad_act = gtk_radio_action_new ("ControlMenu_Stop", "_Stop", NULL, NULL, 0);
	gtk_radio_action_set_group (rad_act, gslist);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_control_stop),
			(void*)("/MenuBar/ControlMenu/ControlMenu_Stop"));

	/* View aspect ratio */
	rad_act = gtk_radio_action_new ("ViewMenuAspectRatio_None", "Match _Source", NULL, NULL, 0);
	gtk_radio_action_set_group (rad_act, NULL);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_view_aspect_ratio_select),
			(void*)0);
	main_window_view_aspect_action = rad_act;

	rad_act = gtk_radio_action_new ("ViewMenuAspectRatio_4x3", "_4:3", NULL, NULL, 1);
	gtk_radio_action_set_group (rad_act, gslist);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_view_aspect_ratio_select),
			(void*)4);

	rad_act = gtk_radio_action_new ("ViewMenuAspectRatio_16x9", "_16:9", NULL, NULL, 2);
	gtk_radio_action_set_group (rad_act, gslist);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_view_aspect_ratio_select),
			(void*)16);

	rad_act = gtk_radio_action_new ("ViewMenuAspectRatio_Dont", "_Stretch", NULL, NULL, -1);
	gtk_radio_action_set_group (rad_act, gslist);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_view_aspect_ratio_select),
			(void*)16);

	/* View input menu */
	rad_act = gtk_radio_action_new ("ViewMenuInput_None", "_None", "Switch input view off", NULL, VIEW_INPUT_OFF);
	gtk_radio_action_set_group (rad_act, NULL);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_view_input_select),
			NULL);
	main_window_view_input_action = rad_act;

	rad_act = gtk_radio_action_new ("ViewMenuInput_File", "_File", "Switch to file input", NULL, VIEW_INPUT_FILE);
	gtk_radio_action_set_group (rad_act, gslist);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_view_input_select),
			NULL);

	rad_act = gtk_radio_action_new ("ViewMenuInput_Input1", "Input _1", "Switch to input #1", NULL, VIEW_INPUT_1);
	gtk_radio_action_set_group (rad_act, gslist);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_view_input_select),
			NULL);

	rad_act = gtk_radio_action_new ("ViewMenuInput_Input2", "Input _2", "Switch to input #2", NULL, VIEW_INPUT_2);
	gtk_radio_action_set_group (rad_act, gslist);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_view_input_select),
			NULL);

	rad_act = gtk_radio_action_new ("ViewMenuInput_Input3", "Input _3", "Switch to input #3", NULL, VIEW_INPUT_3);
	gtk_radio_action_set_group (rad_act, gslist);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_view_input_select),
			NULL);

	rad_act = gtk_radio_action_new ("ViewMenuInput_Input4", "Input _4", "Switch to input #4", NULL, VIEW_INPUT_4);
	gtk_radio_action_set_group (rad_act, gslist);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_view_input_select),
			NULL);

	rad_act = gtk_radio_action_new ("ViewMenuInput_InputIP", "_IP", "Switch to IP input", NULL, VIEW_INPUT_IP);
	gtk_radio_action_set_group (rad_act, gslist);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_view_input_select),
			NULL);

	/* Record button */
	tog_act = gtk_toggle_action_new ("Record", "_Record", NULL, NULL);
	gtk_action_group_add_action (action_group, GTK_ACTION(tog_act));
	g_signal_connect(tog_act, "toggled",
			G_CALLBACK(on_main_window_record),
			NULL);

	/* Play button */
	rad_act = gtk_radio_action_new ("Play", "_Play", NULL, NULL, 1);
	gtk_radio_action_set_group (rad_act, NULL);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_play),
			NULL);

	/* Pause button */
	rad_act = gtk_radio_action_new ("Pause", "_Pause", NULL, NULL, 0);
	gtk_radio_action_set_group (rad_act, gslist);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_pause),
			NULL);

	/* Stop button */
	rad_act = gtk_radio_action_new ("Stop", "_Stop", NULL, NULL, 0);
	gtk_radio_action_set_group (rad_act, gslist);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_stop),
			NULL);

	/* Input: None button */
	rad_act = gtk_radio_action_new ("ViewInput_None", "_None", "Switch input view off", NULL, VIEW_INPUT_OFF);
	gtk_radio_action_set_group (rad_act, NULL);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_input_select),
			NULL);
	main_window_toolbar_input_action = rad_act;

	/* Input: File button */
	rad_act = gtk_radio_action_new ("ViewInput_File", "_File", "Switch to file input", NULL, VIEW_INPUT_FILE);
	gtk_radio_action_set_group (rad_act, gslist);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_input_select),
			NULL);

	/* Input: Input 1 button */
	rad_act = gtk_radio_action_new ("ViewInput_Input1", " 1 ", "Switch to input #1", NULL, VIEW_INPUT_1);
	gtk_radio_action_set_group (rad_act, gslist);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_input_select),
			NULL);

	/* Input: Input 2 button */
	rad_act = gtk_radio_action_new ("ViewInput_Input2", " 2 ", "Switch to input #2", NULL, VIEW_INPUT_2);
	gtk_radio_action_set_group (rad_act, gslist);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_input_select),
			NULL);

	/* Input: Input 3 button */
	rad_act = gtk_radio_action_new ("ViewInput_Input3", " 3 ", "Switch to input #3", NULL, VIEW_INPUT_3);
	gtk_radio_action_set_group (rad_act, gslist);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_input_select),
			NULL);

	/* Input: Input 4 button */
	rad_act = gtk_radio_action_new ("ViewInput_Input4", " 4 ", "Switch to input #4", NULL, VIEW_INPUT_4);
	gtk_radio_action_set_group (rad_act, gslist);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_input_select),
			NULL);

	/* Input: Input IP button */
	rad_act = gtk_radio_action_new ("ViewInput_InputIP", "IP", "Switch to IP input", NULL, VIEW_INPUT_IP);
	gtk_radio_action_set_group (rad_act, gslist);
	gslist = gtk_radio_action_get_group (rad_act);
	gtk_action_group_add_action (action_group, GTK_ACTION(rad_act));
	g_signal_connect(rad_act, "changed",
			G_CALLBACK(on_main_window_input_select),
			NULL);

	/* OSD switch */
	tog_act = gtk_toggle_action_new ("ViewMenuOSD", "Enable _OSD", NULL, NULL);
	gtk_action_group_add_action (action_group, GTK_ACTION(tog_act));
	g_signal_connect(tog_act, "toggled",
			G_CALLBACK(on_main_window_view_osd),
			NULL);

	main_window_ui_mgr = gtk_ui_manager_new();
	g_object_set_data_full(G_OBJECT(main_window), "ui-manager", main_window_ui_mgr,
			g_object_unref);
	gtk_ui_manager_insert_action_group (main_window_ui_mgr, action_group, 0);
	gtk_window_add_accel_group(GTK_WINDOW(main_window),
			gtk_ui_manager_get_accel_group (main_window_ui_mgr));

	if (!gtk_ui_manager_add_ui_from_string (main_window_ui_mgr, ui_info, -1, &error)) {
		g_message ("building menus failed: %s", error->message);
		g_error_free (error);
	}

	bar = gtk_ui_manager_get_widget (main_window_ui_mgr, "/MenuBar");
	gtk_widget_show (bar);
	gtk_table_attach (GTK_TABLE (table),
			bar,
			/* X direction */          /* Y direction */
			0, 1,                      0, 1,
			(GtkAttachOptions)(GTK_EXPAND | GTK_FILL),     (GtkAttachOptions)0,
			0,                         0);

	bar = gtk_ui_manager_get_widget (main_window_ui_mgr, "/ToolBar");
	gtk_table_attach (GTK_TABLE (table),
			bar,
			/* X direction */       /* Y direction */
			0, 1,                   1, 2,
			(GtkAttachOptions)(GTK_EXPAND | GTK_FILL),  (GtkAttachOptions)0,
			0,                      0);

	/* Create document
	*/
#ifdef GTK_HAS_INFO_BAR
	infobar = gtk_info_bar_new (); /* FIXME: When did this appear in the GTK+? */
	gtk_widget_set_no_show_all (infobar, TRUE);
#endif
	messagelabel = gtk_label_new ("");
	gtk_widget_show (messagelabel);
#ifdef GTK_HAS_INFO_BAR
	gtk_box_pack_start (GTK_BOX (gtk_info_bar_get_content_area (GTK_INFO_BAR (infobar))),
			messagelabel,
			TRUE, TRUE, 0);
	gtk_info_bar_add_button (GTK_INFO_BAR (infobar),
			GTK_STOCK_OK, GTK_RESPONSE_OK);
	g_signal_connect (infobar, "response",
			G_CALLBACK (gtk_widget_hide), NULL);

	gtk_table_attach (GTK_TABLE (table),
			infobar,
			/* X direction */       /* Y direction */
			0, 1,                   2, 3,
			(GtkAttachOptions)(GTK_EXPAND | GTK_FILL),  (GtkAttachOptions)0,
			0,                      0);
#endif

	gtk_window_set_default_size(GTK_WINDOW(main_window),
			720, 480);


	/* vertical pane for video and content info */
	vpane = gtk_vpaned_new ();
	gtk_paned_set_position (GTK_PANED(vpane), 350);

	contents = gtk_event_box_new ();
	client_area = contents;
	gtk_widget_set_size_request (contents, 80, 60);
	gtk_paned_pack1 (GTK_PANED(vpane), contents, TRUE, FALSE);

	main_window_contents = contents;
	gtk_signal_connect (GTK_OBJECT(contents), "button_press_event", (GtkSignalFunc)on_video_button_press_event, NULL);
	gtk_signal_connect (GTK_OBJECT(contents), "key_press_event", (GtkSignalFunc)on_video_key_press_event, NULL);
	gtk_widget_set_events(contents, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK | GDK_LEAVE_NOTIFY_MASK);

	contents_info = gtk_scrolled_window_new (NULL, NULL);

	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW(contents_info),
			GTK_POLICY_AUTOMATIC,
			GTK_POLICY_AUTOMATIC);

	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW(contents_info),
			GTK_SHADOW_OUT);

	gtk_widget_set_size_request (contents_info, 210, 100);
	gtk_paned_pack2 (GTK_PANED(vpane), contents_info, TRUE, FALSE);


	contents_info_list_store = gtk_list_store_new (2, G_TYPE_STRING, G_TYPE_STRING);
#if defined(METADATA_STOCK_DEBUG)
	for (i=0;metadata_stock[i].name != NULL;i++) {
		GtkTreeIter iter;
		gtk_list_store_append (contents_info_list_store, &iter);
		gtk_list_store_set (contents_info_list_store, &iter,
				0, metadata_stock[i].name,
				1, metadata_stock[i].value,
				-1);
	}
#endif

	contents_info_list_view = gtk_tree_view_new_with_model (GTK_TREE_MODEL(contents_info_list_store));

	{
		GtkCellRenderer *r = gtk_cell_renderer_text_new();
		GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes ("Name", r, "text", 0, NULL);
		gtk_tree_view_column_set_sizing (c, GTK_TREE_VIEW_COLUMN_FIXED);
		gtk_tree_view_column_set_fixed_width (c, 160);
		gtk_tree_view_append_column (GTK_TREE_VIEW(contents_info_list_view), c);
		gtk_tree_view_column_set_resizable (c, TRUE);
	}

	{
		GtkCellRenderer *r = gtk_cell_renderer_text_new();
		GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes ("Value", r, "text", 1, NULL);
		gtk_tree_view_append_column (GTK_TREE_VIEW(contents_info_list_view), c);
		gtk_tree_view_column_set_resizable (c, TRUE);
	}

	gtk_container_add (GTK_CONTAINER(contents_info), contents_info_list_view);

	gtk_table_attach (GTK_TABLE (table),
			vpane,
			/* X direction */       /* Y direction */
			0, 1,                   3, 4,
			(GtkAttachOptions)(GTK_EXPAND | GTK_FILL),  (GtkAttachOptions)(GTK_EXPAND | GTK_FILL),
			0,                      0);

	main_window_metadata = contents_info;

	gtk_widget_set_app_paintable (GTK_WIDGET(contents), TRUE);
#ifdef GTK_HAS_INFO_BAR
	assert(gtk_widget_get_app_paintable (GTK_WIDGET(contents))); /* FIXME: When did this appear in the GTK+? */
#endif
	gtk_widget_set_double_buffered (GTK_WIDGET(contents), FALSE);

	g_signal_connect(contents, "expose-event",
		G_CALLBACK(on_main_window_video_expose),
		NULL);
#if 0
	/* Okay explain this GTK+ guys: I connect to this signal, but if I do,
	 * the main window loses the ability to respond to window resize events.
	 * A whole bunch of other subtle stuff breaks too.
	 *
	 * Conveniently, the GTK+ offers no way for me to chain a signal, so I
	 * can't just take it over and call down to the previous handler.
	 *
	 * All I want is a notification on when the user moves the window, so
	 * that if an actual hardware overlay is involved we can move it in
	 * sync with our main window. Is that so much to friggin ask?
	 *
	 * This kind of crap is why I despise graphical frameworks. Especially
	 * when UIs and toolkits these days are way too focused on pretty eye
	 * candy and fluffy bullshit. Nobody thinks of these things. */
	g_signal_connect_swapped(main_window, "configure-event",
		G_CALLBACK(on_main_window_configure_event),
		NULL);
#endif

	/* Create statusbar */
	statusbar = gtk_statusbar_new ();
	gtk_table_attach(GTK_TABLE (table),
			statusbar,
			/* X direction */       /* Y direction */
			0, 1,                   4, 5,
			(GtkAttachOptions)(GTK_EXPAND | GTK_FILL),  (GtkAttachOptions)0,
			0,                      0);

	gtk_widget_realize(contents);
	gdk_window_set_cursor(contents->window, gdk_cursor_new(GDK_CROSS));
	main_window_status = statusbar;

	client_area_display = gdk_x11_get_default_xdisplay();
	assert(client_area_display != NULL);
	client_area_xid = GDK_WINDOW_XID(GDK_WINDOW(contents->window));
	assert(client_area_xid != 0);
	client_area_screen = gdk_x11_get_default_screen();
	x_shm_supported = XShmQueryExtension(client_area_display) ? true : false;
	fprintf(stderr,"SHM extension: %s\n",x_shm_supported?"yes":"no");
	xvideo_supported = XvQueryExtension(client_area_display,&xvideo_version,&xvideo_release,&xvideo_request_base,&xvideo_event_base,&xvideo_error_base) == Success;
	fprintf(stderr,"Xv extension: %s\n",xvideo_supported?"yes":"no");
	if (xvideo_supported)
		fprintf(stderr,"  Ver=%u Release=%u ReqBase=%u EvBase=%u ErrBase=%u\n",
			xvideo_version,
			xvideo_release,
			xvideo_request_base,
			xvideo_event_base,
			xvideo_error_base);

	/* record button */
	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/ToolBar/Record");
	gtk_tool_item_set_homogeneous (GTK_TOOL_ITEM(tmp), FALSE);
	gtk_tool_button_set_icon_widget (GTK_TOOL_BUTTON(tmp), gtk_image_new_from_file ("record.png"));
	main_window_toolbar_record = tmp;

	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/MenuBar/ControlMenu/ControlMenu_Record");
	main_window_control_record = tmp;

	/* play button */
	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/ToolBar/Play");
	gtk_tool_item_set_homogeneous (GTK_TOOL_ITEM(tmp), FALSE);
	gtk_tool_button_set_icon_widget (GTK_TOOL_BUTTON(tmp), gtk_image_new_from_file ("play.png"));
	main_window_toolbar_play = tmp;

	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/MenuBar/ControlMenu/ControlMenu_Play");
	main_window_control_play = tmp;

	/* pause button */
	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/ToolBar/Pause");
	gtk_tool_item_set_homogeneous (GTK_TOOL_ITEM(tmp), FALSE);
	gtk_tool_button_set_icon_widget (GTK_TOOL_BUTTON(tmp), gtk_image_new_from_file ("pause.png"));
	main_window_toolbar_pause = tmp;

	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/MenuBar/ControlMenu/ControlMenu_Pause");
	main_window_control_pause = tmp;

	/* stop button */
	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/ToolBar/Stop");
	gtk_tool_item_set_homogeneous (GTK_TOOL_ITEM(tmp), FALSE);
	gtk_tool_button_set_icon_widget (GTK_TOOL_BUTTON(tmp), gtk_image_new_from_file ("stop.png"));
	main_window_toolbar_stop = tmp;

	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/MenuBar/ControlMenu/ControlMenu_Stop");
	main_window_control_stop = tmp;

	/* input: none button */
	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/ToolBar/ViewInput_None");
	gtk_tool_item_set_homogeneous (GTK_TOOL_ITEM(tmp), FALSE);
	gtk_tool_button_set_icon_widget (GTK_TOOL_BUTTON(tmp), gtk_image_new_from_file ("in_none.png"));
	main_window_toolbar_input_none = tmp;

	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/MenuBar/ViewMenu/ViewMenuInput/ViewMenuInput_None");
	main_window_view_input_none = tmp;

	/* input: file button */
	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/ToolBar/ViewInput_File");
	gtk_tool_item_set_homogeneous (GTK_TOOL_ITEM(tmp), FALSE);
	gtk_tool_button_set_icon_widget (GTK_TOOL_BUTTON(tmp), gtk_image_new_from_file ("in_file.png"));
	main_window_toolbar_input_file = tmp;

	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/MenuBar/ViewMenu/ViewMenuInput/ViewMenuInput_File");
	main_window_view_input_file = tmp;

	/* input: 1 button */
	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/ToolBar/ViewInput_Input1");
	gtk_tool_item_set_homogeneous (GTK_TOOL_ITEM(tmp), FALSE);
	main_window_toolbar_input_1 = tmp;

	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/MenuBar/ViewMenu/ViewMenuInput/ViewMenuInput_Input1");
	main_window_view_input_1 = tmp;

	/* input: 2 button */
	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/ToolBar/ViewInput_Input2");
	gtk_tool_item_set_homogeneous (GTK_TOOL_ITEM(tmp), FALSE);
	main_window_toolbar_input_2 = tmp;

	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/MenuBar/ViewMenu/ViewMenuInput/ViewMenuInput_Input2");
	main_window_view_input_2 = tmp;

	/* input: 3 button */
	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/ToolBar/ViewInput_Input3");
	gtk_tool_item_set_homogeneous (GTK_TOOL_ITEM(tmp), FALSE);
	main_window_toolbar_input_3 = tmp;

	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/MenuBar/ViewMenu/ViewMenuInput/ViewMenuInput_Input3");
	main_window_view_input_3 = tmp;

	/* input: 1 button */
	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/ToolBar/ViewInput_Input4");
	gtk_tool_item_set_homogeneous (GTK_TOOL_ITEM(tmp), FALSE);
	main_window_toolbar_input_4 = tmp;

	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/MenuBar/ViewMenu/ViewMenuInput/ViewMenuInput_Input4");
	main_window_view_input_4 = tmp;

	/* input: IP button */
	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/ToolBar/ViewInput_InputIP");
	gtk_tool_item_set_homogeneous (GTK_TOOL_ITEM(tmp), FALSE);
	gtk_tool_button_set_icon_widget (GTK_TOOL_BUTTON(tmp), gtk_image_new_from_file ("in_ip.png"));
	main_window_toolbar_input_ip = tmp;

	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/MenuBar/ViewMenu/ViewMenuInput/ViewMenuInput_InputIP");
	main_window_view_input_ip = tmp;

	tmp = gtk_ui_manager_get_widget (main_window_ui_mgr, "/MenuBar/ViewMenu/ViewMenuOSD");
	main_window_view_osd = tmp;
	gtk_check_menu_item_set_active_notoggle (GTK_CHECK_MENU_ITEM(tmp), user_enable_osd);

#ifdef GTK_HAS_INFO_BAR
	if (gtk_widget_get_visible(main_window)) /* FIXME: When did this appear in the GTK+? */
		g_warning("Main window prematurely visible");
#endif

	gtk_widget_show_all(main_window);

	if (cfg_show_status) gtk_widget_show (main_window_status);
	else gtk_widget_hide (main_window_status);

	if (cfg_show_metadata) gtk_widget_show (main_window_metadata);
	else gtk_widget_hide (main_window_metadata);

	tmp1 = gtk_ui_manager_get_widget (main_window_ui_mgr, "/ToolBar");
	assert(tmp1 != NULL);
	if (cfg_show_toolbar) gtk_widget_show (tmp1);
	else gtk_widget_hide (tmp1);

	/* we want the UI do to something periodically */
	g_timeout_add(1000/60,main_window_ticker,NULL);

#ifdef GTK_HAS_INFO_BAR
	gtk_widget_set_can_focus (contents, TRUE); /* FIXME: When did this appear in the GTK+? */
#endif
	gtk_widget_grab_focus (contents); /* Keep keyboard focus HERE please */

	return 0; /* no error */
}

/******************************************************************************
 * load_application_icon
 *
 * Load our application icon (PNG) into a GDK pixbuf for use as application icon
 *
 * Inputs: none
 * Outputs: none
 * Returns: none
 * Scope: Global, process-wide
 * Global variables used: none
 * Global variables modified: application_icon_gdk_pixbuf
 *****************************************************************************/
static void load_application_icon() {
	GError *error = NULL;
	if (application_icon_gdk_pixbuf == NULL) {
		if ((application_icon_gdk_pixbuf = gdk_pixbuf_new_from_file("tv.png",&error)) == NULL)
			g_warning("Unable to load application icon tv.png");
	}
}

/******************************************************************************
 * free_application_icon
 *
 * Free our application icon GDK pixbuf
 *
 * Inputs: none
 * Outputs: none
 * Returns: none
 * Scope: Global, process-wide
 * Global variables used: none
 * Global variables modified: application_icon_gdk_pixbuf
 *****************************************************************************/
static void free_application_icon() {
	if (application_icon_gdk_pixbuf != NULL) {
		g_object_unref(G_OBJECT(application_icon_gdk_pixbuf));
		application_icon_gdk_pixbuf = NULL;
	}
}

/* TODO: Remove me */
void *secondary_thread(void *p) {
	while (!secondary_thread_must_die) {
		usleep(100000);
	}
}

void secondary_thread_shutdown() {
	secondary_thread_must_die = 1;
	if (secondary_thread_id != 0) {
		pthread_join(secondary_thread_id,NULL);
		secondary_thread_id = 0;
	}
	pthread_mutex_destroy(&global_mutex);
	assert(pthread_mutex_init(&global_mutex,NULL) == 0); // global_mutex = PTHREAD_MUTEX_INITIALIZER;
}

int main(int argc,char **argv) {
	gtk_init(&argc,&argv);
	if (!XInitThreads())
		fprintf(stderr,"Warning, cannot init X threads\n");

	/* BUGFIX: IN case the program crashes, shut down capture_v4l instances */
	system("killall -w capture_v4l 2>/dev/null");

	if (init_inputs()) {
		g_error("Unable to initialize inputs");
		return 1;
	}

	load_configuration();
	load_application_icon();
	ui_notification_log_init();

	avcodec_register_all();

	if (init_main_window()) {
		g_error("Unable to initialize main window");
		return 1;
	}

	/* start thread */
	if (pthread_create(&secondary_thread_id,NULL,secondary_thread,NULL)) {
		g_error("Cannot start secondary thread");
		return 1;
	}

	/* TODO: This will be updated later */
	client_area_width = -1;
	client_area_height = -1;

	update_ui();
	CurrentInputObj()->onActivate(1);
/* the GTK+ library takes over from here to run the "message pump" and handle events */
/* the GTK+ will return control to us when instructed to terminate and it has freed the windows and widgets */
	gtk_main();
	secondary_thread_shutdown();
	save_configuration();

/* cleanup after ourself */
	printf("Goodbye!\n");
	free_application_icon();
	ui_notification_log_free();
	client_area_free();
	free_inputs();

	if (temp_avi_frame) av_free(temp_avi_frame);
	temp_avi_frame = NULL;

	return 0;
}

