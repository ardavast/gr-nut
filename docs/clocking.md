# Clocking in gr-nut

Which chain configurations are supported, why, and how the unsupported
ones fail. The companion document [buffering.md](buffering.md) covers the
buffer-level mechanics (deadlock, sizing); this one covers the chain-level
question: **where does time come from, and how many clocks are in play?**
It applies to `nut_source` today and to the planned reverse block
(`nut_sink`, GR → ffmpeg) symmetrically.

## 1. The rule: count the clocks

Walk the *entire* chain — media origin, ffmpeg, the pipe, the flowgraph,
the final sink — and count the **pacers**: the elements that force their
own rate on the chain by refusing to run faster or slower.

- an SDR (TX or RX) — always a pacer,
- a sound card, input or output — always a pacer,
- a capture device (microphone, webcam, HDMI grabber) — always a pacer,
- a **live network input** (an RTSP camera, a pulled stream) — always a
  pacer: the data does not exist until the remote crystal creates it,
  so you cannot read faster than the sender sends. The remote crystal
  counts even though it is not in your room,
- ffmpeg's `-re` flag — a deliberate *artificial* pacer (see below).

Not pacers: files, generators, filters, pipes — and, importantly,
**network outputs**. A UDP/RTP send is fire-and-forget and never
blocks; a TCP push (icecast, RTMP) blocks only on congestion, not at
any crystal's rate. Pushing a live stream out does not add a clock to
*your* chain — it exports the drift problem to each receiver's jitter
buffer, which is normal streaming and exactly what player machinery
exists for.

`-re` is not forbidden per se — it is forbidden *as a second pacer*.
Its one legitimate use is a chain whose census is otherwise zero but
whose output must be real time (e.g. a generator pushed out over
RTP/UDP: nothing else paces, and without a clock ffmpeg would blast the
stream at CPU speed). In every gr-nut source chain something else
already paces or free-running is intended, so there `-re` is always
wrong.

- **0 clocks** — the chain free-runs at CPU speed. Batch processing and
  faster-than-real-time rendering. Always fine.
- **1 clock, anywhere** — that device is the master; backpressure
  distributes its rate to every other stage (§2). Always fine. This is
  the design's home territory.
- **2+ clocks** — the crystals differ by ppm, every buffer between them
  is bounded, so the chain **must** eventually glitch (§5). Out of
  contract.

## 2. How one clock paces everything

Neither ffmpeg nor GNU Radio has a clock of its own; both are dataflow
engines that run when data can move and block when it cannot. A single
real-time device therefore paces the whole chain through blocking I/O,
in either direction:

- device downstream (SDR TX): the sink consumes at its rate → GR buffers
  drain at that rate → `nut_source` reads the pipe at that rate →
  ffmpeg's writes block when the pipe is full. ffmpeg runs ~zero CPU
  while streaming and can never run ahead by more than the pipe plus
  GR's buffers.
- device upstream (capture, SDR RX): the producer emits at its rate and
  everything downstream processes on arrival.

No Throttle blocks, ever: Throttle is a *fake clock* (a second one, or a
first one where zero is wanted).

Inside the flowgraph the single-clock guarantee has one obligation:
**all rate relationships must be exact rational ratios** (rational
resamplers, never approximate floats). One clock plus exact ratios makes
inter-stream drift identically zero — not small, zero.

## 3. The configuration matrix

Source chains (ffmpeg → pipe → flowgraph):

| # | ffmpeg input | flowgraph end | clocks | verdict |
|---|--------------|---------------|--------|---------|
| S0 | file / generator | file / processing | 0 | free-run; batch processing |
| S1 | file / generator | SDR TX, audio sink | 1 | **the home case** — the sink paces everything |
| S2 | live capture, RTSP | file / processing | 1 | capture paces ffmpeg; the pipe paces GR |
| S3 | live capture, RTSP | SDR TX, audio sink | 2 | **out of contract** (§5) |

Sink chains (flowgraph → pipe → ffmpeg; `nut_sink`, planned):

| # | flowgraph end | ffmpeg output | clocks | verdict |
|---|---------------|---------------|--------|---------|
| K0 | generator | file | 0 | free-run; faster-than-real-time rendering |
| K1 | SDR RX | file | 1 | recording; ffmpeg must keep up *on average* |
| K2 | generator | audio device | 1 | ffmpeg's output paces the graph — mirror of S1 |
| K2b | generator | network push (RTP, icecast) | 0 | needs a pacer: this is `-re`'s one legitimate home |
| K3 | SDR RX | audio device | 2 | **out of contract** (§5) |
| K3b | SDR RX | network push (RTP, icecast) | 1 | fine — SDR paces; the crystal mismatch is exported to each receiver's jitter buffer |

S1/K2 are mirrors: one clock at the far end, distributed backward
through the pipe. S3/K3 are the same disease in both directions. K3b is
the instructive asymmetry: a network *input* is a pacer but a network
*output* is not (§1), so pushing an SDR-clocked stream to the network
is single-clock on our side — every receiver then faces our SDR's
crystal, with the standard player remedies (latency creep, resync,
adaptive buffering) on their side.

Watch for the **hidden clock**: after ffmpeg's demuxer, an RTSP input is
indistinguishable from a file — same packets, same pts — but its
timestamps were written by a remote crystal, and pulling it into a TX
chain silently creates configuration S3. Whether a timeline has a
physical clock behind it is not visible in the data; it must be known
from provenance.

## 4. What ffmpeg contributes — and deliberately does not

ffmpeg's roles in the chain are exactly three, all clock-free:

1. **Rate conversion** (`aresample=48000`, scaling): real DSP, exact
   rational ratios, converts the *declared* media rate. No-op only when
   the source already matches.
2. **Conformance** (`aresample=async=1`, `-fps_mode cfr`): per stream,
   compares the accumulated sample/frame *count* against the declared
   *pts* and edits the data (fill/trim silence, dup/drop frames) until
   count and label agree. Both operands are in-band integers; no wall
   clock is consulted, and it runs identically at 40× real time. This
   is what repairs VFR video, dropouts, and lying timestamps *before*
   the pipe — and what makes "sample index = timeline" a valid contract
   on the GR side.
3. **Pacing by blocking I/O** (§2): not a capability, just UNIX.

What ffmpeg does **not** contain — by design, unlike GStreamer or
PipeWire, which ship clock-slaving infrastructure (pipeline clocks,
skew/resample sink modes, adaptive resampling between clock domains) —
is any **feedback loop between two physical clocks**. No occupancy
servo, no rate trimming against a local device. This absence is
load-bearing for gr-nut: a muxer that quietly rate-adapted to some local
clock would be a second, hidden servo fighting the SDR's backpressure.
The single-clock theorem works *because* every stage between the media
and the one real clock is a deterministic, clockless function.

Corollary — conformance and clock recovery are disjoint mechanisms and
neither can do the other's job:

|                      | conformance (`async`/`cfr`) | occupancy servo (not in this design) |
|----------------------|-----------------------------|--------------------------------------|
| observable           | label − count (in-band)     | buffer fullness (physical)           |
| detects              | defects in one timeline     | disagreement of two clocks           |
| control              | feed-forward, deterministic | feedback loop, tuned bandwidth       |
| blind to             | clock drift (labels stay self-consistent) | whether fill changes are defects or drift |

## 5. The two-clock failure mode

Two crystals nominally at the same rate differ by some Δ (crystal
tolerance: tens of ppm; 50 ppm = 50 µs of drift per second). All
buffering between them is bounded, fills or drains at rate Δ, and the
chain glitches when the slack runs out:

    time to glitch ≈ buffered seconds ÷ Δ

Concrete: with `nut_source`'s default ~4 s audio buffering (192 000
items at 48 kHz) and 50 ppm, that is ~80 000 s ≈ **22 hours** — the
worst kind of failure: passes every test, survives a soak, dies during
the event. With a 200 ms cushion it is ~67 minutes.

Where the damage lands: everything between the two clocks — GR ring
buffers, the pipe, ffmpeg's internal queues — is bounded but *blocking*;
none of it overflows, it just stalls, and the stall propagates until it
reaches the one element that cannot block: a real-time device. A
consumer that cannot wait underruns (SDR TX, DAC); a producer that
cannot pause overruns (the kernel's capture ring — the ADC keeps
sampling whether or not anyone reads). **The drift damage always
materializes at one of the clocks, never in the middle.** A side effect
worth knowing: capture overruns become timestamp gaps, which
`aresample=async` then dutifully converts to silence — the conformance
layer keeps the timeline consistent while the audio glitches.

Symptoms by direction:

- clock at the sink faster than the source's → buffers drain →
  periodic **underruns** (TX gap on air, player stutter).
- source's clock faster → buffers fill → either unbounded **latency
  growth** (if the slack can grow) or periodic **overruns** at the
  producer (capture drops).
- `nut_sink` special case (K3): its pts are synthesized from sample
  counts, hence always self-consistent — the drift is *invisible
  in-band* and appears only as pipe/buffer occupancy. Nothing downstream
  can detect it, let alone fix it.

None of ffmpeg's flags help: `async`/`cfr` compare a stream against its
own labels, and two honest clocks produce self-consistent labels each.
The drift lives *between* timelines, in a quantity (buffer fill) that
nothing in the chain observes.

## 6. If you genuinely need two clocks

Options, in order of preference:

1. **Make them one clock.** Reference-discipline both devices (shared
   10 MHz/PPS, SyncE-style infrastructure). Nothing to estimate;
   the chain returns to §1's one-clock case.
2. **Add an explicit servo in the flowgraph** — an occupancy-driven
   fractional resampler (e.g. a PI loop on buffer fill trimming
   `pfb_arb_resampler`'s rate). This is deliberate, visible, tunable —
   the opposite of a hidden framework servo. It belongs GR-side because
   that is where the occupancy observable lives.
3. **Accept the glitch period** and size buffers for the mission
   duration (§5's formula, solved for buffering).

What is *not* an option is expecting the pipe, the block, or ffmpeg to
absorb the drift: bounded buffers plus deterministic stages have no
degree of freedom to absorb anything.

## 7. Checklist

- Count the pacers in the whole chain; the count must be ≤ 1.
- No Throttle. No `-re` — unless the census is zero and the output must
  be real time, in which case `-re` *is* the chain's one clock.
- Exact rational resampler ratios everywhere in the flowgraph.
- Conform in ffmpeg (`aresample=async=1`, `-fps_mode cfr`): the pipe
  must carry a stream whose position *is* its timeline.
- Live network inputs are pacers even though they look like files;
  network outputs are not pacers — they export drift to the receivers.
- Two clocks unavoidable → discipline them to one reference, or add an
  explicit occupancy servo, or budget buffers for the mission duration.
