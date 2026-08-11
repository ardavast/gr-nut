# gr-nut

A single, reusable, well-defined interface between **ffmpeg** and **GNU
Radio** for analogue transmission chains: ffmpeg is the codec/format/sync
layer, GNU Radio is the physical layer, and a **NUT stream over a
pipe/FIFO** is the socket between them.

The module contains one block, `nut.nut_source`: it ingests a raw-payload
NUT stream and emits deinterleaved float audio (one port per channel) plus
an optional rgb24 video byte stream. It is a generic ingest block — no
TV- or radio-specific logic; modulators are ordinary flowgraphs built from
stock GR blocks (see `examples/` for a mono FM transmitter).

## The interface contract (NUT profile)

| Property       | Value                                                                  |
|----------------|------------------------------------------------------------------------|
| Container      | NUT, streamed (no seeking), read from a pipe/FIFO or file              |
| Audio codec    | `pcm_f32le`, interleaved                                               |
| Audio rate     | as declared to the block (default profile: 48000 Hz)                   |
| Audio channels | as declared to the block                                               |
| Video codec    | `rawvideo`, pix_fmt `rgb24` (only if the block instance declares video)|
| Video geometry | exactly the declared width × height; ffmpeg does ALL scaling           |
| Video rate     | exactly the declared fps, CFR enforced                                 |
| Stream layout  | exactly the declared set of streams — anything else is a hard error    |
| Interleaving   | muxer bounds skew; always pass `-max_interleave_delta` explicitly      |

The block **validates** this profile at `start()` and throws
`std::runtime_error` with an actionable message on any mismatch — it never
adapts. All flexibility lives on ffmpeg's input side; ffmpeg exists to
conform arbitrary media to this profile.

Reference ffmpeg invocations:

```sh
# audio-only, mono
ffmpeg -i INPUT \
  -vn -af "aresample=48000:async=1" -ac 1 \
  -c:a pcm_f32le \
  -max_interleave_delta 500000 \
  -f nut pipe:1 > "$FIFO"

# audio-only, stereo: same with -ac 2

# audio + video
ffmpeg -i INPUT \
  -filter_complex "[0:v]fps=FPS,scale=W:H:force_original_aspect_ratio=decrease,pad=W:H:-1:-1,setsar=1[v]; \
                   [0:a]aresample=48000:async=1[a]" \
  -map "[v]" -map "[a]" \
  -c:v rawvideo -pix_fmt rgb24 -c:a pcm_f32le \
  -fps_mode cfr -max_interleave_delta 500000 \
  -f nut pipe:1 > "$FIFO"
```

## Clocking model (load-bearing)

- The flowgraph sink (SDR, audio device) is the **only** clock. No
  Throttle blocks in TX flowgraphs. ffmpeg is paced purely by pipe
  backpressure (no `-re`).
- All downstream rate relationships must be exact rational ratios
  (rational resamplers, never approximate float ratios). With a single
  clock and rational ratios, inter-stream drift is identically zero.
- Rate-defective sources (VFR video, lying sample rates, gaps) are
  repaired by ffmpeg *before* the pipe (`-fps_mode cfr`,
  `aresample=async=1`).
- When both audio and video are declared, the initial pts offset between
  them is trimmed at startup so both ports start at the same media time
  (within one audio sample / one video frame). pts discontinuities beyond
  a few ms are logged as warnings, never resynced.

## Video port

`unsigned char` stream: rgb24 frames back to back, row-major. A stream tag
is attached to the first byte of every frame with keys `pts` (int64, NUT
stream timebase), `width`, `height`. Steady-state consumers must not
*need* the tags (geometry is contractual); they exist for diagnostics and
alignment.

## Buffering (deadlock prevention — do not shrink)

A multi-output demuxer with bounded output buffers can deadlock when
downstream ends in a synchronous combiner: the demuxer blocks pushing
stream A into a full buffer while the consumer starves for stream B, which
the blocked demuxer would deliver next. Because the NUT muxer bounds
interleave skew (`-max_interleave_delta`), buffers larger than the maximum
interleave burst break the cycle. The block therefore requests
`set_min_output_buffer` of **≥ 1 s** on each audio port and **≥ 4 frames**
(4×W×H×3 bytes) on the video port. GR's default buffers are tens of kB
while a raw frame is MBs — without this the video path would be broken out
of the box.

## Build

Dependencies: GNU Radio 3.10.x (+ gr_modtool/pybind11 dev bits),
libavformat/libavutil/libavcodec dev headers (Debian/Ubuntu:
`libavformat-dev libavutil-dev libavcodec-dev`), and the `ffmpeg` CLI for
running the QA tests and examples.

```sh
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
ctest            # QA (needs ffmpeg; skips cleanly without it)
sudo make install && sudo ldconfig
```

## Usage

Python:

```python
from gnuradio import nut
src = nut.nut_source(uri="/tmp/stream.nut",
                     audio_channels=1, audio_rate=48000,
                     emit_video=False, video_width=0, video_height=0,
                     repeat=False)
```

GRC (category `[nut]`): three symmetric entries, all instantiating the
same C++ block and differing only in the declared stream layout:

- **NUT Audio Source** — audio only (`emit_video=False`); one float port
  per channel.
- **NUT Video Source** — video only (`audio_channels=0`); one byte port.
- **NUT A/V Source** — both; audio ports plus the video byte port.

One block, three fixed-shape GRC entries: GRC cannot express
zero-multiplicity port groups, and honest fixed shapes are clearer than
conditionally hidden ports.

Notes:

- ffmpeg is launched externally (shell script, systemd unit); the block is
  given the FIFO/file path. See `examples/fm_mono_tx.sh`.
- `start()` blocks until the writer delivers the NUT stream headers (they
  are needed for validation), so start ffmpeg first — or at least make
  sure it will start.
- `repeat=True` reopens the input on EOF (seekable inputs only, i.e. real
  files); a FIFO stops at EOF.
- On EOF the block signals done; a dead writer (broken pipe) is EOF plus a
  logged warning.

## Design

See `gr-nut-design.md` (kept next to this repo) for the full design
document: architecture, contract rationale, scheduler mechanics, pts
handling, and the QA plan the tests in `python/nut/qa_nut_source.py`
implement.
