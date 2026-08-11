/* -*- c++ -*- */
/*
 * Copyright 2026 Ardavast Dayleryan.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_NUT_NUT_SOURCE_H
#define INCLUDED_NUT_NUT_SOURCE_H

#include <gnuradio/block.h>
#include <gnuradio/nut/api.h>

namespace gr {
namespace nut {

/*!
 * \brief Source block that ingests a raw-payload NUT stream (from ffmpeg)
 *        and emits deinterleaved float audio streams and an optional
 *        rgb24 video byte stream.
 * \ingroup nut
 *
 * \details
 * gr-nut is the interface between ffmpeg and GNU Radio for analogue
 * transmission chains: ffmpeg is the codec/format/sync layer, GNU Radio is
 * the physical layer, and a NUT stream over a pipe/FIFO is the socket
 * between them. ffmpeg decodes anything, conforms it to a strict raw
 * profile, and performs all A/V sync adaptation; this block receives
 * sample-exact raw streams and never adapts rates — it asserts them.
 *
 * Error model: constructor-time misuse (both or neither of uri/command,
 * invalid parameter values) throws std::invalid_argument in the caller's
 * thread. Everything after construction — open/spawn failures, contract
 * validation, mid-stream violations — cannot throw usefully: GNU Radio
 * runs start() and work() inside the block's scheduler thread, whose
 * wrapper catches exceptions and would kill only that thread while the
 * rest of the flowgraph hangs. Such failures are therefore logged at
 * ERROR level with the same actionable messages, and the block terminates
 * the flowgraph cleanly (WORK_DONE). After tb.run()/tb.wait() returns,
 * last_error() distinguishes failure (non-empty message) from clean EOF
 * (empty).
 *
 * Interface contract (a strict profile of NUT):
 *  - container: NUT, streamed, read from a FIFO or file (\p uri);
 *  - audio: pcm_f32le, interleaved, exactly \p audio_channels channels at
 *    exactly \p audio_rate Hz;
 *  - video (only if \p emit_video): rawvideo, pix_fmt rgb24, exactly
 *    \p video_width x \p video_height, constant frame rate;
 *  - stream layout: exactly the declared set of streams.
 * Any mismatch is a hard error: start() throws std::runtime_error with an
 * actionable message (the fix is always on the ffmpeg command line).
 * Mid-stream violations (geometry change, new stream) are hard errors at
 * the point of detection.
 *
 * Reference ffmpeg command (audio-only, mono):
 * \code
 * ffmpeg -i INPUT -vn -af "aresample=48000:async=1" -ac 1 \
 *   -c:a pcm_f32le -max_interleave_delta 500000 -f nut pipe:1 > fifo
 * \endcode
 *
 * Plumbing — two modes, selected by which of \p uri / \p command is set:
 *  - external mode (\p uri non-empty, \p command empty): the NUT writer
 *    (ffmpeg) is launched externally (shell script, systemd unit) and
 *    writes the FIFO/file named by \p uri;
 *  - spawn mode (\p uri empty, \p command non-empty): the block runs the
 *    user-authored \p command via "/bin/sh -c" and reads the NUT stream
 *    from an anonymous pipe on the child's stdout. The command must emit
 *    NUT per the contract on stdout (i.e. end in "-f nut pipe:1" or
 *    "-f nut -"); the reference commands above are the suggested starting
 *    points. A full shell command keeps all of ffmpeg's power available
 *    (arbitrary filtergraphs, multiple inputs, pipelines like
 *    "curl ... | ffmpeg -i - ...") and carries the same trust level as a
 *    shell script the user would write anyway. The child's stdin is
 *    redirected to /dev/null (so ffmpeg cannot grab the terminal) and
 *    stderr is inherited so its diagnostics stay visible. Contract
 *    validation (§ above) is the guardrail: if the command's output does
 *    not match the declared parameters, start() throws the usual
 *    actionable error. The child runs in its own process group, spawned
 *    in start() before header validation; stop() SIGTERMs the group
 *    (grace, then SIGKILL) and reaps it — no zombies. For seamless
 *    looping, put -stream_loop -1 before -i in the command. Spawn mode
 *    is POSIX-only.
 * Setting both or neither of \p uri / \p command is a constructor error.
 *
 * Outputs: ports 0 .. audio_channels-1 are float audio (deinterleaved,
 * one per channel); if \p emit_video, the last port is an unsigned char
 * stream carrying rgb24 frames back to back (row-major). A stream tag is
 * attached to the first byte of every frame with keys "pts" (int64, in the
 * NUT stream timebase), "width" and "height". Steady-state consumers must
 * not need the tags (geometry is contractual); they exist for diagnostics
 * and alignment.
 *
 * Clocking: the flowgraph sink is the only clock. No Throttle blocks in TX
 * flowgraphs; ffmpeg is paced purely by pipe backpressure. When both audio
 * and video are declared, the initial pts offset between the streams is
 * trimmed at startup so both ports start at the same media time (within
 * one audio sample / one video frame). pts discontinuities beyond a few
 * milliseconds are logged as warnings, never resynced.
 *
 * Buffering rationale (do not shrink these): a multi-output demuxer with
 * bounded output buffers can deadlock when downstream ends in a
 * synchronous combiner — the demuxer blocks pushing stream A into a full
 * buffer while the consumer is starved of stream B, which the blocked
 * demuxer would deliver next. Because the NUT muxer bounds interleave skew
 * (-max_interleave_delta), sizing the output buffers above the maximum
 * interleave burst breaks the cycle: the constructor requests at least 1 s
 * of buffer on each audio port and at least 4 frames (4*W*H*3 bytes) on
 * the video port. Note that GR default buffers are tens of kB while a raw
 * frame is MBs — without this the video path would be broken out of the
 * box.
 */
class NUT_API nut_source : virtual public gr::block
{
public:
    typedef std::shared_ptr<nut_source> sptr;

    /*!
     * \brief Create a NUT source.
     *
     * \param uri path to a FIFO or file containing the NUT stream
     * \param audio_channels number of float audio output ports
     *        (deinterleaved); 0 for no audio
     * \param audio_rate expected audio sample rate in Hz (validated
     *        against the stream header, never adapted)
     * \param emit_video whether the video output port exists
     * \param video_width expected frame width (validated); required if
     *        emit_video
     * \param video_height expected frame height (validated); required if
     *        emit_video
     * \param command full shell command (run via "/bin/sh -c") whose
     *        stdout emits the NUT stream per the contract (spawn mode).
     *        Mutually exclusive with \p uri: exactly one of the two must
     *        be non-empty.
     */
    static sptr make(const std::string& uri,
                     int audio_channels,
                     int audio_rate,
                     bool emit_video,
                     int video_width,
                     int video_height,
                     const std::string& command = "");

    /*!
     * \brief The fatal error that terminated the flowgraph, or "" if none.
     *
     * Post-constructor failures (open/spawn failure, contract validation,
     * mid-stream violations) cannot propagate as exceptions from a GR
     * block thread; they are logged at ERROR level and end the flowgraph
     * cleanly instead. Call this after tb.run()/tb.wait() returns to
     * distinguish failure (non-empty, the same actionable message that
     * was logged) from clean EOF (empty).
     */
    virtual std::string last_error() const = 0;
};

} // namespace nut
} // namespace gr

#endif /* INCLUDED_NUT_NUT_SOURCE_H */
