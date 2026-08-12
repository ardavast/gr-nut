# gr-nut examples

Two example flowgraphs: a mono FM transmitter (the first real
transmission chain) and a file-based video-path check. The interface
contract and the ffmpeg command shapes they rely on are documented in
[docs/contract.md](../docs/contract.md); the buffering/clocking model in
[docs/buffering.md](../docs/buffering.md).

## Mono FM transmitter (`fm_mono_tx.grc` / `.sh`)

Chain: `nut_source` (1 ch; the ffmpeg command sets 48 kHz, which the
block adopts) → rational resampler 48k→200k (exact 25/6) → WBFM mod
(±75 kHz deviation, 50 µs pre-emphasis, mono, no pilot) → rational
resampler 200k→8M (exact 40/1) → Soapy HackRF sink @ 8 MSPS (HackRF's
practical minimum). All rate ratios are exact rationals, so with the
single HackRF clock the inter-stream drift is identically zero.

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

## Video path validation (`video_to_file.grc`, `video_check.sh`)

`video_to_file.grc`: **NUT Video Source** (video-only `nut_source`) →
File Sink. Dumps the rgb24 frame payload back to back into a file. No
modulator, no display — this example only proves the byte path.

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
- On a contract mismatch or a failing command the flowgraph stops by
  itself and an ERROR log says exactly which ffmpeg option to fix
  (`-ac`/`-c:a`/geometry/`-map`); from Python,
  `src.last_error()` returns the same message ("" means clean EOF).
