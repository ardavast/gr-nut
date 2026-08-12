# gr-nut

A single, reusable, well-defined interface between **ffmpeg** and **GNU
Radio** for analogue transmission chains: ffmpeg is the codec/format/sync
layer, GNU Radio is the physical layer, and a **NUT stream over a
pipe/FIFO** is the socket between them.

The module contains one block, `nut.nut_source`: it ingests a raw-payload
NUT stream and emits deinterleaved float audio (one port per channel) and
zero or more rgb24 video byte streams (one port per stream). It is a
generic ingest block — no TV- or radio-specific logic; modulators are
ordinary flowgraphs built from stock GR blocks (see `examples/` for a
mono FM transmitter).

Why this split: ffmpeg already decodes anything, scales anything, and
repairs broken timing (variable frame rates, drifting sample clocks) —
so it does, conforming arbitrary media to one strict raw profile per
run. GNU Radio never sees a codec. The flowgraph sink (SDR, audio
device) is the only clock in the chain: no Throttle blocks, no `-re`;
ffmpeg is paced purely by pipe backpressure and idles at ~zero CPU while
streaming.

## Quick start

In GRC, the blocks live under **[NUT]**: *NUT Audio Source*, *NUT Video
Source*, and *NUT A/V Source* — the same C++ block in three fixed port
shapes. Each ships with a working default **Spawn Command**; drop in the
audio one, put any mp3 at `/tmp/song.mp3`, and it plays. The command is
a full user-authored ffmpeg oneliner — all of ffmpeg's power (filters,
network inputs, shell pipelines) stays available.

The same from Python:

```python
from gnuradio import nut

# spawn mode: the block runs ffmpeg itself
src = nut.nut_source(uri="", audio_channels=1, video_streams=0,
                     command="ffmpeg -i song.flac -vn "
                             "-af aresample=48000:async=1 -ac 1 "
                             "-c:a pcm_f32le -max_interleave_delta 500000 "
                             "-f nut pipe:1")

# external mode: ffmpeg writes a FIFO, e.g. from a systemd unit
src = nut.nut_source(uri="/tmp/stream.nut", audio_channels=1,
                     video_streams=0)
```

After the flowgraph starts, `src.audio_rate()` / `src.video_width(i)` /
`src.video_height(i)` report the format adopted from the stream headers;
after it stops, `src.last_error()` is `""` on clean EOF and carries an
actionable message on failure.

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

## Documentation

- **[docs/contract.md](docs/contract.md)** — the interface contract: the
  NUT profile, validation vs adoption, reference ffmpeg invocations, the
  ports and their tags, external vs spawn plumbing, and the error model.
- **[docs/clocking.md](docs/clocking.md)** — where time comes from: the
  clock-census rule, which chain configurations are supported, what
  ffmpeg's conformance filters do and don't do, and how two-clock chains
  fail.
- **[docs/buffering.md](docs/buffering.md)** — buffering and
  synchronization: NUT interleaving, the demuxer-deadlock analysis
  behind the default buffer sizes, and how to fine-tune them.
- **[examples/](examples/README.md)** — a mono FM transmitter (spawn and
  external variants) and a byte-exact video-path check.
- `python/nut/qa_nut_source.py` — the executable spec: the QA tests cover
  contract validation, header adoption, pts trimming, stream tags, spawn
  mode, and the failure paths.

Prior art: [gr-mediatools](https://github.com/osh/gr-mediatools) (2012)
pioneered libav-inside-a-GR-block; gr-nut is a refined take on the same
idea — decode moved out to an ffmpeg *process* (keeping its full CLI
power), a strict contract at the pipe, and demux-only inside the block.
