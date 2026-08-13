/* -*- c++ -*- */
/*
 * Copyright 2026 Ardavast Dayleryan.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_NUT_NUT_SINK_H
#define INCLUDED_NUT_NUT_SINK_H

#include <gnuradio/block.h>
#include <gnuradio/nut/api.h>

#include <string>
#include <vector>

namespace gr {
namespace nut {

/*!
 * \brief Sink block that muxes float audio and rgb24 video byte streams
 *        into a raw-payload NUT stream and feeds it to ffmpeg (or a
 *        FIFO/file) for encoding, recording, or streaming.
 * \ingroup nut
 *
 * \details
 * The reverse of nut_source: the flowgraph produces the media, this block
 * writes the same strict NUT profile (pcm_f32le interleaved audio + one
 * or more rawvideo/rgb24 streams). Where the source ADOPTS the format
 * from the stream headers, the sink DECLARES it via parameters — the
 * block writes the headers, so the rate/geometry/fps parameters are the
 * single source of truth, and timestamps are synthesized from sample and
 * frame counts (pts = emitted count in the stream timebase; exact by
 * construction). Consequently the rational-ratio duty is UPSTREAM of this
 * block: a producer whose actual rate differs from the declared one
 * yields an internally-consistent-but-wrong file, invisible in-band. If
 * the upstream is an SDR source, receiver overflows silently compress
 * the synthesized timeline; the block WARNs when it sees
 * overflow-indicating tags (an "rx_time" tag mid-stream) on its inputs,
 * but cannot repair the loss.
 *
 * Error model (same as nut_source): constructor-time misuse — both or
 * neither of \p uri / \p command, invalid vector lengths, a rejected fps
 * spelling — throws std::invalid_argument in the caller's thread.
 * Post-constructor failures (spawn/open failure, a dying downstream
 * reader) are logged at ERROR level and terminate the flowgraph cleanly
 * (WORK_DONE); after tb.run()/tb.wait() returns, last_error()
 * distinguishes failure (non-empty message) from a clean stop (empty).
 * A downstream reader that disappears mid-stream (EPIPE on write) is an
 * ERROR, not an EOF: unlike the source — where a dead writer simply ends
 * the media — a dead reader means the recording/encode was lost.
 *
 * Inputs: ports 0 .. audio_channels-1 are float audio (deinterleaved,
 * one per channel; the block interleaves them into ONE pcm_f32le stream
 * with audio_channels channels); ports audio_channels ..
 * audio_channels+video_streams-1 are unsigned char streams carrying
 * rgb24 frames back to back (row-major), cut into widths[i] x heights[i]
 * x 3-byte frames (partial frames are staged internally across work
 * calls). Video stream i is written in NUT stream order after the audio
 * stream, so an ffmpeg reading the output sees the streams in port
 * order.
 *
 * fps is one string per video stream: an integer ("25") or an exact
 * rational ("30000/1001"). Decimal spellings are REJECTED — 29.97 is not
 * exact; use 30000/1001. widths/heights/fps must each have length
 * video_streams, or length 1 to apply to every stream.
 *
 * Consumption and interleaving: the block consumes greedily from
 * whichever input has data and hands packets to libavformat's
 * interleave queue, which reorders by time and bounds its memory by
 * max_interleave_delta (1 s, set by the block) — skewed production
 * (audio far ahead of video or vice versa) can therefore never deadlock
 * the flowgraph; in the worst case the output is force-flushed with
 * degenerate interleaving, which remains a valid NUT file.
 *
 * Plumbing — two modes, selected by which of \p uri / \p command is set:
 *  - external mode (\p uri non-empty, \p command empty): the NUT stream
 *    is written to the FIFO or file named by \p uri; the reader (ffmpeg,
 *    started externally) is launched by a shell script/systemd unit. For
 *    a FIFO, start() waits for the reader to appear.
 *  - spawn mode (\p uri empty, \p command non-empty): the block runs the
 *    user-authored \p command via "/bin/sh -c" and feeds the NUT stream
 *    to the child's STDIN over an anonymous pipe. The command reads NUT
 *    from stdin (ffmpeg: "-i pipe:0" or "-i -"; no -ar/-s/-r input flags
 *    are needed — the NUT headers carry the format), e.g.:
 * \code
 * ffmpeg -y -loglevel warning -i pipe:0 /tmp/recording.flac
 * \endcode
 *    stdout and stderr are inherited so the child's diagnostics stay
 *    visible; the child runs in its own process group. Anything goes
 *    (encoders, filters, pipelines, network pushes) as long as it reads
 *    NUT on stdin.
 * Setting both or neither of \p uri / \p command is a constructor error.
 *
 * Shutdown (the mirror of the source's, in the opposite order): on
 * stop() the block flushes the interleave queue, writes the NUT trailer,
 * closes the pipe/FIFO (delivering EOF to the reader), then WAITS for a
 * spawned child to exit on its own — the child finishing (encoder
 * flushed, container finalized) is the desired end state; killing it
 * early corrupts the recording. Only after \p flush_timeout seconds
 * (default 10) does the block escalate to SIGTERM (then SIGKILL) of the
 * child's process group. In external mode the same flush ordering
 * applies and the FIFO close delivers the EOF; there is no child to wait
 * for. Note that tb.stop() discards flowgraph samples not yet consumed
 * by the block — the guarantee is that everything CONSUMED is muxed,
 * flushed, and finalized.
 *
 * Clocking: as with the source, the chain must contain at most one
 * pacer (see docs/clocking.md, cases K0-K3b). Typical sink chains:
 * SDR RX -> nut_sink -> ffmpeg encoding to a file (K1, the SDR paces),
 * or a generator -> nut_sink -> ffmpeg to a file (K0, free-running
 * faster-than-real-time render). No Throttle blocks; ffmpeg needs no
 * -re on the reading side ever.
 */
class NUT_API nut_sink : virtual public gr::block
{
public:
    typedef std::shared_ptr<nut_sink> sptr;

    /*!
     * \brief Create a NUT sink.
     *
     * \param uri path of a FIFO or file to write the NUT stream to
     *        (external mode). Mutually exclusive with \p command.
     * \param audio_channels number of float audio input ports
     *        (deinterleaved; interleaved by the block into one
     *        pcm_f32le stream with this many channels); 0 for no audio.
     * \param audio_rate declared audio sample rate in Hz (written into
     *        the NUT headers; audio pts are synthesized against it).
     *        Must be > 0 when audio_channels > 0; ignored otherwise.
     * \param video_streams number of rawvideo/rgb24 streams == number of
     *        byte input ports; 0 for no video.
     * \param widths declared frame width per video stream (length
     *        video_streams, or length 1 to apply to all).
     * \param heights declared frame height per video stream (same
     *        length rule).
     * \param fps declared frame rate per video stream (same length
     *        rule): an integer ("25") or an exact rational
     *        ("30000/1001"). Decimals are rejected.
     * \param command full shell command (run via "/bin/sh -c") that
     *        reads the NUT stream on its STDIN (spawn mode). Mutually
     *        exclusive with \p uri: exactly one of the two must be
     *        non-empty.
     * \param flush_timeout seconds to wait, after flushing and closing
     *        the stream on stop(), for a spawned command to exit on its
     *        own before escalating to SIGTERM/SIGKILL. Default 10.
     */
    static sptr make(const std::string& uri,
                     int audio_channels,
                     int audio_rate,
                     int video_streams,
                     const std::vector<int>& widths,
                     const std::vector<int>& heights,
                     const std::vector<std::string>& fps,
                     const std::string& command = "",
                     double flush_timeout = 10.0);

    /*!
     * \brief The fatal error that terminated the flowgraph, or "" if none.
     *
     * Post-constructor failures (spawn/open failure, EPIPE from a dying
     * downstream reader, write errors) cannot propagate as exceptions
     * from a GR block thread; they are logged at ERROR level and end the
     * flowgraph cleanly instead. Call this after tb.run()/tb.wait()
     * returns to distinguish failure (non-empty, the same actionable
     * message that was logged) from a clean stop (empty).
     */
    virtual std::string last_error() const = 0;
};

} // namespace nut
} // namespace gr

#endif /* INCLUDED_NUT_NUT_SINK_H */
