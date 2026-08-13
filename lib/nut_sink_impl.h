/* -*- c++ -*- */
/*
 * Copyright 2026 Ardavast Dayleryan.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_NUT_NUT_SINK_IMPL_H
#define INCLUDED_NUT_NUT_SINK_IMPL_H

#include <gnuradio/nut/nut_sink.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <sys/types.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

namespace gr {
namespace nut {

class nut_sink_impl : public nut_sink
{
private:
    // ---- parameters (fixed at construction) ----
    const std::string d_uri;
    const int d_nchan; // audio channels == number of float ports (structural)
    const int d_rate;  // declared audio sample rate (headers + pts synthesis)
    const int d_nvideo;          // number of video streams == byte ports
    const std::string d_command; // spawn mode: shell command reading NUT on stdin
    const bool d_spawn;          // true when d_command is used (uri empty)
    const int d_nports;          // total number of input ports
    const double d_flush_timeout; // seconds to wait for the child after EOF
    // Port layout: audio0..d_nchan-1 (float), then video0..d_nvideo-1
    // (byte) at port indices d_nchan + v.

    // Per-video-stream state; index v == video port slot == the v-th
    // video stream in NUT stream order (written after the audio stream).
    struct video_state {
        int width = 0;  // declared geometry (broadcast-resolved)
        int height = 0;
        AVRational fps{ 0, 1 };    // declared frame rate (num/den)
        size_t frame_bytes = 0;    // W*H*3, one rgb24 frame
        int stream_idx = -1;       // container stream index
        AVRational time_base{ 0, 1 }; // post-write_header muxer timebase
        int64_t count = 0;         // frames written (pts synthesis)
        // partial-frame staging across work calls
        std::vector<uint8_t> stage;
        size_t stage_off = 0; // bytes of the next frame already staged
        bool warned_rx_time = false; // overflow-tag warning latch
    };
    std::vector<video_state> d_video; // size d_nvideo

    // ---- audio pts synthesis / stream state ----
    int d_audio_idx = -1;               // container stream index, -1 if none
    AVRational d_a_time_base{ 0, 1 };   // post-write_header muxer timebase
    int64_t d_a_count = 0;              // audio frames (sample groups) written
    bool d_a_warned_rx_time = false;    // overflow-tag warning latch

    // ---- output / libavformat state ----
    int d_fd = -1;         // write end: pipe (spawn), FIFO or file (uri)
    bool d_is_fifo = false;
    AVIOContext* d_avio = nullptr;
    AVFormatContext* d_oc = nullptr;
    bool d_header_written = false;
    bool d_finalized = false; // flush/trailer/close/child-wait already done

    // ---- spawned child (spawn mode; reads NUT on its stdin) ----
    pid_t d_child = -1;
    std::mutex d_child_mutex;

    // ---- stop/flush deadline ----
    // During a normal run writes block indefinitely on backpressure (the
    // reader paces us). Once a stop is requested, all remaining I/O — the
    // in-flight write, the flush, the trailer, waiting for the child —
    // shares ONE deadline of flush_timeout seconds; past it, d_abort is
    // latched and everything bails out. Note that stop() runs in the
    // block's own work thread with the boost interruption flag already
    // raised, so the flush path must NOT treat interruption as an abort —
    // the deadline is the only brake.
    std::atomic<bool> d_deadline_set{ false };
    std::chrono::steady_clock::time_point d_deadline;
    std::atomic<bool> d_abort{ false }; // fatal: stop trying to write

    // ---- fatal-error latch (see nut_sink.h "Error model") ----
    std::atomic<bool> d_failed{ false };
    mutable std::mutex d_error_mutex;
    std::string d_error; // guarded by d_error_mutex

    // ---- helpers ----
    // avio write callback (buf gained const in libavformat 61 / FFmpeg 7)
#if LIBAVFORMAT_VERSION_MAJOR >= 61
    static int write_cb(void* opaque, const uint8_t* buf, int buf_size);
#else
    static int write_cb(void* opaque, uint8_t* buf, int buf_size);
#endif
    int write_bytes(const uint8_t* buf, int buf_size); // the non-static body
    static int64_t seek_cb(void* opaque, int64_t offset, int whence);

    // Arm the flush deadline (idempotent; first caller wins).
    void arm_deadline();
    bool deadline_expired() const;
    // Milliseconds until the deadline, clamped to [0, cap]; cap if unset.
    int deadline_wait_ms(int cap) const;

    // Log msg at ERROR level, latch it for last_error(), mark the block
    // failed (work() then returns WORK_DONE immediately).
    void fail(const std::string& msg);

    void spawn_child(); // fork/exec "/bin/sh -c command"; throws on failure
    // Wait for the child to exit on its own until the deadline, then
    // SIGTERM the process group, grace, SIGKILL; always reaps.
    void wait_child_then_escalate() noexcept;

    void open_output(); // spawn/open + build streams + write header; throws
    void close_output() noexcept; // release everything (no flushing)
    // Salvage drain (see the .cc): GR declares a sink done as soon as ANY
    // input is exhausted, before work() gets to see the backlog still
    // queued on the other inputs — drain it straight from the input
    // buffers at shutdown, bounded by the deadline. Throws on write
    // errors.
    void drain_inputs();
    // Flush interleave queue, write trailer, close the fd (EOF), wait for
    // the child (deadline), escalate. Idempotent, never throws.
    void finalize() noexcept;
    void reset_stream_state();

    // Interleave n frames from the audio input buffers into one packet
    // and hand it to the muxer. Throws on write errors.
    void write_audio(gr_vector_const_void_star& input_items, int n);
    // Stage n bytes from video port v, emitting a packet per completed
    // frame. Throws on write errors.
    void write_video(int v, const uint8_t* in, int n);
    void send_packet(AVPacket* pkt, const char* what); // throws on error
    // WARN once per port when an overflow-indicating tag (rx_time beyond
    // offset 0) shows up: the synthesized timeline cannot represent the
    // gap and silently compresses.
    void check_overflow_tags(int port, int nitems, bool& warned_latch);

public:
    nut_sink_impl(const std::string& uri,
                  int audio_channels,
                  int audio_rate,
                  int video_streams,
                  const std::vector<int>& widths,
                  const std::vector<int>& heights,
                  const std::vector<std::string>& fps,
                  const std::string& command,
                  double flush_timeout);
    ~nut_sink_impl() override;

    bool start() override;
    bool stop() override;
    std::string last_error() const override;

    void forecast(int noutput_items, gr_vector_int& ninput_items_required) override;

    int general_work(int noutput_items,
                     gr_vector_int& ninput_items,
                     gr_vector_const_void_star& input_items,
                     gr_vector_void_star& output_items) override;
};

} // namespace nut
} // namespace gr

#endif /* INCLUDED_NUT_NUT_SINK_IMPL_H */
