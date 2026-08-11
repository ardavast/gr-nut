#!/bin/sh
# gr-nut M1 example: mono FM transmitter (HackRF).
#
# usage: fm_mono_tx.sh INPUT [FREQ_HZ] [VGA_GAIN_DB]
#   INPUT       anything ffmpeg can read (music file, playlist, URL, ...)
#   FREQ_HZ     center frequency, default 99.9e6
#   VGA_GAIN_DB HackRF TX VGA gain 0-47, default 40
#
# ffmpeg decodes/conforms INPUT to the gr-nut contract (mono pcm_f32le
# 48 kHz in a NUT stream) and writes it into a FIFO. The flowgraph ingests
# the FIFO and transmits: 48k -> 200k (25/6) -> WBFM mono (+-75 kHz,
# 50 us preemphasis) -> 200k -> 8M (40/1) -> HackRF @ 8 MSPS.
#
# The HackRF is the ONLY clock: there is no Throttle anywhere and ffmpeg
# runs without -re — it is paced purely by pipe backpressure. While
# streaming, ffmpeg's CPU usage should sit near zero.
#
# Start order does not matter much (each side blocks on the FIFO until the
# other appears), but note that the flowgraph waits inside start() until
# ffmpeg has written the NUT stream headers.
set -eu

INPUT=${1:?usage: fm_mono_tx.sh INPUT [FREQ_HZ] [VGA_GAIN_DB]}
FREQ=${2:-99.9e6}
VGA=${3:-40}
FIFO=${FIFO:-/tmp/fm_mono.nut}
DIR=$(dirname "$0")

[ -p "$FIFO" ] || mkfifo "$FIFO"

cleanup() {
    [ -n "${FFPID:-}" ] && kill "$FFPID" 2>/dev/null || true
    rm -f "$FIFO"
}
trap cleanup EXIT INT TERM

# Reference command from the gr-nut contract (audio-only, mono profile).
ffmpeg -hide_banner -loglevel warning -nostdin -i "$INPUT" \
    -vn -af "aresample=48000:async=1" -ac 1 \
    -c:a pcm_f32le \
    -max_interleave_delta 500000 \
    -f nut pipe:1 > "$FIFO" &
FFPID=$!

python3 "$DIR/fm_mono_tx.py" --uri "$FIFO" --freq "$FREQ" --vga-gain "$VGA"
