#!/bin/sh
# gr-nut sink example checker: prove the full circle is byte-exact.
#
# Runs sink->source round trips: known vectors (a deterministic tone and
# synthetic rgb24 frames) -> nut_sink -> file, then the same file ->
# nut_source -> vector sinks, comparing end to end — samples and frame
# payloads byte for byte, adopted rate/geometry against the declared
# ones, and (via ffprobe, if present) the written headers and per-packet
# video durations. The sink and the source share no code paths, so a
# green run certifies both directions of the contract at once.
#
# Three legs: audio-only and video-only at the full requested size, and
# a joint A/V leg at a small fixed size. The joint leg is small on
# purpose: GNU Radio declares a sink done as soon as its FIRST input is
# exhausted, and an upstream source still blocked on a full buffer at
# that moment quits early — so a finite multi-input flowgraph is only
# byte-complete when every stream's payload fits its input buffer
# (~16k float items / ~64k byte items). Single-input legs are immune at
# any size, and real recorders stop via tb.stop()/Ctrl-C rather than by
# exhausting finite vectors.
#
# usage: roundtrip_check.sh [WIDTH] [HEIGHT] [FPS] [SECONDS]
set -eu

W=${1:-320}
H=${2:-240}
FPS=${3:-25}
DUR=${4:-2}
TMP=$(mktemp -d)

cleanup() {
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

W="$W" H="$H" FPS="$FPS" DUR="$DUR" TMP="$TMP" python3 <<'EOF'
import os
import shutil
import subprocess

import numpy as np
from gnuradio import blocks, gr, nut

W = int(os.environ["W"])
H = int(os.environ["H"])
FPS = int(os.environ["FPS"])
DUR = int(os.environ["DUR"])
TMP = os.environ["TMP"]
RATE = 48000


def tone(n):
    return (0.5 * np.sin(2 * np.pi * 997 * np.arange(n) / RATE)).astype(
        np.float32
    )


def make_frames(nframes, w, h):
    fsz = w * h * 3
    frames = np.empty((nframes, fsz), dtype=np.uint8)
    for k in range(nframes):
        frames[k] = (np.arange(fsz, dtype=np.int64) * 3 + k * 17) % 256
    return frames


def probe_headers(path):
    if not shutil.which("ffprobe"):
        return
    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-show_streams", "-of", "csv", path],
        capture_output=True, text=True, check=True,
    ).stdout
    if "rawvideo" in probe:
        durs = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "v:0",
             "-show_entries", "packet=duration", "-of", "csv=p=0", path],
            capture_output=True, text=True, check=True,
        ).stdout.split()
        assert durs and "N/A" not in durs, "video packets must carry durations"
    return probe


def read_back(path, nchan, nvideo):
    src = nut.nut_source(path, nchan, nvideo)
    sinks = []
    tb = gr.top_block()
    for ch in range(nchan):
        s = blocks.vector_sink_f()
        tb.connect((src, ch), s)
        sinks.append(s)
    for v in range(nvideo):
        s = blocks.vector_sink_b()
        tb.connect((src, nchan + v), s)
        sinks.append(s)
    tb.run()
    assert src.last_error() == "", "source failed: %s" % src.last_error()
    return src, sinks


# ---- leg 1: audio only, full size ----------------------------------
audio = tone(DUR * RATE)
out = os.path.join(TMP, "a.nut")
snk = nut.nut_sink(out, 1, RATE, 0)
tb = gr.top_block()
tb.connect(blocks.vector_source_f(audio.tolist(), False), snk)
tb.run()
assert snk.last_error() == "", "sink failed: %s" % snk.last_error()
probe = probe_headers(out)
if probe:
    assert "pcm_f32le" in probe and ",%d," % RATE in probe
src, (sa,) = read_back(out, 1, 0)
assert src.audio_rate() == RATE, "adopted rate != declared"
assert np.array_equal(np.array(sa.data(), dtype=np.float32), audio), \
    "audio not sample-exact"
print("OK: audio %d samples @ %d Hz round-tripped sample-exact"
      % (len(audio), RATE))

# ---- leg 2: video only, full size -----------------------------------
frames = make_frames(DUR * FPS, W, H)
out = os.path.join(TMP, "v.nut")
snk = nut.nut_sink(out, 0, 0, 1, [W], [H], [FPS])
tb = gr.top_block()
tb.connect(blocks.vector_source_b(frames.reshape(-1).tolist(), False), snk)
tb.run()
assert snk.last_error() == "", "sink failed: %s" % snk.last_error()
probe = probe_headers(out)
if probe:
    assert "rawvideo" in probe
src, (sv,) = read_back(out, 0, 1)
assert (src.video_width(), src.video_height()) == (W, H), "geometry mismatch"
assert np.array_equal(np.array(sv.data(), dtype=np.uint8),
                      frames.reshape(-1)), "video not byte-exact"
print("OK: video %d frames of %dx%d rgb24 round-tripped byte-exact"
      % (len(frames), W, H))

# ---- leg 3: joint A/V, buffer-safe size (see the header comment) ----
w, h, fps, nframes = 64, 48, 25, 6
audio = tone(nframes * RATE // fps)     # same media time as the video
frames = make_frames(nframes, w, h)
out = os.path.join(TMP, "av.nut")
snk = nut.nut_sink(out, 1, RATE, 1, [w], [h], [fps])
tb = gr.top_block()
tb.connect(blocks.vector_source_f(audio.tolist(), False), (snk, 0))
tb.connect(blocks.vector_source_b(frames.reshape(-1).tolist(), False), (snk, 1))
tb.run()
assert snk.last_error() == "", "sink failed: %s" % snk.last_error()
probe_headers(out)
_, (sa, sv) = read_back(out, 1, 1)
assert np.array_equal(np.array(sa.data(), dtype=np.float32), audio), \
    "A/V audio not sample-exact"
assert np.array_equal(np.array(sv.data(), dtype=np.uint8),
                      frames.reshape(-1)), "A/V video not byte-exact"
print("OK: joint A/V (%d samples + %d frames of %dx%d) round-tripped, "
      "nothing trimmed" % (len(audio), nframes, w, h))
EOF
