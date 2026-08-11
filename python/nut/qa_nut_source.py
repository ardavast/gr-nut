#!/usr/bin/env python
# -*- coding: utf-8 -*-
#
# Copyright 2026 Ardavast Dayleryan.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

"""QA for nut::nut_source (design doc §6, items 1-7).

Every test muxes its fixture with the ffmpeg CLI at test time and is
skipped cleanly if ffmpeg is not installed.
"""

import os
import shlex
import shutil
import subprocess
import tempfile
import threading
import time
import unittest

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

RATE = 48000


def run_ffmpeg(args):
    subprocess.run(
        [FFMPEG, "-hide_banner", "-loglevel", "error", "-y"] + args,
        check=True,
        stdin=subprocess.DEVNULL,
    )


def sine(n, freq=997.0, amp=0.5, rate=RATE):
    return (amp * np.sin(2 * np.pi * freq * np.arange(n) / rate)).astype(np.float32)


def make_frames(nframes, width, height):
    """Synthetic rgb24 frames with a distinct per-frame signature."""
    n = width * height * 3
    frames = np.empty((nframes, n), dtype=np.uint8)
    for k in range(nframes):
        frames[k] = (np.arange(n, dtype=np.int64) * 3 + k * 17) % 256
    return frames


def mux_audio(tmp, samples, channels, rate=RATE, codec="pcm_f32le", name="a.nut"):
    """Mux interleaved float32 samples into an audio-only NUT file."""
    raw = os.path.join(tmp, "a.f32")
    samples.astype(np.float32).tofile(raw)
    out = os.path.join(tmp, name)
    run_ffmpeg(
        # fmt: off
        ["-f", "f32le", "-ar", str(rate), "-ac", str(channels), "-i", raw,
         "-c:a", codec, "-max_interleave_delta", "500000", "-f", "nut", out]
        # fmt: on
    )
    return out


def mux_av(
    tmp,
    samples,
    frames,
    width,
    height,
    fps=25,
    rate=RATE,
    audio_offset=None,
    video_offset=None,
    interleave_delta="500000",
    fps_mode="cfr",
    name="av.nut",
):
    """Mux mono float32 audio + rgb24 frames into an A/V NUT file."""
    araw = os.path.join(tmp, "a.f32")
    samples.astype(np.float32).tofile(araw)
    vraw = os.path.join(tmp, "v.rgb")
    frames.astype(np.uint8).tofile(vraw)
    out = os.path.join(tmp, name)
    cmd = []
    if video_offset is not None:
        cmd += ["-itsoffset", str(video_offset)]
    # fmt: off
    cmd += ["-f", "rawvideo", "-pix_fmt", "rgb24",
            "-video_size", "%dx%d" % (width, height), "-framerate", str(fps),
            "-i", vraw]
    if audio_offset is not None:
        cmd += ["-itsoffset", str(audio_offset)]
    cmd += ["-f", "f32le", "-ar", str(rate), "-ac", "1", "-i", araw,
            "-map", "0:v", "-map", "1:a",
            "-c:v", "rawvideo", "-pix_fmt", "rgb24", "-c:a", "pcm_f32le",
            "-fps_mode", fps_mode, "-max_interleave_delta", interleave_delta,
            "-f", "nut", out]
    # fmt: on
    run_ffmpeg(cmd)
    return out


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


def make_wav(tmp, name, seconds, rate=44100):
    """A stereo s16 wav fixture for spawn-mode tests (decode+conform work)."""
    path = os.path.join(tmp, name)
    run_ffmpeg(
        # fmt: off
        ["-f", "lavfi", "-i", "sine=frequency=440:duration=%s" % seconds,
         "-af", "aeval=val(0)|-val(0)", "-ar", str(rate), "-ac", "2", path]
        # fmt: on
    )
    return path


def spawn_cmd_audio(path, channels=1, rate=RATE):
    """The §3 reference audio command, stdout variant, for spawn mode."""
    return (
        "ffmpeg -loglevel warning -i %s -vn -af aresample=%d:async=1 -ac %d "
        "-c:a pcm_f32le -max_interleave_delta 500000 -f nut pipe:1"
        % (shlex.quote(path), rate, channels)
    )


def decode_reference(tmp, path, channels=1, rate=RATE):
    """Direct ffmpeg decode of the fixture with the same conform chain."""
    ref = os.path.join(tmp, "spawn_ref.f32")
    run_ffmpeg(
        # fmt: off
        ["-i", path, "-vn", "-af", "aresample=%d:async=1" % rate,
         "-ac", str(channels), "-f", "f32le", ref]
        # fmt: on
    )
    return np.fromfile(ref, dtype=np.float32)


def procs_matching(token):
    """ps lines of any process whose command line contains token."""
    out = subprocess.run(
        ["ps", "-eo", "pid=,stat=,args="], capture_output=True, text=True
    ).stdout
    return [
        l for l in out.splitlines() if token in l and "ps -eo" not in l
    ]


def zombie_children():
    out = subprocess.run(
        ["ps", "--ppid", str(os.getpid()), "-o", "pid=,stat=,comm="],
        capture_output=True,
        text=True,
    ).stdout
    return [l for l in out.splitlines() if l.split()[1].startswith("Z")]


def tags_by_offset(tags):
    out = {}
    for t in tags:
        out.setdefault(t.offset, {})[pmt.symbol_to_string(t.key)] = pmt.to_python(
            t.value
        )
    return out


class qa_nut_source(gr_unittest.TestCase):
    def setUp(self):
        self._tmpdir = tempfile.TemporaryDirectory(prefix="qa_nut_")
        self.tmp = self._tmpdir.name

    def tearDown(self):
        self._tmpdir.cleanup()

    # ---- construction (no ffmpeg needed) ------------------------------

    def test_000_constructor_validation(self):
        # No outputs at all
        with self.assertRaises(ValueError):
            nut.nut_source("x.nut", 0, RATE, False, 0, 0)
        # Video without geometry
        with self.assertRaises(ValueError):
            nut.nut_source("x.nut", 1, RATE, True, 0, 0)
        # Negative channel count
        with self.assertRaises(ValueError):
            nut.nut_source("x.nut", -1, RATE, False, 0, 0)

    # ---- 1. audio exactness -------------------------------------------

    @unittest.skipUnless(FFMPEG, "ffmpeg CLI not found")
    def test_001_audio_mono_exact(self):
        ref = sine(RATE)  # 1 s
        uri = mux_audio(self.tmp, ref, 1)
        src = nut.nut_source(uri, 1, RATE, False, 0, 0)
        snk = blocks.vector_sink_f()
        tb = gr.top_block()
        tb.connect(src, snk)
        tb.run()
        out = np.array(snk.data(), dtype=np.float32)
        self.assertEqual(len(out), len(ref))
        self.assertTrue(np.array_equal(out, ref), "mono audio must be sample-exact")

    @unittest.skipUnless(FFMPEG, "ffmpeg CLI not found")
    def test_002_audio_stereo_deinterleave(self):
        n = RATE // 2
        left = sine(n, freq=440.0, amp=0.4)
        right = np.linspace(-0.9, 0.9, n, dtype=np.float32)  # distinct content
        inter = np.empty(2 * n, dtype=np.float32)
        inter[0::2] = left
        inter[1::2] = right
        uri = mux_audio(self.tmp, inter, 2)
        src = nut.nut_source(uri, 2, RATE, False, 0, 0)
        snk_l = blocks.vector_sink_f()
        snk_r = blocks.vector_sink_f()
        tb = gr.top_block()
        tb.connect((src, 0), snk_l)
        tb.connect((src, 1), snk_r)
        tb.run()
        out_l = np.array(snk_l.data(), dtype=np.float32)
        out_r = np.array(snk_r.data(), dtype=np.float32)
        self.assertTrue(np.array_equal(out_l, left), "L must be sample-exact")
        self.assertTrue(np.array_equal(out_r, right), "R must be sample-exact")

    # ---- 2. video byte-exactness + tags -------------------------------

    @unittest.skipUnless(FFMPEG, "ffmpeg CLI not found")
    def test_003_video_bytes_and_tags(self):
        w, h, nframes = 32, 24, 10
        fsz = w * h * 3
        frames = make_frames(nframes, w, h)
        vraw = os.path.join(self.tmp, "v.rgb")
        frames.tofile(vraw)
        uri = os.path.join(self.tmp, "v.nut")
        # fmt: off
        run_ffmpeg(["-f", "rawvideo", "-pix_fmt", "rgb24",
                    "-video_size", "%dx%d" % (w, h), "-framerate", "25",
                    "-i", vraw,
                    "-c:v", "rawvideo", "-pix_fmt", "rgb24",
                    "-max_interleave_delta", "500000", "-f", "nut", uri])
        # fmt: on
        src = nut.nut_source(uri, 0, 0, True, w, h)
        snk = blocks.vector_sink_b()
        tb = gr.top_block()
        tb.connect(src, snk)
        tb.run()
        out = np.array(snk.data(), dtype=np.uint8)
        self.assertEqual(len(out), nframes * fsz)
        self.assertTrue(
            np.array_equal(out, frames.reshape(-1)), "video must be byte-exact"
        )
        tags = tags_by_offset(snk.tags())
        self.assertEqual(len(tags), nframes, "exactly one tag set per frame")
        self.assertEqual(sorted(tags.keys()), [k * fsz for k in range(nframes)])
        pts = []
        for off in sorted(tags.keys()):
            t = tags[off]
            self.assertEqual(set(t.keys()), {"pts", "width", "height"})
            self.assertEqual(t["width"], w)
            self.assertEqual(t["height"], h)
            pts.append(t["pts"])
        deltas = np.diff(pts)
        self.assertTrue((deltas > 0).all(), "pts must be strictly increasing")
        self.assertEqual(len(set(deltas)), 1, "CFR: pts spacing must be constant")

    # ---- 3. initial pts trim ------------------------------------------

    @unittest.skipUnless(FFMPEG, "ffmpeg CLI not found")
    def test_004_pts_trim_video_leads(self):
        # Audio delayed 0.2 s => the first 5 video frames (25 fps) are
        # dropped so both ports start at the same media time.
        w, h, fps, nframes = 32, 24, 25, 25
        fsz = w * h * 3
        ref = sine(RATE)
        frames = make_frames(nframes, w, h)
        uri = mux_av(self.tmp, ref, frames, w, h, fps=fps, audio_offset=0.2)
        src = nut.nut_source(uri, 1, RATE, True, w, h)
        snk_a = blocks.vector_sink_f()
        snk_v = blocks.vector_sink_b()
        tb = gr.top_block()
        tb.connect((src, 0), snk_a)
        tb.connect((src, 1), snk_v)
        tb.run()
        out_a = np.array(snk_a.data(), dtype=np.float32)
        out_v = np.array(snk_v.data(), dtype=np.uint8)
        self.assertTrue(np.array_equal(out_a, ref), "audio must be untouched")
        self.assertEqual(len(out_v) % fsz, 0)
        got = len(out_v) // fsz
        dropped = nframes - got
        self.assertAlmostEqual(dropped, 5, delta=1)
        self.assertTrue(
            np.array_equal(out_v, frames[dropped:].reshape(-1)),
            "kept frames must be the trailing ones, byte-exact",
        )

    @unittest.skipUnless(FFMPEG, "ffmpeg CLI not found")
    def test_005_pts_trim_audio_leads(self):
        # Video delayed 0.2 s => ~9600 leading audio samples are trimmed.
        w, h, fps, nframes = 32, 24, 25, 25
        fsz = w * h * 3
        ref = sine(RATE)
        frames = make_frames(nframes, w, h)
        # -fps_mode cfr would regenerate the video timestamps from zero and
        # absorb the offset by duplicating frames; passthrough keeps the
        # deliberate 0.2 s lead intact for the trim to remove.
        uri = mux_av(
            self.tmp, ref, frames, w, h, fps=fps, video_offset=0.2,
            fps_mode="passthrough",
        )
        src = nut.nut_source(uri, 1, RATE, True, w, h)
        snk_a = blocks.vector_sink_f()
        snk_v = blocks.vector_sink_b()
        tb = gr.top_block()
        tb.connect((src, 0), snk_a)
        tb.connect((src, 1), snk_v)
        tb.run()
        out_a = np.array(snk_a.data(), dtype=np.float32)
        out_v = np.array(snk_v.data(), dtype=np.uint8)
        self.assertEqual(len(out_v) // fsz, nframes, "video must be untouched")
        skip = len(ref) - len(out_a)
        self.assertAlmostEqual(skip, int(0.2 * RATE), delta=1)
        self.assertTrue(
            np.array_equal(out_a, ref[skip:]),
            "kept audio must be the trailing samples, sample-exact",
        )

    # ---- 4. contract validation errors --------------------------------

    @unittest.skipUnless(FFMPEG, "ffmpeg CLI not found")
    def test_006_validation_channel_count(self):
        inter = np.zeros(2 * 1024, dtype=np.float32)
        uri = mux_audio(self.tmp, inter, 2)  # stereo fixture
        src = nut.nut_source(uri, 1, RATE, False, 0, 0)  # declared mono
        with self.assertRaisesRegex(RuntimeError, r"channel.*-ac"):
            src.start()

    @unittest.skipUnless(FFMPEG, "ffmpeg CLI not found")
    def test_007_validation_sample_rate(self):
        uri = mux_audio(self.tmp, sine(4410, rate=44100), 1, rate=44100)
        src = nut.nut_source(uri, 1, RATE, False, 0, 0)
        with self.assertRaisesRegex(RuntimeError, r"48000 Hz.*44100 Hz"):
            src.start()

    @unittest.skipUnless(FFMPEG, "ffmpeg CLI not found")
    def test_008_validation_codec(self):
        uri = mux_audio(self.tmp, sine(1024), 1, codec="pcm_s16le")
        src = nut.nut_source(uri, 1, RATE, False, 0, 0)
        with self.assertRaisesRegex(RuntimeError, r"pcm_f32le.*pcm_s16le"):
            src.start()

    @unittest.skipUnless(FFMPEG, "ffmpeg CLI not found")
    def test_009_validation_geometry(self):
        w, h = 64, 48
        frames = make_frames(5, w, h)
        vraw = os.path.join(self.tmp, "v.rgb")
        frames.tofile(vraw)
        uri = os.path.join(self.tmp, "v.nut")
        # fmt: off
        run_ffmpeg(["-f", "rawvideo", "-pix_fmt", "rgb24",
                    "-video_size", "%dx%d" % (w, h), "-framerate", "25",
                    "-i", vraw, "-c:v", "rawvideo", "-pix_fmt", "rgb24",
                    "-max_interleave_delta", "500000", "-f", "nut", uri])
        # fmt: on
        src = nut.nut_source(uri, 0, 0, True, 32, 24)  # declared 32x24
        with self.assertRaisesRegex(RuntimeError, r"32x24.*64x48"):
            src.start()

    @unittest.skipUnless(FFMPEG, "ffmpeg CLI not found")
    def test_010_validation_stream_layout(self):
        uri = mux_audio(self.tmp, sine(1024), 1)  # audio-only fixture
        src = nut.nut_source(uri, 1, RATE, True, 32, 24)  # video declared
        with self.assertRaisesRegex(RuntimeError, r"stream layout"):
            src.start()

    # ---- 5. deadlock regression ---------------------------------------

    @unittest.skipUnless(FFMPEG, "ffmpeg CLI not found")
    def test_011_deadlock_synchronous_recombine(self):
        # Frame geometry chosen so the video byte rate equals the audio
        # sample rate (40*16*3*25 == 48000): the two streams can then be
        # recombined by a synchronous 1:1 interleaver, which is exactly the
        # §4.7 deadlock shape. Must complete with the block's default
        # min-output-buffer settings.
        w, h, fps = 40, 16, 25
        dur = 2
        ref = sine(dur * RATE)
        frames = make_frames(dur * fps, w, h)
        uri = mux_av(self.tmp, ref, frames, w, h, fps=fps)
        src = nut.nut_source(uri, 1, RATE, True, w, h)
        a2b = blocks.float_to_char(1, 127.0)
        comb = blocks.interleave(gr.sizeof_char, 1)
        snk = blocks.null_sink(gr.sizeof_char)
        tb = gr.top_block()
        tb.connect((src, 0), a2b, (comb, 0))
        tb.connect((src, 1), (comb, 1))
        tb.connect(comb, snk)
        self.assertTrue(
            run_with_timeout(tb, 60), "synchronous A/V recombine deadlocked"
        )

    # ---- 6. EOF -------------------------------------------------------

    @unittest.skipUnless(FFMPEG, "ffmpeg CLI not found")
    def test_012_eof_work_done(self):
        n = RATE // 4
        ref = sine(n)
        uri = mux_audio(self.tmp, ref, 1)
        src = nut.nut_source(uri, 1, RATE, False, 0, 0)
        snk = blocks.vector_sink_f()
        tb = gr.top_block()
        tb.connect(src, snk)
        self.assertTrue(run_with_timeout(tb, 30), "flowgraph did not finish at EOF")
        self.assertTrue(np.array_equal(np.array(snk.data(), dtype=np.float32), ref))

    # ---- 7. shutdown with a stalled writer ----------------------------

    @unittest.skipUnless(FFMPEG, "ffmpeg CLI not found")
    def test_014_stop_unblocks_stalled_fifo(self):
        # A writer that delivers the NUT headers plus some payload and then
        # stalls forever (fd stays open, so no EOF). The block parks in a
        # blocked pipe read inside the work loop; tb.stop() must still
        # return promptly (interrupt callback / stop flag works).
        #
        # Note: start()-time validation needs the stream headers, so a
        # writer that never produces even those would block tb.start()
        # itself — by contract, start ffmpeg before the flowgraph.
        nutfile = mux_audio(self.tmp, sine(RATE // 4), 1)
        fifo = os.path.join(self.tmp, "stalled.fifo")
        os.mkfifo(fifo)
        wfd = os.open(fifo, os.O_RDWR)  # keep a writer end open forever
        with open(nutfile, "rb") as f:
            os.write(wfd, f.read())  # headers + payload, then stall
        try:
            src = nut.nut_source(fifo, 1, RATE, False, 0, 0)
            snk = blocks.null_sink(gr.sizeof_float)
            tb = gr.top_block()
            tb.connect(src, snk)
            tb.start()
            time.sleep(1.0)  # let the block park in the blocked read
            t0 = time.monotonic()
            tb.stop()
            done = threading.Event()

            def waiter():
                tb.wait()
                done.set()

            t = threading.Thread(target=waiter, daemon=True)
            t.start()
            self.assertTrue(
                done.wait(10), "tb.wait() hung after stop() with a stalled writer"
            )
            elapsed = time.monotonic() - t0
            self.assertLess(elapsed, 5.0, "shutdown was not prompt: %.1f s" % elapsed)
        finally:
            os.close(wfd)


    # ---- spawn mode (POSIX-only; command run via /bin/sh -c) ----------

    def test_015_ctor_uri_command_exclusive(self):
        with self.assertRaises(ValueError):  # neither given
            nut.nut_source("", 1, RATE, False, 0, 0, command="")
        with self.assertRaises(ValueError):  # both given
            nut.nut_source(
                "x.nut", 1, RATE, False, 0, 0, command="ffmpeg ..."
            )

    @unittest.skipUnless(FFMPEG, "ffmpeg CLI not found")
    def test_016_spawn_audio_exact(self):
        # 44.1k stereo s16 fixture: the spawned command does real decode +
        # conform work; the block's output must equal a direct ffmpeg CLI
        # decode of the same fixture through the same filter chain.
        wav = make_wav(self.tmp, "spawn_fix.wav", seconds=2)
        ref = decode_reference(self.tmp, wav)
        src = nut.nut_source(
            "", 1, RATE, False, 0, 0, command=spawn_cmd_audio(wav)
        )
        snk = blocks.vector_sink_f()
        tb = gr.top_block()
        tb.connect(src, snk)
        self.assertTrue(run_with_timeout(tb, 60), "spawn run did not finish")
        out = np.array(snk.data(), dtype=np.float32)
        self.assertEqual(len(out), len(ref))
        self.assertTrue(
            np.array_equal(out, ref),
            "spawn-mode output must match direct ffmpeg decode sample-exactly",
        )
        self.assertEqual(zombie_children(), [], "spawned command not reaped")

    @unittest.skipUnless(FFMPEG, "ffmpeg CLI not found")
    def test_017_spawn_shutdown_pipeline(self):
        # The spawn command is a two-process shell pipeline (cat | ffmpeg).
        # A throttled consumer keeps both alive mid-stream via backpressure;
        # tb.stop() must return promptly and kill/reap the whole process
        # group — no survivors of either process, no zombies.
        token = "qa_nut_spawn_pipeline_fixture.wav"
        wav = make_wav(self.tmp, token, seconds=60)
        cmd = "cat %s | %s" % (shlex.quote(wav), spawn_cmd_audio("pipe:0"))
        src = nut.nut_source("", 1, RATE, False, 0, 0, command=cmd)
        thr = blocks.throttle(gr.sizeof_float, RATE)
        snk = blocks.null_sink(gr.sizeof_float)
        tb = gr.top_block()
        tb.connect(src, thr, snk)
        tb.start()
        time.sleep(1.5)
        self.assertTrue(procs_matching(token), "pipeline did not start")
        t0 = time.monotonic()
        tb.stop()
        done = threading.Event()

        def waiter():
            tb.wait()
            done.set()

        threading.Thread(target=waiter, daemon=True).start()
        self.assertTrue(done.wait(10), "tb.wait() hung after stop() in spawn mode")
        self.assertLess(time.monotonic() - t0, 5.0, "shutdown was not prompt")
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline and (
            procs_matching(token) or zombie_children()
        ):
            time.sleep(0.2)
        self.assertEqual(
            procs_matching(token), [], "pipeline processes survived stop()"
        )
        self.assertEqual(zombie_children(), [], "zombie child left behind")

    @unittest.skipUnless(FFMPEG, "ffmpeg CLI not found")
    def test_019_spawn_failing_command(self):
        # ffmpeg with a nonexistent input: exits nonzero, writes no NUT.
        # start() must surface it as a RuntimeError pointing at the command.
        src = nut.nut_source(
            "", 1, RATE, False, 0, 0,
            command=spawn_cmd_audio("/nonexistent/no_such_media.wav"),
        )
        with self.assertRaisesRegex(RuntimeError, r"spawn command"):
            src.start()
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline and zombie_children():
            time.sleep(0.2)
        self.assertEqual(zombie_children(), [], "failed command not reaped")

    @unittest.skipUnless(FFMPEG, "ffmpeg CLI not found")
    def test_020_spawn_contract_guardrail(self):
        # A command whose output does not match the declared params must
        # produce the usual actionable validation error, plus a pointer to
        # the spawn command.
        wav = make_wav(self.tmp, "spawn_guard.wav", seconds=0.2)
        src = nut.nut_source(  # command emits stereo, block declares mono
            "", 1, RATE, False, 0, 0,
            command=spawn_cmd_audio(wav, channels=2),
        )
        with self.assertRaisesRegex(
            RuntimeError, r"channel.*-ac.*check your spawn command"
        ):
            src.start()


if __name__ == "__main__":
    gr_unittest.run(qa_nut_source)
