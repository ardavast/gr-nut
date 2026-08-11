# Buffering & synchronization in gr-nut

This is the reference explanation of how `nut_source` keeps an
audio+video flowgraph deadlock-free and drift-free, why its output
buffers default to the sizes they do, and what to do when the defaults
don't fit. Nothing here is required reading to *use* the block; it is
required reading before *changing* any buffer size or pacing behavior.

## 1. The topology

```
                       (one interleaved byte stream)
  ffmpeg ── pipe/FIFO ───────────────────────────────┐
                                                     v
                                              ┌────────────┐
                                              │ nut_source │  multi-output
                                              │  (demuxer) │  demuxer block
                                              └─────┬──┬───┘
                              audio (float) ◄───────┘  └───────► video (bytes)
                                    │                                │
                              [GR ring buffer]                 [GR ring buffer]
                                    │                                │
                                    └──────────► synchronous ◄───────┘
                                                 combiner / sink
                                                 (e.g. a TV modulator)
```

Every GR stream connection is a **bounded ring buffer**. `nut_source`
reads one interleaved NUT stream and produces onto independent per-stream
paths that typically reconverge downstream in a block that consumes its
inputs in lockstep (a synchronous combiner, correlator, or modulator that
needs both audio and video for the same instant). That reconvergence is
what makes buffering here more than a performance knob — see §4.

## 2. Clocking

The flowgraph sink (SDR, audio device) is the **only clock** in the whole
chain. There is no Throttle block, and ffmpeg runs without `-re`: it is
paced purely by pipe backpressure — when GR is not consuming, ffmpeg's
write into the pipe blocks, so it can never run ahead by more than the
pipe plus GR's buffers, and it costs ~zero CPU while streaming.

All rate relationships downstream must be **exact rational ratios**
(rational resamplers, never float "approximately" ratios). With a single
clock and rational ratios, inter-stream drift is *identically zero* — not
small, zero. The steady-state audio↔video skew is:

    skew = initial pts offset (removed at start by the pts trim)
         + fixed buffering differences between the two paths

Both terms are constants: the skew is **bounded, not growing**.

## 3. NUT interleaving

The NUT muxer interleaves audio and video packets by time and **bounds
how far apart the two streams can get in the byte stream**;
`-max_interleave_delta` (microseconds) makes that bound explicit — always
pass it. The demuxer delivers packets in stream order, so the delivered
media times of the two output ports can never diverge by more than the
interleave window. This bound is what makes buffer sizing a *proof*
rather than a heuristic (§5).

## 4. The deadlock mechanism

A multi-output demuxer with bounded output buffers and a synchronous
downstream consumer can deadlock:

```
              ┌───────────────────────────────────────────┐
              │  demuxer blocked: video buffer FULL,      │
              │  next packet in the NUT stream is video   │
              └────────────────┬──────────────────────────┘
                               │ cannot deliver the audio the
                               │ combiner is waiting for
                               v
              ┌───────────────────────────────────────────┐
              │  combiner blocked: needs audio to consume │
              │  the next lockstep chunk — will not drain │
              │  the full video buffer until it gets it   │
              └────────────────┬──────────────────────────┘
                               │ cannot free video buffer space
                               └────────────► back to the top
```

Producer waits on consumer, consumer waits on producer: a cycle.
Ordinary starvation ("one buffer happens to be empty") self-resolves —
the producer simply runs and fills it. This cycle does **not**
self-resolve, because the only process that could break it (the demuxer
delivering audio) is the one that is blocked, and the only block that
could unblock it (the combiner draining video) cannot proceed without
that audio. Both parties are correct locally; the system is stuck
globally.

## 5. Why sizing is a real fix, not a postponement

Because the muxer bounds interleave skew (§3), there is a maximum burst
of stream-A bytes that can arrive between two consecutive stream-B
packets. The fix is the inequality:

    per-port buffer  >  maximum interleave burst of that stream

If it holds, the demuxer can always flush an entire burst of one stream
into its buffer *without blocking*, and therefore always reaches the
other stream's next packet. The delivering side of the cycle can always
make progress ⇒ the cycle cannot close ⇒ no deadlock — for any schedule,
not just the ones we tested. Bigger buffers are not "hiding" the problem;
the bound is what eliminates it.

## 6. The concrete numbers

`nut_source` requests these minimum output buffer sizes in its
constructor:

- **audio ports: 192000 items** per port. Items are 4-byte floats, so
  ~768 kB per channel — exactly 1 s of audio at the 192 kHz cap, ≥ 1 s at
  any adopted rate.
- **video port: 4 frames at the 1080p cap**:

      1920 × 1080 × 3 bytes = 6 220 800 bytes per rgb24 frame
      × 4 frames            = 24 883 200 bytes  (~24.9 MB)

  Four frames comfortably exceeds the reference interleave window
  (`-max_interleave_delta 500000` = 0.5 s only caps *sparse* streams; the
  muxer interleaves tightly in practice) while staying a trivial
  allocation on any machine that can process raw video at all. For
  comparison, GR's default buffers are tens of kB — a single raw frame is
  megabytes, so without this request the video path would be broken out
  of the box.

The audio rate and video geometry themselves are **adopted from the NUT
headers at start** (the ffmpeg command is the single source of truth). If
the adopted format exceeds a cap — audio above 192 kHz, or a frame larger
than 1920×1080×3 bytes — the block fails promptly at start with a message
naming the offending size and the cap (`last_error()` carries it; the
flowgraph terminates cleanly). The fix is to scale down / resample in the
ffmpeg command.

## 7. Why caps instead of exact fits: buffer allocation timing

GR allocates stream buffers during `tb.start()` flowgraph setup —
**before** `block::start()` runs and can read the NUT headers. Deferring
`set_min_output_buffer` until the format is known therefore does not
work; by then the buffers already exist. That is the entire reason the
defaults are fixed generous caps rather than exact per-format fits.

## 8. Fine-tuning

The defaults are plain `set_min_output_buffer` *requests*; the standard
GR override mechanism applies — the last request before `tb.start()` wins
at allocation time:

- **Python:** after constructing the block,
  `src.set_min_output_buffer(port, items)` (or the one-argument form for
  all ports).
- **GRC:** Advanced tab → *Min Output Buffer*. Zero (the default) leaves
  the block's caps in place; a non-zero value overrides them.

**Warning:** shrinking a buffer below the stream's actual interleave
burst re-opens the §4 deadlock window that the defaults exist to close.
If you shrink GR-side buffers, shrink the muxer-side window to match —
`-max_interleave_delta` on the ffmpeg command is the other half of the
contract (§5's inequality has a term on each side of the pipe).

## 9. Failure modes summary

| Situation | Behavior |
|---|---|
| Clean EOF (media ended) | ports drain, flowgraph terminates, `last_error()` == "" |
| Spawned command dies mid-stream | EOF + WARN log with the child's exit status; flowgraph terminates; stderr of the child is on the console |
| Startup failure (bad path/URL, non-NUT output, structural mismatch, cap exceeded) | ERROR log with an actionable message; flowgraph terminates promptly; `last_error()` non-empty |
| Mid-stream contract change (geometry/layout) | same as startup failure, at the point of detection |
| Rate-defective source (VFR video, lying sample rate, gaps) not repaired in ffmpeg | one path's buffer pins full while the sink underruns or skew accumulates — the block will not adapt by design. The fix belongs in the ffmpeg command: `-fps_mode cfr` and `aresample=async=1` run the same drop/dup/stretch logic a media player would run at play time, at mux time instead, because a transmit sample clock cannot bend |

*(See `gr-nut-design.md` for the full design rationale; this document is
the user-facing distillation of its §2, §4.6 and §4.7.)*
