# The interface contract

This is the normative description of what `nut_source` expects on its
input and what it emits on its ports. The ffmpeg command is the single
source of truth for the stream format; the block validates the
*structure* and adopts the rest. For the clock model (which chain
configurations are supported) see [clocking.md](clocking.md); for
buffering and deadlock mechanics see [buffering.md](buffering.md).

## The NUT profile

| Property       | Value                                                                  |
|----------------|------------------------------------------------------------------------|
| Container      | NUT, streamed (no seeking), read from a pipe/FIFO or file              |
| Audio codec    | `pcm_f32le`, interleaved                                               |
| Audio rate     | read from the stream headers (adopted at start; cap 192 kHz)           |
| Audio channels | as declared to the block (structural: fixes the port count)            |
| Video codec    | one or more `rawvideo`/`rgb24` streams (exactly `video_streams` of them, one byte port each, in ffmpeg `-map` order)|
| Video geometry | read from the stream headers, adopted per stream (cap 1920×1080 each); ffmpeg does ALL scaling |
| Video rate     | read from the stream headers; CFR enforced by ffmpeg (`-fps_mode cfr`) |
| Stream layout  | exactly the declared set of streams — anything else is a hard error    |
| Interleaving   | muxer bounds skew; always pass `-max_interleave_delta` explicitly      |

## Validation vs adoption

The block **validates the structure** of this profile at `start()` —
stream kinds vs the instance shape, codecs, channel count — and fails
loudly on any mismatch; it never adapts. The audio rate and video
geometry are *not* validated but **adopted** from the headers: making the
downstream flowgraph match them (resampler ratios, consumer geometry) is
the user's responsibility. Adopted values are exposed as `audio_rate()` /
`video_width(i)` / `video_height(i)` (per video stream, `i` defaulting to
0; valid after the flowgraph has started; 0 before, or if the instance
lacks that stream), and a mid-stream *change* of the adopted format
remains a hard error. All flexibility lives on ffmpeg's input side;
ffmpeg exists to conform arbitrary media to a fixed profile per run.

NUT stream headers are immutable for the stream's lifetime (repeated
headers must be byte-identical), so a conforming writer *cannot* change
rate or geometry mid-stream — the mid-stream checks guard against
malformed streams, not against legitimate format changes.

## Reference ffmpeg invocations

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

Rate-defective sources (VFR video, lying sample rates, gaps) are repaired
by ffmpeg *before* the pipe: `-fps_mode cfr` and `aresample=async=1` run
the same drop/dup/stretch logic a media player would run at play time, at
mux time instead — a transmit sample clock cannot bend.

## The ports

**Audio:** one float port per declared channel, deinterleaved. No stream
tags — after the initial pts trim, the sample index *is* the timeline.

**Video:** one `unsigned char` port per declared stream: rgb24 frames
back to back, row-major. A stream tag is attached to the first byte of
every frame with keys `pts` (int64, NUT stream timebase), `width`,
`height`. Steady-state consumers must not *need* the tags (geometry is
contractual); they exist for diagnostics and alignment.

When both audio and video are declared, the initial pts offset between
the streams is trimmed at startup so all ports start at the same media
time (within one audio sample / one video frame). pts discontinuities
beyond a few ms are logged as warnings, never resynced.

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

Looping: the block itself never loops. In spawn mode put
`-stream_loop -1` before `-i` for seamless in-ffmpeg looping; a recorded
`.nut` file can likewise be looped by external means (e.g. an external
ffmpeg with `-stream_loop`) if ever needed.

## Error model

GNU Radio cannot propagate exceptions out of a block thread (they would
kill one thread and hang the rest of the flowgraph), so post-constructor
failures — open/spawn failure, contract validation, mid-stream
violations — are logged at ERROR level with an actionable message and
terminate the flowgraph cleanly. After `tb.run()`/`tb.wait()` returns,
`last_error()` returns that message (empty string = clean EOF), so
scripts can tell failure from end-of-media. Constructor misuse
(both/neither of `uri`/`command`, invalid parameters) still raises
normally in the calling thread.

On EOF the block signals done; a dead writer (broken pipe / child exit)
is EOF plus a logged warning, with the spawned command's exit status
logged distinctly. A contract mismatch or failing command produces an
ERROR log that says exactly which ffmpeg option to fix
(`-ac`/`-c:a`/geometry/`-map`).
