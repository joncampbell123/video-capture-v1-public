SUBDIRS =
ACLOCAL_AMFLAGS = -I m4
top = $(shell pwd)

noinst_PROGRAMS = videocap capture_v4l line21_to_eia608

# enable a "make binary-pkg" command, which creates a "bin/<arch>" subdirectory suitable for binary distribution
binary-pkg:
	./make-binary

# test util
line21_to_eia608_SOURCES = line21_to_eia608.c line21-analog-decoder.c
line21_to_eia608_CFLAGS = -D_FILE_OFFSET_BITS=64
line21_to_eia608_LDFLAGS = 

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

