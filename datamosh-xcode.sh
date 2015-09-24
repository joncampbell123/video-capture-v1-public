#!/bin/bash
if [[ x"$1" == x"" || x"$2" == x"" || x"$3" == x"" || x"$4" == x"" ]]; then
	echo "Usage: datamosh.sh <input AVI> <output AVI> <start time> <duration>"
	exit 1
fi

ffmpeg -i "$1" -map 0:0 -map 0:1 -acodec pcm_s16le -vcodec libxvid -bf 0 -sc_threshold 1000000000 -g 999999 -nr 525 -p_mask 1.0 -qmin 3 -qmax 3 -qscale 3 -ss "$3" -t "$4" -f avi "$2" || exit 1

