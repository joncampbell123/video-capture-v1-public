#!/bin/bash
./cleantree

# useful stuff
check() { # $1 = command to check
	what="$1"
	echo -n "Checking $what ... "
	x=`which $what`

	# the code below uses Dec VT100 terminal color escapes. Everything in Linux, including
	# ncurses, xterm, and the Linux framebuffer/VGA console assume it. Search Google for more information
	if [ x"$x" == x ]; then echo -e "\x1B[1;31mNOT FOUND\x1B[0m"; return 1; fi # 1;31 = BRIGHT RED     0 = NORMAL
	echo -e "\x1B[1;32mFOUND\x1B[0m at $x"

	return 0
}

pkg_exists() { # $1 = package to check
	what="$1"
	echo -n "Checking package $what ... "
	pkg-config $what --exists; x=$?

	# the code below uses Dec VT100 terminal color escapes. Everything in Linux, including
	# ncurses, xterm, and the Linux framebuffer/VGA console assume it. Search Google for more information
	if [ x"$x" != x"0" ]; then echo -e "\x1B[1;31mNOT FOUND\x1B[0m"; return 1; fi # 1;31 = BRIGHT RED     0 = NORMAL
	echo -e "\x1B[1;32mFOUND\x1B[0m"

	return 0
}

# make sure the host system has the right tools
check aclocal || exit 1
check autoheader || exit 1
check automake || exit 1
check autoconf || exit 1
check python || exit 1
check pkg-config || exit 1
check yasm || exit 1 # WE don't need this, but FFMPEG does
check m4 || exit 1

# check GTK+ and GLIB.
pkg_exists gtk+-2.0 || exit 1
ver=`pkg-config gtk+-2.0 --modversion`
echo "  Version $ver"

# GLIB
pkg_exists glib-2.0 || exit 1
ver=`pkg-config glib-2.0 --modversion`
echo "  Version $ver"

# Pango
pkg_exists pango || exit 1
ver=`pkg-config pango --modversion`
echo "  Version $ver"

# ATK
pkg_exists atk || exit 1
ver=`pkg-config atk --modversion`
echo "  Version $ver"

# ALSA sound library
pkg_exists alsa || exit 1
ver=`pkg-config alsa --modversion`
echo "  Version $ver"

# OK do it
echo >NEWS
echo >AUTHORS
echo >ChangeLog
mkdir -p m4 || exit 1
(aclocal && autoheader && automake --add-missing && autoconf) || exit 1
mkdir -p m4 || exit 1

