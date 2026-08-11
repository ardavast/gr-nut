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
    const int d_nchan;  // audio channels == number of float ports (structural)
    const int d_nvideo; // number of video streams == byte ports (structural)
    const std::string d_command; // spawn mode: shell command emitting NUT on stdout
    const bool d_spawn;          // true when d_command is used (uri empty)
    const int d_nports;          // total number of output ports
    // Port layout: audio0..d_nchan-1 (float), then video0..d_nvideo-1
    // (byte) at port indices d_nchan + v.

    // ---- format adopted from the NUT stream headers at start() ----
    // (0 until adopted; capped, see MAX_* in the .cc — output buffers are
    // sized in the constructor, before the headers are readable)
    int d_rate = 0; // audio sample rate

    // Per-video-stream state; index v == video port slot == the v-th video
    // stream in NUT stream order (== ffmpeg -map order of the writer).
    struct video_state {
        int stream_idx = -1;     // container stream index
        int width = 0;           // adopted geometry
        int height = 0;
        size_t frame_bytes = 0;  // W*H*3, one rgb24 frame
        // one-packet staging
        AVPacket* pkt = nullptr;
        bool staged = false;
        size_t off = 0;          // bytes of the staged frame already emitted
        int64_t staged_pts = 0;  // pts of the staged frame (stream timebase)
        // initial pts trim
        int64_t first_us = INT64_MIN;
        int64_t skip_before_us = INT64_MIN; // drop frames before this
        // continuity watch
        bool expect_valid = false;
        int64_t expect_us = 0;
    };
    std::vector<video_state> d_video; // size d_nvideo

    // ---- input / libavformat state ----
    int d_fd = -1;
    bool d_is_fifo = false;
    bool d_saw_writer = false; // FIFO: a writer has connected at least once
    AVIOContext* d_avio = nullptr;
    AVFormatContext* d_fmt = nullptr;
    int d_audio_idx = -1; // stream index of the audio stream, -1 if none

    // ---- spawned ffmpeg child (spawn mode) ----
    pid_t d_child = -1;
    std::mutex d_child_mutex; // stop() signals from another thread

    std::atomic<bool> d_stop{ false };
    bool d_eof = false;

    // ---- fatal-error latch (see nut_source.h "Error model") ----
    std::atomic<bool> d_failed{ false };
    mutable std::mutex d_error_mutex;
    std::string d_error; // guarded by d_error_mutex

    // ---- one-packet staging area per stream (§4.3 of the design) ----
    // (video staging lives in d_video[v]; audio here)
    AVPacket* d_rd_pkt = nullptr; // scratch packet for av_read_frame
    AVPacket* d_a_pkt = nullptr;  // staged audio packet
    bool d_a_staged = false;
    size_t d_a_off = 0; // audio frames (sample groups) already emitted

    // ---- initial pts trim (§4.6): all streams start at the latest
    // first-pts (video per-stream state lives in d_video[v]) ----
    bool d_priming = false; // still collecting first pts of every stream
    std::deque<AVPacket*> d_prime_q;
    int64_t d_a_first_us = INT64_MIN;
    int64_t d_a_skip_frames = 0; // audio frames to drop at start

    // ---- pts continuity watch (audio; video in d_video[v]) ----
    bool d_a_expect_valid = false;
    int64_t d_a_expect_us = 0;

    // ---- helpers ----
    static int read_cb(void* opaque, uint8_t* buf, int buf_size);
    static int64_t seek_cb(void* opaque, int64_t offset, int whence);
    static int interrupt_cb(void* opaque);
    bool interrupted() const;

    // Log msg at ERROR level, latch it for last_error(), mark the block
    // failed (work() then returns WORK_DONE immediately).
    void fail(const std::string& msg);

    void spawn_child(); // fork/exec "/bin/sh -c command"; throws on failure
    void terminate_child(bool quiet) noexcept; // TERM group, grace, KILL, waitpid

    void open_input();           // open + parse headers + validate; throws
    void close_input() noexcept; // release everything, clear staging
    void reset_stream_state();   // pts / staging / priming bookkeeping
    // Structural validation + adoption of rate/geometry from the headers
    // (§4.4); throws std::runtime_error.
    void validate_and_adopt_streams();

    int64_t pkt_pts_us(const AVPacket* pkt) const;
    int64_t video_frame_dur_us(const AVPacket* pkt) const;
    // Video slot (0..d_nvideo-1) for a container stream index, -1 if none.
    int video_slot(int stream_idx) const;
    bool any_video_staged() const;

    // Returns 0, AVERROR_EOF, AVERROR_EXIT or another AVERROR.
    int next_packet(AVPacket* pkt);
    // Collect packets until the first pts of every declared stream is
    // known. Returns 0 on success/EOF, AVERROR_EXIT if interrupted.
    int prime();
    void finish_priming();
    // Route d_rd_pkt into the right staging slot (or drop it). Throws on
    // mid-stream contract violations.
    void stage_packet(AVPacket* pkt);
    void check_continuity_audio(const AVPacket* pkt);
    void check_continuity_video(video_state& vs, const AVPacket* pkt);

    void flush_audio(int noutput_items,
                     gr_vector_void_star& output_items,
                     std::vector<int>& produced);
    void flush_video(int slot,
                     int noutput_items,
                     gr_vector_void_star& output_items,
                     std::vector<int>& produced);

public:
    nut_source_impl(const std::string& uri,
                    int audio_channels,
                    int video_streams,
                    const std::string& command);
    ~nut_source_impl() override;

    bool start() override;
    bool stop() override;
    std::string last_error() const override;
    int audio_rate() const override { return d_rate; }
    int video_width(int stream = 0) const override
    {
        return (stream >= 0 && stream < d_nvideo) ? d_video[stream].width : 0;
    }
    int video_height(int stream = 0) const override
    {
        return (stream >= 0 && stream < d_nvideo) ? d_video[stream].height : 0;
    }

    void forecast(int noutput_items, gr_vector_int& ninput_items_required) override;

    int general_work(int noutput_items,
                     gr_vector_int& ninput_items,
                     gr_vector_const_void_star& input_items,
                     gr_vector_void_star& output_items) override;
};

} // namespace nut
} // namespace gr

#endif /* INCLUDED_NUT_NUT_SOURCE_IMPL_H */
