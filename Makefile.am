SUBDIRS =
ACLOCAL_AMFLAGS = -I m4
top = $(shell pwd)

noinst_PROGRAMS = videocap capture_v4l line21_to_eia608 raw_eia608_to_line21 raw_eia608_to_scc scc_eia608_to_raw eia608_decoder_test c4_riff_datamosh_assist c4_riff_datamosh_assist2 avicutter

# enable a "make binary-pkg" command, which creates a "bin/<arch>" subdirectory suitable for binary distribution
binary-pkg:
	./make-binary

# test util
line21_to_eia608_SOURCES = line21_to_eia608.c line21-analog-decoder.c
line21_to_eia608_CFLAGS = -D_FILE_OFFSET_BITS=64

# test util
raw_eia608_to_line21_SOURCES = raw_eia608_to_line21.c line21-analog-encoder.c
raw_eia608_to_line21_CFLAGS = -D_FILE_OFFSET_BITS=64
raw_eia608_to_line21_LDFLAGS = -lm

# test util
raw_eia608_to_scc_SOURCES = raw_eia608_to_scc.c
raw_eia608_to_scc_CFLAGS = -D_FILE_OFFSET_BITS=64

# test util
scc_eia608_to_raw_SOURCES = scc_eia608_to_raw.c sliding_window.c scc-reader.c
scc_eia608_to_raw_CFLAGS = -D_FILE_OFFSET_BITS=64

# test util
eia608_decoder_test_SOURCES = eia608_decoder_test.c eia608-decoder.c sliding_window.c line21-analog-decoder.c scc-reader.c extended-data-service-decoder.c eia608-demux.c unicode.c webtv-decoder.c vgafont_8x14.c eia608-font-8x14.c
eia608_decoder_test_CFLAGS = -std=gnu99 -D_FILE_OFFSET_BITS=64
eia608_decoder_test_LDFLAGS = -lSDL

# fun util
c4_riff_datamosh_assist_SOURCES = c4_riff_datamosh_assist.c now.c avi_writer.c avi_reader.c avi_rw_iobuf.c riff.c
c4_riff_datamosh_assist_CFLAGS = -std=gnu99 -D_FILE_OFFSET_BITS=64

# fun util
c4_riff_datamosh_assist2_SOURCES = c4_riff_datamosh_assist2.c now.c avi_writer.c avi_reader.c avi_rw_iobuf.c riff.c
c4_riff_datamosh_assist2_CFLAGS = -std=gnu99 -D_FILE_OFFSET_BITS=64

# fun util
avicutter_SOURCES = avicutter.cpp now.c avi_writer.c avi_reader.c avi_rw_iobuf.c riff.c
avicutter_CXXFLAGS = -std=gnu++0x -D_FILE_OFFSET_BITS=64
avicutter_LDFLAGS = -lm

# capture sub-process
capture_v4l_SOURCES = capture_v4l.cpp now.c avi_writer.c avi_reader.c avi_rw_iobuf.c riff.c
capture_v4l_CFLAGS = $(GTK_CFLAGS) -D_FILE_OFFSET_BITS=64
capture_v4l_CXXFLAGS = $(GTK_CFLAGS) -D_FILE_OFFSET_BITS=64
capture_v4l_LDFLAGS = $(GTK_LIBS) -lrt -lavcodec -lavutil -lasound

# main app
videocap_SOURCES = videocap.cpp now.c avi_writer.c avi_reader.c avi_rw_iobuf.c riff.c
videocap_CFLAGS = $(GTK_CFLAGS) -D_FILE_OFFSET_BITS=64
videocap_CXXFLAGS = $(GTK_CFLAGS) -D_FILE_OFFSET_BITS=64
videocap_LDFLAGS = $(GTK_LIBS) -lXv -pthread -lrt -lX11 -lXv -lXext -lavcodec -lavutil -lasound

install-exec-hook:

