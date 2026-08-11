#!/bin/sh
# gr-nut M3 example checker: prove the video path is byte-exact.
#
# Pipes a deterministic ffmpeg test pattern (testsrc2) through the NUT
# contract into video_to_file.py, then renders the same pattern directly
# to raw rgb24 with ffmpeg and compares the two dumps bit for bit.
#
# usage: video_check.sh [WIDTH] [HEIGHT] [FPS] [SECONDS]
set -eu

W=${1:-320}
H=${2:-240}
FPS=${3:-25}
DUR=${4:-2}
DIR=$(dirname "$0")
TMP=$(mktemp -d)
FIFO="$TMP/video.nut"

cleanup() {
    [ -n "${FFPID:-}" ] && kill "$FFPID" 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

mkfifo "$FIFO"

# Contract-conforming writer: rawvideo/rgb24, exact geometry, CFR.
ffmpeg -hide_banner -loglevel warning -nostdin \
    -f lavfi -i "testsrc2=size=${W}x${H}:rate=${FPS}:duration=${DUR}" \
    -c:v rawvideo -pix_fmt rgb24 -fps_mode cfr \
    -max_interleave_delta 500000 \
    -f nut pipe:1 > "$FIFO" &
FFPID=$!

python3 "$DIR/video_to_file.py" \
    --uri "$FIFO" --width "$W" --height "$H" --outfile "$TMP/frames.rgb"
wait "$FFPID" || true

# Reference: the same pattern rendered directly to raw rgb24.
ffmpeg -hide_banner -loglevel warning -nostdin \
    -f lavfi -i "testsrc2=size=${W}x${H}:rate=${FPS}:duration=${DUR}" \
    -pix_fmt rgb24 -f rawvideo -y "$TMP/ref.rgb"

if cmp "$TMP/frames.rgb" "$TMP/ref.rgb"; then
    frames=$(( $(stat -c %s "$TMP/frames.rgb") / (W * H * 3) ))
    echo "OK: $frames frames of ${W}x${H} rgb24, byte-exact"
else
    echo "FAIL: frame dump differs from ffmpeg reference" >&2
    exit 1
fi
