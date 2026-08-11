# gr-nut examples

## The contract in one paragraph

ffmpeg is the codec/format/sync layer, GNU Radio is the physical layer, and
a NUT stream over a FIFO is the socket between them. ffmpeg decodes
anything and conforms it to a strict raw profile: audio `pcm_f32le`
interleaved at the declared rate/channel count; video `rawvideo` `rgb24` at
exactly the declared geometry, CFR. `nut_source` validates the profile at
start (hard error on any mismatch) and never adapts rates. The flowgraph
sink (SDR, audio device) is the only clock — **no Throttle blocks, no
`-re`**; ffmpeg is paced purely by pipe backpressure. Always pass
`-max_interleave_delta` explicitly so the muxer bounds A/V interleave skew.

Reference ffmpeg commands:

```sh
# audio-only, mono (M1)
ffmpeg -i INPUT \
  -vn -af "aresample=48000:async=1" -ac 1 \
  -c:a pcm_f32le \
  -max_interleave_delta 500000 \
  -f nut pipe:1 > "$FIFO"

# audio-only, stereo: same with -ac 2

# audio + video (generic)
ffmpeg -i INPUT \
  -filter_complex "[0:v]fps=FPS,scale=W:H:force_original_aspect_ratio=decrease,pad=W:H:-1:-1,setsar=1[v]; \
                   [0:a]aresample=48000:async=1[a]" \
  -map "[v]" -map "[a]" \
  -c:v rawvideo -pix_fmt rgb24 -c:a pcm_f32le \
  -fps_mode cfr -max_interleave_delta 500000 \
  -f nut pipe:1 > "$FIFO"
```

## M1 — mono FM transmitter (`fm_mono_tx.grc` / `.sh`)

Chain: `nut_source(1 ch, 48 kHz)` → rational resampler 48k→200k (exact
25/6) → WBFM mod (±75 kHz deviation, 50 µs pre-emphasis, mono, no pilot) →
rational resampler 200k→8M (exact 40/1) → Soapy HackRF sink @ 8 MSPS
(HackRF's practical minimum). All rate ratios are exact rationals, so with
the single HackRF clock the inter-stream drift is identically zero.

Two ways to run it:

**Spawn mode (the ergonomics showcase):** the block runs ffmpeg itself —
one process to start, one Ctrl-C to stop, no FIFO:

```sh
python3 fm_mono_tx.py --freq 99.9e6 --command \
  "ffmpeg -i song.flac -vn -af aresample=48000:async=1 -ac 1 \
   -c:a pcm_f32le -max_interleave_delta 500000 -f nut pipe:1"
```

(For seamless looping add `-stream_loop -1` before `-i` — looping lives
inside ffmpeg, never in the block.)

**External mode (the ops variant):** `fm_mono_tx.sh` creates the FIFO,
starts ffmpeg into it, and passes `--uri` — the shape you'd put in a
systemd unit:

```sh
./fm_mono_tx.sh some_music.flac 99.9e6 40
```

Tune an FM receiver to 99.9 MHz. Acceptance checks: audio is clean, and
`top` shows ffmpeg CPU near zero while streaming (backpressure pacing).
The `.sh`'s third argument is the HackRF TX VGA gain (0–47 dB); pass
`--amp 1` for the extra +14 dB stage. Mind your local spectrum
regulations.

The `.grc` file opens in gnuradio-companion (with `gr-nut` installed);
`fm_mono_tx.py` is the pre-compiled version of it.

## M3 — video path validation (`video_to_file.grc`, `video_check.sh`)

`video_to_file.grc`: **NUT Video Source** (video-only `nut_source`) →
File Sink. Dumps the rgb24
frame payload back to back into a file. No modulator, no display — this
example only proves the byte path.

`video_check.sh` runs the whole loop unattended: ffmpeg `testsrc2` pattern
→ NUT FIFO → flowgraph → `frames.rgb`, then renders the same pattern
directly with ffmpeg and compares bit for bit:

```sh
./video_check.sh 320 240 25 2
# -> OK: 50 frames of 320x240 rgb24, byte-exact
```

## Notes

- External mode: start order is forgiving (both sides block on the FIFO
  until the peer appears), but the flowgraph waits inside `start()` until
  ffmpeg has written the NUT headers — so if nothing happens, check that
  ffmpeg is actually running and writing to the right FIFO. Spawn mode has
  no such footgun: the block starts its own writer.
- If `start()` throws, the message says exactly which ffmpeg option to fix
  (`-ac`/`-ar`/`-c:a`/geometry/`-map`).
