/* -*- c++ -*- */
/*
 * Copyright 2026 Ardavast Dayleryan.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_NUT_NUT_SOURCE_IMPL_H
#define INCLUDED_NUT_NUT_SOURCE_IMPL_H

#include <gnuradio/nut/nut_source.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <sys/types.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
}

namespace gr {
namespace nut {

class nut_source_impl : public nut_source
{
private:
    // ---- parameters (fixed at construction) ----
    const std::string d_uri;
    const int d_nchan;      // audio channels == number of float ports
    const int d_rate;       // expected audio sample rate
    const bool d_emit_video;
    const int d_width;
    const int d_height;
    const bool d_repeat;
    const std::string d_command; // spawn mode: shell command emitting NUT on stdout
    const bool d_spawn;          // true when d_command is used (uri empty)
    const int d_video_port;      // output port index of the video port (== d_nchan)
    const int d_nports;          // total number of output ports
    const size_t d_frame_bytes;  // W*H*3, one rgb24 frame

    // ---- input / libavformat state ----
    int d_fd = -1;
    bool d_is_fifo = false;
    bool d_saw_writer = false; // FIFO: a writer has connected at least once
    AVIOContext* d_avio = nullptr;
    AVFormatContext* d_fmt = nullptr;
    int d_audio_idx = -1; // stream index of the audio stream, -1 if none
    int d_video_idx = -1; // stream index of the video stream, -1 if none

    // ---- spawned ffmpeg child (spawn mode) ----
    pid_t d_child = -1;
    std::mutex d_child_mutex; // stop() signals from another thread

    std::atomic<bool> d_stop{ false };
    bool d_eof = false;

    // ---- one-packet staging area per stream (§4.3 of the design) ----
    AVPacket* d_rd_pkt = nullptr; // scratch packet for av_read_frame
    AVPacket* d_a_pkt = nullptr;  // staged audio packet
    bool d_a_staged = false;
    size_t d_a_off = 0;          // audio frames (sample groups) already emitted
    AVPacket* d_v_pkt = nullptr; // staged video packet (one frame)
    bool d_v_staged = false;
    size_t d_v_off = 0;          // bytes of the staged frame already emitted
    int64_t d_v_staged_pts = 0;  // pts of the staged frame (stream timebase)

    // ---- initial pts trim (§4.6) ----
    bool d_priming = false; // still collecting first pts of both streams
    std::deque<AVPacket*> d_prime_q;
    int64_t d_a_first_us = INT64_MIN;
    int64_t d_v_first_us = INT64_MIN;
    int64_t d_a_skip_frames = 0;             // audio frames to drop at start
    int64_t d_v_skip_before_us = INT64_MIN;  // drop video frames before this

    // ---- pts continuity watch ----
    bool d_a_expect_valid = false;
    int64_t d_a_expect_us = 0;
    bool d_v_expect_valid = false;
    int64_t d_v_expect_us = 0;

    // ---- helpers ----
    static int read_cb(void* opaque, uint8_t* buf, int buf_size);
    static int64_t seek_cb(void* opaque, int64_t offset, int whence);
    static int interrupt_cb(void* opaque);
    bool interrupted() const;

    void spawn_child(); // fork/exec "/bin/sh -c command"; throws on failure
    void terminate_child(bool quiet) noexcept; // TERM group, grace, KILL, waitpid

    void open_input();           // open + parse headers + validate; throws
    void close_input() noexcept; // release everything, clear staging
    void reset_stream_state();   // pts / staging / priming bookkeeping
    void validate_streams();     // §4.4; throws std::runtime_error

    int64_t pkt_pts_us(const AVPacket* pkt) const;
    int64_t video_frame_dur_us(const AVPacket* pkt) const;

    // Returns 0, AVERROR_EOF, AVERROR_EXIT or another AVERROR.
    int next_packet(AVPacket* pkt);
    // Collect packets until the first pts of both streams is known.
    // Returns 0 on success/EOF, AVERROR_EXIT if interrupted.
    int prime();
    void finish_priming();
    // Route d_rd_pkt into the right staging slot (or drop it). Throws on
    // mid-stream contract violations.
    void stage_packet(AVPacket* pkt);
    void check_continuity_audio(const AVPacket* pkt);
    void check_continuity_video(const AVPacket* pkt);

    void flush_audio(int noutput_items,
                     gr_vector_void_star& output_items,
                     std::vector<int>& produced);
    void flush_video(int noutput_items,
                     gr_vector_void_star& output_items,
                     std::vector<int>& produced);
    bool try_reopen();

public:
    nut_source_impl(const std::string& uri,
                    int audio_channels,
                    int audio_rate,
                    bool emit_video,
                    int video_width,
                    int video_height,
                    bool repeat,
                    const std::string& command);
    ~nut_source_impl() override;

    bool start() override;
    bool stop() override;

    void forecast(int noutput_items, gr_vector_int& ninput_items_required) override;

    int general_work(int noutput_items,
                     gr_vector_int& ninput_items,
                     gr_vector_const_void_star& input_items,
                     gr_vector_void_star& output_items) override;
};

} // namespace nut
} // namespace gr

#endif /* INCLUDED_NUT_NUT_SOURCE_IMPL_H */
