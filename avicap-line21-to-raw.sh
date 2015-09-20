#!/bin/bash
#
# How to use:
#
# After capturing a VHS tape with this capture utility (with VBI enabled),
# use this script on the AVI file to extract Line 21 from the VBI video stream.
# This script will use FFMPEG on your system to extract that one scanline and
# write it out as a raw Line 21 capture. You can then feed it to eia608_decoder_test
# like this:
#
# ./eia608_decoder_test -raw -line21=<width> <name of the AVI file>
#
# Where <width> is the width of the VBI video frame reported by FFMPEG (second video stream)
# and <name of the AVI file> is the name of the AVI file.
field=0
line=0
sline=0
start=
time=

if [[ "$*" == "" ]]; then
	echo "$0 <AVI> [field] [start] [duration]"
	echo ""
	echo "<AVI>                   AVI capture file"
	echo "[field]                 Which field to extract from (0 or 1)."
	echo "                           Use 0 for the main closed caption data (CC1/CC2)"
	echo "                           Use 1 for the secondary captions (CC3/CC4) and Extended Data Services"
	echo "[start]                 Start time within capture, in seconds. This is mapped to FFMPEG's -ss switch"
	echo "[duration]              Duration to extract, in seconds. This is mapped to FFMPEG's -t switch"
	exit 1
fi

avi="$1"
shift

field="$1"
field=$(($field))
shift

start="$1"
shift
if [[ x"$start" == x ]]; then true; else start="-ss $start"; fi

time="$1"
shift
if [[ x"$time" == x ]]; then true; else time="-t $time"; fi

if [[ "$field" == "0" ]]; then
	line=21
	sline=11
elif [[ "$field" == "1" ]]; then
	line=283
	sline=$((12+11))
else
	echo "Unknown!"
	exit 1
fi

echo "AVI file: $avi"
echo "   Field: $field"
echo "    Line: $line"
echo "Src line: $sline"

if [[ "$sline" == "0" ]]; then
	exit 1
fi

if [[ !( -f "$avi" ) ]]; then
	echo "No such file $avi"
	exit 1
fi

ffprobe "$avi" || exit 1

echo "======== Please pay attention to the second video stream (VBI) and note the width ========="
echo "This script assumes VBI capture contains lines 10-21 and 273-284 in that order"
echo "Line 21 contains CC1/CC2 and Line 283 contains CC3/CC4"

ffmpeg -i "$avi" -an -vcodec rawvideo $start $time -r 30000/1001 -vf format=pix_fmts=gray,crop=w=in_w:h=1:x=0:y=$sline -map 0:2 -y -f rawvideo line21.field$field.raw || exit 1

