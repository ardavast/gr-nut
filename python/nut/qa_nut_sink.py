#!/usr/bin/env python
# -*- coding: utf-8 -*-
#
# Copyright 2026 Ardavast Dayleryan.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

"""QA for nut::nut_sink (sink design doc §3-§5).

The centerpiece is the round trip through the already-trusted nut_source:
known vectors -> nut_sink -> file -> nut_source -> vector sinks, byte
exact end to end, plus ffprobe checks of the written headers. Tests that
need the ffmpeg/ffprobe CLI are skipped cleanly if it is not installed.
"""

import json
import os
import shutil
import subprocess
import tempfile
import threading
import time
import unittest
from fractions import Fraction

import numpy as np
import pmt
from gnuradio import blocks, gr, gr_unittest

try:
    from gnuradio import nut
except ImportError:
    import os
    import sys

    dirname, filename = os.path.split(os.path.abspath(__file__))
    sys.path.append(os.path.join(dirname, "bindings"))
    from gnuradio import nut

FFMPEG = shutil.which("ffmpeg")
FFPROBE = shutil.which("ffprobe")

RATE = 48000


def sine(n, freq=997.0, amp=0.5, rate=RATE):
    return (amp * np.sin(2 * np.pi * freq * np.arange(n) / rate)).astype(np.float32)


def make_frames(nframes, width, height):
    """Synthetic rgb24 frames with a distinct per-frame signature."""
    n = width * height * 3
    frames = np.empty((nframes, n), dtype=np.uint8)
    for k in range(nframes):
        frames[k] = (np.arange(n, dtype=np.int64) * 3 + k * 17) % 256
    return frames


def run_with_timeout(tb, timeout):
    """tb.run() guarded by a watchdog; returns True if it completed."""
    done = threading.Event()

    def body():
        tb.run()
        done.set()

    t = threading.Thread(target=body, daemon=True)
    t.start()
    if not done.wait(timeout):
        tb.stop()
        t.join(10)
        return False
    t.join(10)
    return True


def ffprobe_streams(path):
    """Stream dicts (codec, rate, geometry, frame rate) via ffprobe."""
    out = subprocess.run(
        [FFPROBE, "-v", "error", "-show_streams", "-of", "json", path],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    return json.loads(out)["streams"]


def ffprobe_video_packet_durations(path):
    """Per-packet durations of the first video stream ('N/A' if unset)."""
    out = subprocess.run(
        # fmt: off
        [FFPROBE, "-v", "error", "-select_streams", "v:0",
         "-show_entries", "packet=duration", "-of", "csv=p=0", path],
        # fmt: on
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    return [l for l in out.splitlines() if l.strip()]


def read_back(uri, audio_channels, video_streams, timeout=60):
    """File -> trusted nut_source -> vector sinks. Returns (src, [data])."""
    src = nut.nut_source(uri, audio_channels, video_streams)
    sinks = []
    tb = gr.top_block()
    for ch in range(audio_channels):
        s = blocks.vector_sink_f()
        tb.connect((src, ch), s)
        sinks.append(s)
    for v in range(video_streams):
        s = blocks.vector_sink_b()
        tb.connect((src, audio_channels + v), s)
        sinks.append(s)
    if not run_with_timeout(tb, timeout):
        raise AssertionError("read-back flowgraph did not finish")
    return src, sinks


def procs_matching(token):
    """ps lines of any process whose command line contains token."""
    out = subprocess.run(
        ["ps", "-eo", "pid=,stat=,args="], capture_output=True, text=True
    ).stdout
    return [l for l in out.splitlines() if token in l and "ps -eo" not in l]


def zombie_children():
    out = subprocess.run(
        ["ps", "--ppid", str(os.getpid()), "-o", "pid=,stat=,comm="],
        capture_output=True,
        text=True,
    ).stdout
    return [l for l in out.splitlines() if l.split()[1].startswith("Z")]


class qa_nut_sink(gr_unittest.TestCase):
    def setUp(self):
        self._tmpdir = tempfile.TemporaryDirectory(prefix="qa_nut_sink_")
        self.tmp = self._tmpdir.name

    def tearDown(self):
        self._tmpdir.cleanup()

    # ---- construction (no ffmpeg needed) ------------------------------

    def test_000_ctor_validation(self):
        # No inputs at all
        with self.assertRaises(ValueError):
            nut.nut_sink("x.nut", 0, 0, 0)
        # Negative channel count
        with self.assertRaises(ValueError):
            nut.nut_sink("x.nut", -1, RATE, 0)
        # Neither uri nor command
        with self.assertRaises(ValueError):
            nut.nut_sink("", 1, RATE, 0)
        # Both uri and command
        with self.assertRaises(ValueError):
            nut.nut_sink("x.nut", 1, RATE, 0, command="ffmpeg ...")
        # Audio declared but no rate: the sink WRITES the headers, so the
        # rate must be declared (the mirror of the source's adoption).
        with self.assertRaises(ValueError):
            nut.nut_sink("x.nut", 1, 0, 0)
        # Negative flush timeout
        with self.assertRaises(ValueError):
            nut.nut_sink("x.nut", 1, RATE, 0, flush_timeout=-1.0)

    def test_000a_ctor_broadcast_rule(self):
        # Length video_streams: OK
        nut.nut_sink("x.nut", 0, 0, 2, [64, 32], [48, 24], ["25", "30"])
        # Length 1: replicated to every stream
        nut.nut_sink("x.nut", 0, 0, 3, [64], [48], ["25"])
        # Anything else is a ctor error
        with self.assertRaisesRegex(ValueError, r"widths"):
            nut.nut_sink("x.nut", 0, 0, 3, [64, 32], [48], ["25"])
        with self.assertRaisesRegex(ValueError, r"heights"):
            nut.nut_sink("x.nut", 0, 0, 2, [64], [48, 24, 12], ["25"])
        with self.assertRaisesRegex(ValueError, r"fps"):
            nut.nut_sink("x.nut", 0, 0, 2, [64], [48], ["25", "30", "50"])
        # Geometry vectors without any video stream
        with self.assertRaises(ValueError):
            nut.nut_sink("x.nut", 1, RATE, 0, [64], [48], ["25"])
        # Invalid geometry
        with self.assertRaises(ValueError):
            nut.nut_sink("x.nut", 0, 0, 1, [0], [48], ["25"])
        with self.assertRaises(ValueError):
            nut.nut_sink("x.nut", 0, 0, 1, [64], [-1], ["25"])

    def test_000b_fps_parser(self):
        # Accepted spellings: integer or exact rational, as strings — and,
        # through the str() coercion in the Python wrapper, bare ints and
        # fractions.Fraction too.
        for f in ["25", "30000/1001", "24000/1001", 25, Fraction(30000, 1001)]:
            nut.nut_sink("x.nut", 0, 0, 1, [16], [16], [f])
        # Decimals are rejected with the exact-rational alternative named:
        # 29.97 is not exact; use 30000/1001. str(29.97) == "29.97", so
        # the policy survives the wrapper's coercion.
        with self.assertRaisesRegex(ValueError, r"30000/1001"):
            nut.nut_sink("x.nut", 0, 0, 1, [16], [16], ["29.97"])
        with self.assertRaisesRegex(ValueError, r"30000/1001"):
            nut.nut_sink("x.nut", 0, 0, 1, [16], [16], [29.97])
        with self.assertRaisesRegex(ValueError, r"24000/1001"):
            nut.nut_sink("x.nut", 0, 0, 1, [16], [16], ["23.976"])
        # Other rejections: zero denominator, zero rate, junk
        for bad in ["25/0", "0", "0/25", "25.0", "abc", "-25", "25/", "/25",
                    "25/30/40", ""]:
            with self.assertRaises(ValueError):
                nut.nut_sink("x.nut", 0, 0, 1, [16], [16], [bad])

    # ---- round trip through the trusted nut_source --------------------

    @unittest.skipUnless(FFPROBE, "ffprobe CLI not found")
    def test_001_roundtrip_audio_mono(self):
        ref = sine(RATE)  # 1 s
        out = os.path.join(self.tmp, "a.nut")
        src = blocks.vector_source_f(ref.tolist(), False)
        snk = nut.nut_sink(out, 1, RATE, 0)
        tb = gr.top_block()
        tb.connect(src, snk)
        tb.run()
        self.assertEqual(snk.last_error(), "")

        # Header checks: the declared format must be what was written.
        (st,) = ffprobe_streams(out)
        self.assertEqual(st["codec_name"], "pcm_f32le")
        self.assertEqual(int(st["sample_rate"]), RATE)
        self.assertEqual(int(st["channels"]), 1)

        # Byte-exact round trip; the adopted rate must match the declared.
        rsrc, (data,) = read_back(out, 1, 0)
        got = np.array(data.data(), dtype=np.float32)
        self.assertEqual(len(got), len(ref))
        self.assertTrue(np.array_equal(got, ref), "audio must be sample-exact")
        self.assertEqual(rsrc.audio_rate(), RATE, "declared rate adopted back")
        self.assertEqual(rsrc.last_error(), "")

    # NOTE on multi-input test sizes: GR declares a sink done as soon as
    # its FIRST input is exhausted; the block salvages whatever the other
    # upstreams already delivered, but an upstream source still blocked on
    # a full buffer at that moment quits early (scheduler semantics, not
    # the block's). For byte-exact assertions every stream's full payload
    # must therefore fit in its default input buffer (~16k float items /
    # ~64k byte items), so the sources finish delivering before any stream
    # ends. Real recorders end their streams together instead.

    @unittest.skipUnless(FFPROBE, "ffprobe CLI not found")
    def test_002_roundtrip_audio_stereo_interleave(self):
        n = 12000  # fits the default input buffer (see NOTE above)
        left = sine(n, freq=440.0, amp=0.4)
        right = np.linspace(-0.9, 0.9, n, dtype=np.float32)  # distinct content
        out = os.path.join(self.tmp, "st.nut")
        src_l = blocks.vector_source_f(left.tolist(), False)
        src_r = blocks.vector_source_f(right.tolist(), False)
        snk = nut.nut_sink(out, 2, RATE, 0)
        tb = gr.top_block()
        tb.connect(src_l, (snk, 0))
        tb.connect(src_r, (snk, 1))
        tb.run()
        self.assertEqual(snk.last_error(), "")

        (st,) = ffprobe_streams(out)
        self.assertEqual(int(st["channels"]), 2)

        _, (dl, dr) = read_back(out, 2, 0)
        self.assertTrue(
            np.array_equal(np.array(dl.data(), dtype=np.float32), left),
            "L must round-trip sample-exact through the interleave",
        )
        self.assertTrue(
            np.array_equal(np.array(dr.data(), dtype=np.float32), right),
            "R must round-trip sample-exact through the interleave",
        )

    @unittest.skipUnless(FFPROBE, "ffprobe CLI not found")
    def test_003_roundtrip_video(self):
        w, h, nframes = 32, 24, 10
        frames = make_frames(nframes, w, h)
        out = os.path.join(self.tmp, "v.nut")
        src = blocks.vector_source_b(frames.reshape(-1).tolist(), False)
        snk = nut.nut_sink(out, 0, 0, 1, [w], [h], ["25"])
        tb = gr.top_block()
        tb.connect(src, snk)
        tb.run()
        self.assertEqual(snk.last_error(), "")

        (st,) = ffprobe_streams(out)
        self.assertEqual(st["codec_name"], "rawvideo")
        self.assertEqual(int(st["width"]), w)
        self.assertEqual(int(st["height"]), h)
        self.assertEqual(st["avg_frame_rate"], "25/1")

        # THE avg_frame_rate regression check (design doc §7): without
        # avg_frame_rate set before write_header, NUT packets demux with
        # no duration and every downstream consumer has to guess.
        durs = ffprobe_video_packet_durations(out)
        self.assertEqual(len(durs), nframes)
        self.assertNotIn("N/A", durs, "every video packet must carry a duration")
        self.assertEqual(len(set(durs)), 1, "CFR: constant packet duration")

        rsrc, (data,) = read_back(out, 0, 1)
        got = np.array(data.data(), dtype=np.uint8)
        self.assertTrue(
            np.array_equal(got, frames.reshape(-1)), "video must be byte-exact"
        )
        self.assertEqual(rsrc.video_width(), w, "declared width adopted back")
        self.assertEqual(rsrc.video_height(), h, "declared height adopted back")

    @unittest.skipUnless(FFPROBE, "ffprobe CLI not found")
    def test_004_roundtrip_av(self):
        # 0.25 s of audio + 6 frames at 24 fps: the timelines match, so
        # the source-side pts trim must drop nothing. Payloads sized to
        # fit the input buffers (see NOTE above).
        w, h, fps = 32, 24, 24
        ref = sine(12000)
        frames = make_frames(6, w, h)
        out = os.path.join(self.tmp, "av.nut")
        src_a = blocks.vector_source_f(ref.tolist(), False)
        src_v = blocks.vector_source_b(frames.reshape(-1).tolist(), False)
        snk = nut.nut_sink(out, 1, RATE, 1, [w], [h], [str(fps)])
        tb = gr.top_block()
        tb.connect(src_a, (snk, 0))
        tb.connect(src_v, (snk, 1))
        tb.run()
        self.assertEqual(snk.last_error(), "")

        sts = ffprobe_streams(out)
        self.assertEqual(len(sts), 2)
        kinds = {s["codec_type"]: s for s in sts}
        self.assertEqual(kinds["audio"]["codec_name"], "pcm_f32le")
        self.assertEqual(kinds["video"]["codec_name"], "rawvideo")
        self.assertNotIn("N/A", ffprobe_video_packet_durations(out))

        _, (da, dv) = read_back(out, 1, 1)
        self.assertTrue(
            np.array_equal(np.array(da.data(), dtype=np.float32), ref),
            "audio must be sample-exact, nothing trimmed",
        )
        self.assertTrue(
            np.array_equal(np.array(dv.data(), dtype=np.uint8), frames.reshape(-1)),
            "video must be byte-exact, nothing trimmed",
        )

    @unittest.skipUnless(FFPROBE, "ffprobe CLI not found")
    def test_005_roundtrip_two_video_streams(self):
        # Two streams with different geometry; fps as a length-1 list
        # (broadcast). Each must round-trip byte-exact on its own port.
        (w0, h0), (w1, h1), nframes = (64, 48), (32, 24), 6
        f0 = make_frames(nframes, w0, h0)
        f1 = (make_frames(nframes, w1, h1).astype(np.int64) + 91).astype(np.uint8)
        out = os.path.join(self.tmp, "mv.nut")
        src0 = blocks.vector_source_b(f0.reshape(-1).tolist(), False)
        src1 = blocks.vector_source_b(f1.reshape(-1).tolist(), False)
        snk = nut.nut_sink(out, 0, 0, 2, [w0, w1], [h0, h1], ["25"])
        tb = gr.top_block()
        tb.connect(src0, (snk, 0))
        tb.connect(src1, (snk, 1))
        tb.run()
        self.assertEqual(snk.last_error(), "")

        sts = [s for s in ffprobe_streams(out) if s["codec_type"] == "video"]
        self.assertEqual(
            [(int(s["width"]), int(s["height"])) for s in sts],
            [(w0, h0), (w1, h1)],
            "streams must be written in port order with per-stream geometry",
        )

        rsrc, (d0, d1) = read_back(out, 0, 2)
        self.assertTrue(np.array_equal(np.array(d0.data(), np.uint8), f0.reshape(-1)))
        self.assertTrue(np.array_equal(np.array(d1.data(), np.uint8), f1.reshape(-1)))
        self.assertEqual((rsrc.video_width(0), rsrc.video_height(0)), (w0, h0))
        self.assertEqual((rsrc.video_width(1), rsrc.video_height(1)), (w1, h1))

    @unittest.skipUnless(FFPROBE, "ffprobe CLI not found")
    def test_006_fractional_fps(self):
        # NTSC-family rate spelled exactly; the file must carry 30000/1001
        # and the packets a constant duration.
        w, h, nframes = 32, 24, 12
        frames = make_frames(nframes, w, h)
        out = os.path.join(self.tmp, "ntsc.nut")
        src = blocks.vector_source_b(frames.reshape(-1).tolist(), False)
        snk = nut.nut_sink(out, 0, 0, 1, [w], [h], [Fraction(30000, 1001)])
        tb = gr.top_block()
        tb.connect(src, snk)
        tb.run()
        self.assertEqual(snk.last_error(), "")
        (st,) = ffprobe_streams(out)
        self.assertEqual(st["avg_frame_rate"], "30000/1001")
        durs = ffprobe_video_packet_durations(out)
        self.assertNotIn("N/A", durs)
        _, (data,) = read_back(out, 0, 1)
        self.assertTrue(
            np.array_equal(np.array(data.data(), np.uint8), frames.reshape(-1))
        )

    def test_007_partial_trailing_frame_dropped(self):
        # An input byte count that is not a multiple of the frame size:
        # the complete frames are muxed, the trailing fragment is staged
        # and (with a warning) dropped at shutdown — never written.
        w, h, nframes = 16, 16, 4
        fsz = w * h * 3
        frames = make_frames(nframes, w, h)
        payload = np.concatenate([frames.reshape(-1), frames[0][: fsz // 2]])
        out = os.path.join(self.tmp, "part.nut")
        src = blocks.vector_source_b(payload.tolist(), False)
        snk = nut.nut_sink(out, 0, 0, 1, [w], [h], ["25"])
        tb = gr.top_block()
        tb.connect(src, snk)
        tb.run()
        self.assertEqual(snk.last_error(), "")
        _, (data,) = read_back(out, 0, 1)
        got = np.array(data.data(), dtype=np.uint8)
        self.assertEqual(len(got), nframes * fsz, "only complete frames written")
        self.assertTrue(np.array_equal(got, frames.reshape(-1)))

    # ---- skewed production / interleave boundedness --------------------

    @unittest.skipUnless(FFPROBE, "ffprobe CLI not found")
    def test_008_av_skew_no_deadlock(self):
        # Audio far ahead of video: 5 s of audio vs 0.2 s of video. The
        # greedy consumer + the muxer's interleave queue (bounded by
        # max_interleave_delta, force-flushing beyond it) must absorb the
        # skew without ever blocking — the §7-verified behavior. The
        # result is a badly interleaved but valid file. The audio payload
        # deliberately does NOT fit the input buffer, so the recording
        # ends at the shorter (video) stream and the audio is a prefix
        # (see NOTE above) — what must hold is: no deadlock, a valid
        # file, complete video, and prefix-exact audio.
        w, h, fps = 32, 24, 25
        ref = sine(5 * RATE)
        frames = make_frames(5, w, h)
        out = os.path.join(self.tmp, "skew.nut")
        src_a = blocks.vector_source_f(ref.tolist(), False)
        src_v = blocks.vector_source_b(frames.reshape(-1).tolist(), False)
        snk = nut.nut_sink(out, 1, RATE, 1, [w], [h], [fps])
        tb = gr.top_block()
        tb.connect(src_a, (snk, 0))
        tb.connect(src_v, (snk, 1))
        self.assertTrue(
            run_with_timeout(tb, 60), "skewed A/V production deadlocked"
        )
        self.assertEqual(snk.last_error(), "")
        _, (da, dv) = read_back(out, 1, 1)
        got_a = np.array(da.data(), dtype=np.float32)
        self.assertGreaterEqual(
            len(got_a), int(0.2 * RATE), "at least the co-timed audio survives"
        )
        self.assertTrue(
            np.array_equal(got_a, ref[: len(got_a)]),
            "the recorded audio must be a sample-exact prefix",
        )
        self.assertTrue(
            np.array_equal(np.array(dv.data(), np.uint8), frames.reshape(-1)),
            "the shorter (video) stream must be complete",
        )

    # ---- spawn mode (POSIX-only; command run via /bin/sh -c) ----------

    @unittest.skipUnless(FFMPEG, "ffmpeg CLI not found")
    def test_009_spawn_happy_path(self):
        # The spawned command copies NUT-from-stdin to a file; the copy
        # must round-trip byte-exact — proving the child got the whole
        # stream, correctly finalized (flush -> trailer -> EOF -> child
        # exits on its own).
        ref = sine(RATE)
        out = os.path.join(self.tmp, "spawn.nut")
        cmd = (
            "ffmpeg -y -loglevel error -i pipe:0 -c:a pcm_f32le "
            "-max_interleave_delta 500000 -f nut %s" % out
        )
        src = blocks.vector_source_f(ref.tolist(), False)
        snk = nut.nut_sink("", 1, RATE, 0, command=cmd)
        tb = gr.top_block()
        tb.connect(src, snk)
        self.assertTrue(run_with_timeout(tb, 60), "spawn run did not finish")
        self.assertEqual(snk.last_error(), "")
        self.assertEqual(zombie_children(), [], "spawned command not reaped")
        _, (data,) = read_back(out, 1, 0)
        self.assertTrue(
            np.array_equal(np.array(data.data(), dtype=np.float32), ref),
            "spawn-mode output must round-trip sample-exact",
        )

    @unittest.skipUnless(FFMPEG, "ffmpeg CLI not found")
    def test_010_stop_mid_stream_flush(self):
        # Record an endless source to a spawned ffmpeg flac encode, stop
        # mid-stream: the shutdown ordering (flush -> trailer -> close ->
        # WAIT for the child) must leave a complete, decodable file — the
        # opposite of the source's kill-on-stop.
        flac = os.path.join(self.tmp, "rec.flac")
        cmd = "ffmpeg -y -loglevel error -i pipe:0 %s" % flac
        src = blocks.vector_source_f(sine(RATE).tolist(), True)  # endless
        snk = nut.nut_sink("", 1, RATE, 0, command=cmd)
        tb = gr.top_block()
        tb.connect(src, snk)
        tb.start()
        time.sleep(1.0)
        tb.stop()
        tb.wait()
        self.assertEqual(snk.last_error(), "", "a clean stop is not an error")
        self.assertEqual(zombie_children(), [], "spawned command not reaped")
        # The file must be complete: full decode succeeds and reports a
        # nonzero duration (a killed encoder leaves a broken file).
        (st,) = ffprobe_streams(flac)
        self.assertEqual(st["codec_name"], "flac")
        self.assertGreater(float(st["duration"]), 0.0)
        subprocess.run(
            [FFMPEG, "-v", "error", "-i", flac, "-f", "null", "-"],
            check=True,
            stdin=subprocess.DEVNULL,
        )

    def test_011_child_death_is_error(self):
        # The reader exits after 64 kB while the producer is endless:
        # EPIPE on write is an ERROR (the recording is lost), not an EOF —
        # fail() latch, ERROR log, WORK_DONE, last_error() non-empty, and
        # the flowgraph terminates on its own.
        src = blocks.vector_source_f(sine(RATE).tolist(), True)  # endless
        snk = nut.nut_sink(
            "", 1, RATE, 0, command="head -c 65536 > /dev/null"
        )
        tb = gr.top_block()
        tb.connect(src, snk)
        t0 = time.monotonic()
        self.assertTrue(
            run_with_timeout(tb, 30),
            "flowgraph must terminate on its own when the reader dies",
        )
        self.assertLess(time.monotonic() - t0, 20.0)
        err = snk.last_error()
        self.assertNotEqual(err, "", "EPIPE must latch an error")
        self.assertRegex(err, r"EPIPE|reader")

    def test_012_spawn_failing_command(self):
        # A command that exits immediately without reading stdin: the
        # header/packet writes hit EPIPE and the flowgraph terminates
        # promptly with a non-empty last_error().
        src = blocks.vector_source_f(sine(RATE).tolist(), True)
        snk = nut.nut_sink("", 1, RATE, 0, command="exec false")
        tb = gr.top_block()
        tb.connect(src, snk)
        self.assertTrue(
            run_with_timeout(tb, 30), "failing spawn must not hang the flowgraph"
        )
        self.assertNotEqual(snk.last_error(), "")
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline and zombie_children():
            time.sleep(0.2)
        self.assertEqual(zombie_children(), [], "failed command not reaped")

    def test_013_flush_timeout_escalation(self):
        # A reader that consumes everything but refuses to exit at EOF:
        # after flush_timeout the block must escalate (SIGTERM the process
        # group) instead of waiting forever, and the whole pipeline must
        # be gone afterwards.
        token = "qa_nut_sink_stubborn_reader"
        cmd = "cat > /dev/null; sleep 600 # %s" % token
        src = blocks.vector_source_f(sine(RATE // 4).tolist(), False)
        snk = nut.nut_sink("", 1, RATE, 0, command=cmd, flush_timeout=1.0)
        tb = gr.top_block()
        tb.connect(src, snk)
        t0 = time.monotonic()
        self.assertTrue(
            run_with_timeout(tb, 30), "escalation did not unblock the shutdown"
        )
        self.assertLess(
            time.monotonic() - t0, 10.0, "shutdown must be bounded by the timeout"
        )
        self.assertEqual(snk.last_error(), "")
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline and (
            procs_matching(token) or zombie_children()
        ):
            time.sleep(0.2)
        self.assertEqual(
            procs_matching(token), [], "reader pipeline survived the escalation"
        )
        self.assertEqual(zombie_children(), [], "zombie child left behind")

    # ---- external (uri) mode ------------------------------------------

    @unittest.skipUnless(FFMPEG, "ffmpeg CLI not found")
    def test_014_external_fifo(self):
        # External mode over a FIFO: an externally started ffmpeg reads
        # the FIFO (the ops-variant plumbing); closing the FIFO at stop is
        # its EOF. The copy must round-trip byte-exact.
        fifo = os.path.join(self.tmp, "sink.fifo")
        out = os.path.join(self.tmp, "fifo_copy.nut")
        os.mkfifo(fifo)
        ref = sine(RATE // 2)
        reader = subprocess.Popen(
            # fmt: off
            [FFMPEG, "-hide_banner", "-loglevel", "error", "-y",
             "-i", fifo, "-c:a", "pcm_f32le",
             "-max_interleave_delta", "500000", "-f", "nut", out],
            # fmt: on
            stdin=subprocess.DEVNULL,
        )
        try:
            src = blocks.vector_source_f(ref.tolist(), False)
            snk = nut.nut_sink(fifo, 1, RATE, 0)
            tb = gr.top_block()
            tb.connect(src, snk)
            self.assertTrue(run_with_timeout(tb, 60), "FIFO run did not finish")
            self.assertEqual(snk.last_error(), "")
            self.assertEqual(reader.wait(timeout=30), 0)
        finally:
            if reader.poll() is None:
                reader.kill()
                reader.wait()
        _, (data,) = read_back(out, 1, 0)
        self.assertTrue(
            np.array_equal(np.array(data.data(), dtype=np.float32), ref),
            "external-mode output must round-trip sample-exact",
        )

    def test_015_external_unwritable_uri(self):
        src = blocks.vector_source_f(sine(1024).tolist(), False)
        snk = nut.nut_sink("/nonexistent/no_such_dir/x.nut", 1, RATE, 0)
        tb = gr.top_block()
        tb.connect(src, snk)
        t0 = time.monotonic()
        self.assertTrue(
            run_with_timeout(tb, 15),
            "failing flowgraph must terminate on its own, not hang",
        )
        self.assertLess(time.monotonic() - t0, 10.0)
        err = snk.last_error()
        self.assertNotEqual(err, "")
        self.assertRegex(err, r"cannot open.*x\.nut")

    # ---- overflow-tag vigilance ----------------------------------------

    def test_016_rx_time_tag_midstream(self):
        # An rx_time tag beyond offset 0 signals an upstream overflow (the
        # synthesized timeline silently compresses); the block WARNs but
        # must keep running and stay byte-exact. (The warning text goes to
        # the logger; this test exercises the path and the data promise.)
        ref = sine(RATE // 4)
        tag = gr.tag_t()
        tag.offset = RATE // 8
        tag.key = pmt.intern("rx_time")
        tag.value = pmt.make_tuple(pmt.from_uint64(123), pmt.from_double(0.5))
        out = os.path.join(self.tmp, "tagged.nut")
        src = blocks.vector_source_f(ref.tolist(), False, 1, [tag])
        snk = nut.nut_sink(out, 1, RATE, 0)
        tb = gr.top_block()
        tb.connect(src, snk)
        tb.run()
        self.assertEqual(snk.last_error(), "")
        _, (data,) = read_back(out, 1, 0)
        self.assertTrue(
            np.array_equal(np.array(data.data(), dtype=np.float32), ref),
            "the warning must not disturb the data path",
        )


if __name__ == "__main__":
    gr_unittest.run(qa_nut_sink)
