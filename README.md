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
| Audio rate     | read from the stream headers (adopted at start; cap 192 kHz)           |
| Audio channels | as declared to the block (structural: fixes the port count)            |
| Video codec    | `rawvideo`, pix_fmt `rgb24` (only if the block instance declares video)|
| Video geometry | read from the stream headers (adopted at start; cap 1920×1080); ffmpeg does ALL scaling |
| Video rate     | read from the stream headers; CFR enforced by ffmpeg (`-fps_mode cfr`) |
| Stream layout  | exactly the declared set of streams — anything else is a hard error    |
| Interleaving   | muxer bounds skew; always pass `-max_interleave_delta` explicitly      |

The block **validates the structure** of this profile at `start()` —
stream kinds vs the instance shape, codecs, channel count — and fails
loudly on any mismatch; it never adapts. The audio rate and video
geometry are *not* validated but **adopted** from the headers: the ffmpeg
command is the single source of truth, and making the downstream
flowgraph match it (resampler ratios, consumer geometry) is the user's
responsibility. Adopted values are exposed as `audio_rate()` /
`video_width()` / `video_height()` (valid after the flowgraph has
started; 0 before, or if the instance lacks that stream), and a
mid-stream *change* of the adopted format remains a hard error. All
flexibility lives on ffmpeg's input side; ffmpeg exists to conform
arbitrary media to a fixed profile per run.

Error model: GNU Radio cannot propagate exceptions out of a block thread
(they would kill one thread and hang the rest of the flowgraph), so
post-constructor failures — open/spawn failure, contract validation,
mid-stream violations — are logged at ERROR level with an actionable
message and terminate the flowgraph cleanly. After `tb.run()`/`tb.wait()`
returns, `last_error()` returns that message (empty string = clean EOF),
so scripts can tell failure from end-of-media. Constructor misuse
(both/neither of `uri`/`command`, invalid parameters) still raises
normally in the calling thread.

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

## Buffering & synchronization

**Full reference: [docs/buffering.md](docs/buffering.md)** — the complete
chain of reasoning (topology, clocking, NUT interleaving, the
demuxer-deadlock cycle and why buffer sizing genuinely eliminates it,
allocation timing, fine-tuning, failure modes). Read it before changing
any buffer size or pacing behavior. The short version:

- A multi-output demuxer with bounded buffers plus a synchronous
  downstream combiner can deadlock; because the NUT muxer bounds
  interleave skew (`-max_interleave_delta`), buffers larger than the
  maximum interleave burst provably break the cycle.
- Since the format is adopted from the headers *after* GR has already
  allocated the buffers (allocation happens during `tb.start()` setup,
  before `block::start()` runs), the block requests fixed generous
  defaults in its constructor: **192000 items per audio port** (1 s at
  the 192 kHz cap) and **4 × 1920×1080×3 bytes ≈ 24.9 MB** on the video
  port. Streams beyond the caps fail promptly at start.
- These are plain `set_min_output_buffer` requests and remain
  overridable: call `set_min_output_buffer(...)` after construction in
  Python, or set GRC's Advanced → *Min Output Buffer* (non-zero
  overrides; GRC emits the setter after the make, so the untouched field
  keeps the defaults). **Warning:** shrinking below the stream's
  interleave burst re-opens the deadlock window the defaults exist to
  close; `-max_interleave_delta` on the ffmpeg side is the other half of
  that contract.

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
src = nut.nut_source(uri="/tmp/stream.nut", audio_channels=1,
                     emit_video=False)
# or spawn mode:
src = nut.nut_source(uri="", audio_channels=1, emit_video=False,
                     command="ffmpeg -i song.flac -vn "
                             "-af aresample=48000:async=1 -ac 1 "
                             "-c:a pcm_f32le -max_interleave_delta 500000 "
                             "-f nut pipe:1")
# after tb.start(): src.audio_rate() / src.video_width() /
# src.video_height() report the adopted format; src.last_error() is ""
# on clean EOF and carries the actionable message on failure.
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

## Plumbing modes

Exactly one of `uri` / `command` must be set:

- **External mode** (`uri`): ffmpeg is launched externally (shell script,
  systemd unit) and writes the FIFO/file named by `uri`. See
  `examples/fm_mono_tx.sh`. `start()` blocks until the writer delivers the
  NUT stream headers (they are needed for validation), so start ffmpeg
  first — or at least make sure it will start.
- **Spawn mode** (`command`, POSIX-only): the block runs the full
  user-authored shell command via `/bin/sh -c` and reads the NUT stream
  from an anonymous pipe on its stdout. The command must write NUT per the
  contract to stdout — take the reference commands above and replace
  `> "$FIFO"` with nothing (keep `-f nut pipe:1`), e.g.:

  ```sh
  # audio, mono
  ffmpeg -i song.flac -vn -af aresample=48000:async=1 -ac 1 \
    -c:a pcm_f32le -max_interleave_delta 500000 -f nut pipe:1
  ```

  Anything goes — extra filters, multiple inputs, pipelines like
  `curl -s URL | ffmpeg -i - ...` — as long as stdout is
  contract-conforming NUT; the command carries the same trust level as a
  shell script you would write anyway, and start-time validation is the
  guardrail (its errors point back at the spawn command). The child runs
  in its own process group with stdin from `/dev/null` and stderr
  inherited; it is spawned in `start()` (no "start ffmpeg first" footgun)
  and SIGTERM'd/reaped on `stop()` — no zombies.

Notes:

- On EOF the block signals done; a dead writer (broken pipe / child exit)
  is EOF plus a logged warning, with the spawned command's exit status
  logged distinctly.
- Looping: the block itself never loops. In spawn mode put
  `-stream_loop -1` before `-i` for seamless in-ffmpeg looping; a
  recorded `.nut` file can likewise be looped by external means (e.g. an
  external ffmpeg with `-stream_loop`) if ever needed.

## Design

See `gr-nut-design.md` (kept next to this repo) for the full design
document: architecture, contract rationale, scheduler mechanics, pts
handling, and the QA plan the tests in `python/nut/qa_nut_source.py`
implement.
