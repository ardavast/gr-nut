# gr-nut examples

Example flowgraphs for both directions: a mono FM transmitter and a
file-based video-path check for `nut_source`, and an audio recorder plus
a full-circle round-trip check for `nut_sink`. The interface contract
and the ffmpeg command shapes they rely on are documented in
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

## Audio recording (`audio_record.grc` / `.py`)

The sink-side mirror of the FM transmitter's ingest: signal generator →
**NUT Audio Sink** with its default spawn command — the block feeds NUT
to a spawned `ffmpeg -y -loglevel warning -i pipe:0 /tmp/recording.flac`
on its stdin. A K0 chain (no pacer anywhere): the graph free-runs faster
than real time and renders the tone in a fraction of its duration.

```sh
python3 audio_record.py --seconds 10
ffprobe /tmp/recording.flac   # -> flac, 48000 Hz, mono, 10.0 s
```

The interesting part is the ending: when the `head` block runs out, the
sink flushes the muxer, writes the NUT trailer, closes the pipe (ffmpeg's
EOF) and *waits* for ffmpeg to finalize the file — the child exiting on
its own is the normal end of a recording; it is only killed after the
flush timeout. External-mode variant: pass `--command "" --uri FIFO` and
run your own `ffmpeg -i FIFO ...` against it.

## Round-trip validation (`roundtrip_check.sh`)

The sink counterpart of `video_check.sh`, unattended: known vectors →
`nut_sink` → file → `nut_source` → vector sinks, compared byte for byte
(three legs: full-size audio, full-size video, and a joint A/V leg),
plus ffprobe checks of the written headers and per-packet video
durations. The sink and the source share no code paths, so a green run
certifies both directions of the contract at once:

```sh
./roundtrip_check.sh 320 240 25 2
# -> OK: audio 96000 samples @ 48000 Hz round-tripped sample-exact
# -> OK: video 50 frames of 320x240 rgb24 round-tripped byte-exact
# -> OK: joint A/V (11520 samples + 6 frames of 64x48) round-tripped, ...
```

(The joint leg is deliberately small: GNU Radio ends a sink when its
*first* input is exhausted, so a finite multi-input flowgraph is only
byte-complete when every stream's payload fits its input buffer — see
the comment in the script. Real recorders stop via Ctrl-C/`tb.stop()`,
not by exhausting finite vectors.)

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
