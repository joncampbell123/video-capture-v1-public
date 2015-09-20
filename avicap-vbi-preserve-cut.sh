#!/bin/bash
#
# How to use:
#
# After capturing a VHS tape with this capture utility (with VBI enabled),
# use this script on the AVI file to extract a subset of the capture to another AVI
# file with VBI intact.
start=
time=

if [[ "$*" == "" ]]; then
	echo "$0 <AVI> <output> [start] [duration]"
	echo ""
	echo "<AVI>                   AVI capture file"
	echo "<output>                Output AVI file"
	echo "[start]                 Start time within capture, in seconds. This is mapped to FFMPEG's -ss switch"
	echo "[duration]              Duration to extract, in seconds. This is mapped to FFMPEG's -t switch"
	exit 1
fi

avi="$1"
shift

output_avi="$1"
shift

start="$1"
shift
if [[ x"$start" == x ]]; then true; else start="-ss $start"; fi

time="$1"
shift
if [[ x"$time" == x ]]; then true; else time="-t $time"; fi

echo "AVI file: $avi"
echo "     Out: $output_avi"
echo "    Line: $line"
echo "Src line: $sline"

if [[ !( -f "$avi" ) ]]; then
	echo "No such file $avi"
	exit 1
fi

ffmpeg -i "$avi" -map 0:0 -acodec copy -map 0:1 -vcodec copy -map 0:2 -vcodec copy $start $time -r 30000/1001 -y -f avi "$output_avi" || exit 1

