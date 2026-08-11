/* -*- c++ -*- */
/*
 * Copyright 2026 Ardavast Dayleryan.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nut_source_impl.h"
#include <gnuradio/io_signature.h>

#include <boost/thread/thread.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/codec_desc.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace gr {
namespace nut {

namespace {

constexpr int AVIO_BUF_SIZE = 32768;
constexpr int POLL_INTERVAL_MS = 100;    // stop-flag poll period while blocked
constexpr int64_t PTS_WARN_US = 5000;    // continuity warning threshold
constexpr size_t PRIME_MAX_PACKETS = 100000;
constexpr size_t PRIME_MAX_BYTES = 256u * 1024 * 1024;
const AVRational US_TB = { 1, 1000000 };

// Buffer caps. The audio rate and video geometry are adopted from the NUT
// headers at start(), but GR allocates stream buffers during tb.start()
// flowgraph setup — BEFORE block::start() can read the headers — so
// set_min_output_buffer cannot be deferred until the format is known. The
// constructor therefore requests fixed generous caps, and start() fails
// cleanly if the adopted format exceeds them. The size floor itself is the
// §4.7 deadlock prevention: the NUT muxer bounds interleave skew
// (-max_interleave_delta), so buffers larger than the maximum interleave
// burst guarantee a synchronous downstream combiner can always be fed.
constexpr int MAX_AUDIO_RATE = 192000; // buffer = 1 s of audio at this cap
constexpr size_t MAX_FRAME_BYTES = size_t(1920) * 1080 * 3; // one 1080p rgb24 frame
constexpr int VIDEO_BUFFER_FRAMES = 4; // >= 4 frames of buffer (~24.9 MB)

std::string averr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

const char* codec_name(AVCodecID id)
{
    const AVCodecDescriptor* d = avcodec_descriptor_get(id);
    return d ? d->name : "unknown";
}

const char* pix_fmt_name(int fmt)
{
    const char* n = av_get_pix_fmt_name(static_cast<AVPixelFormat>(fmt));
    return n ? n : "unknown";
}

// Validates the constructor parameters and builds the output signature
// (N float audio ports, then one byte video port if emit_video).
std::vector<int> out_sizes(int audio_channels, bool emit_video)
{
    if (audio_channels < 0)
        throw std::invalid_argument("nut_source: audio_channels must be >= 0");
    if (audio_channels == 0 && !emit_video)
        throw std::invalid_argument(
            "nut_source: block would have no outputs — set audio_channels > 0 and/or "
            "emit_video");
    std::vector<int> sizes(audio_channels, sizeof(float));
    if (emit_video)
        sizes.push_back(sizeof(unsigned char));
    return sizes;
}

} // namespace

nut_source::sptr nut_source::make(const std::string& uri,
                                  int audio_channels,
                                  bool emit_video,
                                  const std::string& command)
{
    return gnuradio::make_block_sptr<nut_source_impl>(
        uri, audio_channels, emit_video, command);
}

nut_source_impl::nut_source_impl(const std::string& uri,
                                 int audio_channels,
                                 bool emit_video,
                                 const std::string& command)
    : gr::block("nut_source",
                gr::io_signature::make(0, 0, 0),
                gr::io_signature::makev(audio_channels + (emit_video ? 1 : 0),
                                        audio_channels + (emit_video ? 1 : 0),
                                        out_sizes(audio_channels, emit_video))),
      d_uri(uri),
      d_nchan(audio_channels),
      d_emit_video(emit_video),
      d_command(command),
      d_spawn(uri.empty()),
      d_video_port(audio_channels),
      d_nports(audio_channels + (emit_video ? 1 : 0))
{
    if (uri.empty() && command.empty())
        throw std::invalid_argument(
            "nut_source: no input given — set uri (path to a FIFO/file written by an "
            "external NUT writer) or command (a shell command emitting NUT on "
            "stdout, e.g. 'ffmpeg -i ... -f nut pipe:1', to be spawned by the "
            "block)");
    if (!uri.empty() && !command.empty())
        throw std::invalid_argument(
            "nut_source: uri and command are mutually exclusive — set uri for an "
            "external NUT writer, or leave it empty and set command to have the "
            "block spawn the writer itself");

    d_rd_pkt = av_packet_alloc();
    d_a_pkt = av_packet_alloc();
    d_v_pkt = av_packet_alloc();
    if (!d_rd_pkt || !d_a_pkt || !d_v_pkt)
        throw std::runtime_error("nut_source: av_packet_alloc failed");

    // Fixed generous buffer caps (see MAX_* above): buffers must be sized
    // here — GR allocates them during tb.start() flowgraph setup, before
    // start() can read the stream headers — and the sizing is what prevents
    // the §4.7 demuxer deadlock.
    for (int i = 0; i < d_nchan; i++)
        set_min_output_buffer(i, MAX_AUDIO_RATE); // 1 s of audio at the cap
    if (d_emit_video)
        set_min_output_buffer(d_video_port, VIDEO_BUFFER_FRAMES * MAX_FRAME_BYTES);
}

nut_source_impl::~nut_source_impl()
{
    close_input();
    av_packet_free(&d_rd_pkt);
    av_packet_free(&d_a_pkt);
    av_packet_free(&d_v_pkt);
}

bool nut_source_impl::interrupted() const
{
    return d_stop.load(std::memory_order_relaxed) ||
           boost::this_thread::interruption_requested();
}

int nut_source_impl::interrupt_cb(void* opaque)
{
    return static_cast<nut_source_impl*>(opaque)->interrupted() ? 1 : 0;
}

// Custom AVIO read: non-blocking fd + poll loop so that a stop request can
// always unblock a read that would otherwise wait forever on an idle pipe.
int nut_source_impl::read_cb(void* opaque, uint8_t* buf, int buf_size)
{
    auto* self = static_cast<nut_source_impl*>(opaque);
    for (;;) {
        if (self->interrupted())
            return AVERROR_EXIT;
        ssize_t n = ::read(self->d_fd, buf, buf_size);
        if (n > 0) {
            self->d_saw_writer = true;
            return static_cast<int>(n);
        }
        if (n == 0) {
            if (!self->d_is_fifo)
                return AVERROR_EOF; // regular file: end of data
            if (self->d_saw_writer)
                return AVERROR_EOF; // FIFO: writer connected earlier, now gone
            // FIFO with no writer yet: wait for one to appear.
            ::poll(nullptr, 0, POLL_INTERVAL_MS);
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // FIFO writer connected but idle.
            self->d_saw_writer = true;
            struct pollfd p = { self->d_fd, POLLIN, 0 };
            ::poll(&p, 1, POLL_INTERVAL_MS);
            continue;
        }
        if (errno == EINTR)
            continue;
        return AVERROR(errno);
    }
}

int64_t nut_source_impl::seek_cb(void* opaque, int64_t offset, int whence)
{
    auto* self = static_cast<nut_source_impl*>(opaque);
    if (self->d_is_fifo)
        return AVERROR(ESPIPE);
    if (whence & AVSEEK_SIZE) {
        struct stat st;
        if (fstat(self->d_fd, &st) != 0)
            return AVERROR(errno);
        return st.st_size;
    }
    whence &= ~AVSEEK_FORCE;
    off_t r = ::lseek(self->d_fd, offset, whence);
    return r < 0 ? AVERROR(errno) : r;
}

void nut_source_impl::reset_stream_state()
{
    av_packet_unref(d_a_pkt);
    av_packet_unref(d_v_pkt);
    d_a_staged = d_v_staged = false;
    d_a_off = d_v_off = 0;
    d_v_staged_pts = 0;
    for (AVPacket* p : d_prime_q)
        av_packet_free(&p);
    d_prime_q.clear();
    d_priming = false;
    d_a_first_us = d_v_first_us = INT64_MIN;
    d_a_skip_frames = 0;
    d_v_skip_before_us = INT64_MIN;
    d_a_expect_valid = d_v_expect_valid = false;
    d_a_expect_us = d_v_expect_us = 0;
}

// Spawn mode (POSIX-only): run the user-authored command via /bin/sh -c and
// read the NUT stream from an anonymous pipe on its stdout. The command
// carries the same trust level as a shell script the user would write
// anyway; contract validation in validate_streams() is the guardrail
// against commands whose output does not match the declared parameters.
void nut_source_impl::spawn_child()
{
    d_logger->info("spawning: /bin/sh -c \"{}\"", d_command);

    int out_pipe[2]; // child's stdout -> our reader
    int err_pipe[2]; // exec-failure reporting (CLOEXEC survives on success)
    if (pipe2(out_pipe, O_CLOEXEC) != 0)
        throw std::runtime_error(std::string("nut_source: pipe2 failed: ") +
                                 std::strerror(errno));
    if (pipe2(err_pipe, O_CLOEXEC) != 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        throw std::runtime_error(std::string("nut_source: pipe2 failed: ") +
                                 std::strerror(errno));
    }

    const pid_t pid = fork();
    if (pid < 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        throw std::runtime_error(std::string("nut_source: fork failed: ") +
                                 std::strerror(errno));
    }
    if (pid == 0) {
        // Child. Own process group so stop() can signal the whole pipeline
        // the shell may create (e.g. curl | ffmpeg). stderr is inherited so
        // diagnostics stay visible; stdin comes from /dev/null so ffmpeg
        // cannot grab the terminal.
        setpgid(0, 0);
        const int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull < 0 || dup2(devnull, STDIN_FILENO) < 0 ||
            dup2(out_pipe[1], STDOUT_FILENO) < 0)
            _exit(127);
        execl("/bin/sh", "sh", "-c", d_command.c_str(), (char*)nullptr);
        const int e = errno;
        ssize_t n = ::write(err_pipe[1], &e, sizeof(e));
        (void)n;
        _exit(127);
    }

    // Parent. Also set the pgid here to close the fork/kill race.
    setpgid(pid, pid);
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    int exec_errno = 0;
    const ssize_t n = ::read(err_pipe[0], &exec_errno, sizeof(exec_errno));
    ::close(err_pipe[0]);
    if (n > 0) {
        // exec of /bin/sh failed; the child has already exited.
        int status = 0;
        waitpid(pid, &status, 0);
        ::close(out_pipe[0]);
        throw std::runtime_error(std::string("nut_source: cannot exec /bin/sh: ") +
                                 std::strerror(exec_errno));
    }

    fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
    d_fd = out_pipe[0];
    {
        std::lock_guard<std::mutex> lock(d_child_mutex);
        d_child = pid;
    }
}

void nut_source_impl::terminate_child(bool quiet) noexcept
{
    pid_t pid;
    {
        std::lock_guard<std::mutex> lock(d_child_mutex);
        pid = d_child;
        d_child = -1;
    }
    if (pid <= 0)
        return;

    int status = 0;
    auto log_exit = [&](bool killed) {
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0 && !quiet)
            d_logger->warn(
                "spawned command exited with status {} — check its stderr above "
                "(bad path/URL, network error, ...)",
                WEXITSTATUS(status));
        else if (killed && !quiet)
            d_logger->warn("spawned command did not exit on SIGTERM; killed");
    };

    if (waitpid(pid, &status, WNOHANG) == pid) {
        // Already exited (e.g. at EOF). Sweep any stragglers of its group.
        ::kill(-pid, SIGTERM);
        log_exit(false);
        return;
    }
    // Signal the whole process group: /bin/sh may have spawned a pipeline.
    ::kill(-pid, SIGTERM);
    for (int i = 0; i < 40; i++) { // up to ~2 s of grace
        if (waitpid(pid, &status, WNOHANG) == pid) {
            log_exit(false);
            return;
        }
        ::poll(nullptr, 0, 50);
    }
    ::kill(-pid, SIGKILL);
    waitpid(pid, &status, 0);
    log_exit(true);
}

void nut_source_impl::close_input() noexcept
{
    terminate_child(/*quiet=*/d_stop.load());
    if (d_fmt)
        avformat_close_input(&d_fmt);
    if (d_avio) {
        av_freep(&d_avio->buffer);
        avio_context_free(&d_avio);
    }
    if (d_fd >= 0) {
        ::close(d_fd);
        d_fd = -1;
    }
    reset_stream_state();
}

void nut_source_impl::open_input()
{
    close_input();

    if (d_spawn) {
        spawn_child(); // sets d_fd to the pipe's read end; throws on exec failure
        d_is_fifo = true;     // anonymous pipe: FIFO read semantics
        d_saw_writer = true;  // the child holds the write end from the start
    } else {
        d_saw_writer = false;
        d_fd = ::open(d_uri.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (d_fd < 0)
            throw std::runtime_error("nut_source: cannot open '" + d_uri +
                                     "': " + std::strerror(errno));
        struct stat st;
        if (fstat(d_fd, &st) != 0) {
            close_input();
            throw std::runtime_error("nut_source: fstat('" + d_uri +
                                     "') failed: " + std::strerror(errno));
        }
        d_is_fifo = S_ISFIFO(st.st_mode);
    }

    auto* iobuf = static_cast<unsigned char*>(av_malloc(AVIO_BUF_SIZE));
    if (!iobuf) {
        close_input();
        throw std::runtime_error("nut_source: av_malloc failed");
    }
    d_avio = avio_alloc_context(
        iobuf, AVIO_BUF_SIZE, 0, this, &nut_source_impl::read_cb, nullptr,
        d_is_fifo ? nullptr : &nut_source_impl::seek_cb);
    if (!d_avio) {
        av_free(iobuf);
        close_input();
        throw std::runtime_error("nut_source: avio_alloc_context failed");
    }

    d_fmt = avformat_alloc_context();
    if (!d_fmt) {
        close_input();
        throw std::runtime_error("nut_source: avformat_alloc_context failed");
    }
    d_fmt->pb = d_avio;
    d_fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    d_fmt->interrupt_callback.callback = &nut_source_impl::interrupt_cb;
    d_fmt->interrupt_callback.opaque = this;

    const AVInputFormat* infmt = av_find_input_format("nut");
    if (!infmt) {
        close_input();
        throw std::runtime_error(
            "nut_source: libavformat has no NUT demuxer (broken ffmpeg build?)");
    }

    const std::string label =
        d_spawn ? "spawned command \"" + d_command + "\"" : "'" + d_uri + "'";
    int ret = avformat_open_input(
        &d_fmt, d_spawn ? "pipe:" : d_uri.c_str(), infmt, nullptr);
    if (ret < 0) {
        // avformat_open_input frees d_fmt on failure; d_avio is still ours.
        std::string why =
            interrupted() ? "interrupted by stop" : averr(ret);
        close_input(); // reaps the child in spawn mode (exit status is logged)
        throw std::runtime_error(
            "nut_source: cannot parse NUT stream from " + label + " (" + why +
            ") — " +
            (d_spawn ? "check your spawn command: it likely failed (see its stderr "
                       "above) or does not write NUT to stdout (it must end in "
                       "'-f nut pipe:1')"
                     : "is the writer running the reference ffmpeg command "
                       "(-f nut)?"));
    }

    // Raw codecs carry complete parameters in the NUT stream headers; do not
    // buffer video frames just to estimate the frame rate.
    av_opt_set_int(d_fmt, "fpsprobesize", 0, 0);
    ret = avformat_find_stream_info(d_fmt, nullptr);
    if (ret < 0) {
        std::string why = interrupted() ? "interrupted by stop" : averr(ret);
        close_input();
        throw std::runtime_error("nut_source: cannot read stream info from " + label +
                                 " (" + why + ")");
    }

    try {
        validate_and_adopt_streams();
    } catch (...) {
        close_input();
        throw;
    }

    d_priming = (d_nchan > 0 && d_emit_video);
}

void nut_source_impl::validate_and_adopt_streams()
{
    int n_audio = 0, n_video = 0, n_other = 0;
    d_audio_idx = d_video_idx = -1;
    for (unsigned i = 0; i < d_fmt->nb_streams; i++) {
        const AVCodecParameters* cp = d_fmt->streams[i]->codecpar;
        if (cp->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (n_audio++ == 0)
                d_audio_idx = static_cast<int>(i);
        } else if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (n_video++ == 0)
                d_video_idx = static_cast<int>(i);
        } else {
            n_other++;
        }
    }

    const char* hint = d_spawn ? " (check your spawn command)" : "";
    const int want_audio = d_nchan > 0 ? 1 : 0;
    const int want_video = d_emit_video ? 1 : 0;
    if (n_audio != want_audio || n_video != want_video || n_other != 0) {
        std::ostringstream os;
        os << "nut_source: stream layout mismatch: expected exactly " << want_audio
           << " audio and " << want_video << " video stream(s), found " << n_audio
           << " audio, " << n_video << " video, " << n_other
           << " other — fix the ffmpeg -map/-vn/-an options" << hint;
        throw std::runtime_error(os.str());
    }

    // Structural validation: codec and channel count are part of the
    // instance shape. Rate and geometry are NOT validated — the ffmpeg
    // command is the single source of truth; they are adopted below
    // (subject only to the buffer caps).
    std::ostringstream adopted;
    if (d_nchan > 0) {
        const AVCodecParameters* cp = d_fmt->streams[d_audio_idx]->codecpar;
        const int ch = cp->ch_layout.nb_channels;
        if (cp->codec_id != AV_CODEC_ID_PCM_F32LE || ch != d_nchan) {
            std::ostringstream os;
            os << "nut_source: expected " << d_nchan
               << " audio channel(s) of pcm_f32le, stream has " << ch
               << " channel(s) of " << codec_name(cp->codec_id)
               << " — fix the ffmpeg -ac/-c:a options" << hint;
            throw std::runtime_error(os.str());
        }
        if (cp->sample_rate <= 0) {
            throw std::runtime_error(
                "nut_source: audio stream declares no sample rate" +
                std::string(hint));
        }
        if (cp->sample_rate > MAX_AUDIO_RATE) {
            std::ostringstream os;
            os << "nut_source: audio rate " << cp->sample_rate
               << " Hz exceeds the built-in cap of " << MAX_AUDIO_RATE
               << " Hz (output buffers are sized for 1 s at the cap before the "
                  "headers are readable)"
               << hint;
            throw std::runtime_error(os.str());
        }
        d_rate = cp->sample_rate;
        adopted << "audio " << d_rate << " Hz x" << d_nchan << "ch";
    }

    if (d_emit_video) {
        const AVCodecParameters* cp = d_fmt->streams[d_video_idx]->codecpar;
        if (cp->codec_id != AV_CODEC_ID_RAWVIDEO || cp->format != AV_PIX_FMT_RGB24) {
            std::ostringstream os;
            os << "nut_source: expected rawvideo/rgb24 video, stream has "
               << codec_name(cp->codec_id) << "/" << pix_fmt_name(cp->format)
               << " — fix the ffmpeg -c:v/-pix_fmt options" << hint;
            throw std::runtime_error(os.str());
        }
        if (cp->width <= 0 || cp->height <= 0) {
            throw std::runtime_error(
                "nut_source: video stream declares no geometry" + std::string(hint));
        }
        const size_t frame_bytes = size_t(cp->width) * cp->height * 3;
        if (frame_bytes > MAX_FRAME_BYTES) {
            std::ostringstream os;
            os << "nut_source: video frame " << cp->width << "x" << cp->height
               << " rgb24 = " << frame_bytes
               << " bytes exceeds the built-in cap of one 1920x1080 frame ("
               << MAX_FRAME_BYTES
               << " bytes); output buffers hold 4 cap-sized frames and are "
                  "allocated before the headers are readable — scale the video "
                  "down in the ffmpeg command"
               << hint;
            throw std::runtime_error(os.str());
        }
        d_width = cp->width;
        d_height = cp->height;
        d_frame_bytes = frame_bytes;
        if (d_nchan > 0)
            adopted << ", ";
        adopted << "video " << d_width << "x" << d_height << " rgb24";
        const AVRational fr = d_fmt->streams[d_video_idx]->avg_frame_rate;
        if (fr.num > 0 && fr.den > 0)
            adopted << " @ " << av_q2d(fr) << " fps";
    }

    d_logger->info("adopted: {}", adopted.str());
}

void nut_source_impl::fail(const std::string& msg)
{
    d_logger->error("{}", msg);
    {
        std::lock_guard<std::mutex> lock(d_error_mutex);
        d_error = msg;
    }
    d_failed.store(true);
}

std::string nut_source_impl::last_error() const
{
    std::lock_guard<std::mutex> lock(d_error_mutex);
    return d_error;
}

bool nut_source_impl::start()
{
    d_stop.store(false);
    d_eof = false;
    d_failed.store(false);
    {
        std::lock_guard<std::mutex> lock(d_error_mutex);
        d_error.clear();
    }
    // Re-adopt the format from the (possibly new) stream on every start.
    d_rate = d_width = d_height = 0;
    d_frame_bytes = 0;
    // start() runs inside the block's scheduler thread; an exception here
    // would be swallowed by GR's thread wrapper, killing only this thread
    // while the rest of the flowgraph hangs. Convert failures to a logged
    // ERROR + clean flowgraph termination instead (see header docs).
    try {
        open_input();
    } catch (const std::exception& e) {
        fail(e.what());
    }
    return true;
}

bool nut_source_impl::stop()
{
    // Raise the flag: the work thread may still be inside av_read_frame();
    // the AVIO interrupt callback / read loop will observe it and unblock.
    // In spawn mode also terminate and reap the child here (signals and
    // waitpid only — safe concurrently with the work thread; the resulting
    // pipe EOF wakes a blocked read immediately). fds/contexts are released
    // in the destructor (or on the next start()).
    d_stop.store(true);
    terminate_child(/*quiet=*/true);
    return true;
}

int64_t nut_source_impl::pkt_pts_us(const AVPacket* pkt) const
{
    if (pkt->pts == AV_NOPTS_VALUE)
        return INT64_MIN;
    return av_rescale_q(pkt->pts, d_fmt->streams[pkt->stream_index]->time_base, US_TB);
}

int64_t nut_source_impl::video_frame_dur_us(const AVPacket* pkt) const
{
    const AVStream* st = d_fmt->streams[pkt->stream_index];
    if (pkt->duration > 0)
        return av_rescale_q(pkt->duration, st->time_base, US_TB);
    if (st->avg_frame_rate.num > 0)
        return av_rescale(1000000, st->avg_frame_rate.den, st->avg_frame_rate.num);
    if (st->r_frame_rate.num > 0)
        return av_rescale(1000000, st->r_frame_rate.den, st->r_frame_rate.num);
    return 0;
}

int nut_source_impl::next_packet(AVPacket* pkt)
{
    if (!d_prime_q.empty()) {
        AVPacket* c = d_prime_q.front();
        d_prime_q.pop_front();
        av_packet_move_ref(pkt, c);
        av_packet_free(&c);
        return 0;
    }
    return av_read_frame(d_fmt, pkt);
}

int nut_source_impl::prime()
{
    size_t prime_bytes = 0;
    for (AVPacket* p : d_prime_q)
        prime_bytes += p->size;

    while ((d_nchan > 0 && d_a_first_us == INT64_MIN) ||
           (d_emit_video && d_v_first_us == INT64_MIN)) {
        int ret = av_read_frame(d_fmt, d_rd_pkt);
        if (ret == AVERROR_EXIT)
            return ret;
        if (ret == AVERROR_EOF) {
            d_eof = true;
            break;
        }
        if (ret < 0) {
            d_logger->warn("read error while priming ({}); treating as EOF — did the "
                           "ffmpeg writer die?",
                           averr(ret));
            d_eof = true;
            break;
        }
        const int si = d_rd_pkt->stream_index;
        if (si != d_audio_idx && si != d_video_idx) {
            av_packet_unref(d_rd_pkt);
            throw std::runtime_error(
                "nut_source: a new stream appeared mid-stream — the contract forbids "
                "layout changes; restart with a fixed ffmpeg -map");
        }
        const int64_t p_us = pkt_pts_us(d_rd_pkt);
        if (si == d_audio_idx && d_a_first_us == INT64_MIN)
            d_a_first_us = (p_us == INT64_MIN) ? 0 : p_us;
        if (si == d_video_idx && d_v_first_us == INT64_MIN)
            d_v_first_us = (p_us == INT64_MIN) ? 0 : p_us;

        AVPacket* c = av_packet_alloc();
        if (!c)
            throw std::runtime_error("nut_source: av_packet_alloc failed");
        av_packet_move_ref(c, d_rd_pkt);
        prime_bytes += c->size;
        d_prime_q.push_back(c);
        if (d_prime_q.size() > PRIME_MAX_PACKETS || prime_bytes > PRIME_MAX_BYTES)
            throw std::runtime_error(
                "nut_source: interleave burst too large while waiting for the first "
                "packet of every stream — bound it with ffmpeg "
                "-max_interleave_delta (e.g. 500000)");
    }
    finish_priming();
    return 0;
}

void nut_source_impl::finish_priming()
{
    d_priming = false;
    if (d_a_first_us == INT64_MIN || d_v_first_us == INT64_MIN)
        return; // EOF before both streams appeared; nothing to trim
    const int64_t start = std::max(d_a_first_us, d_v_first_us);
    if (start > d_a_first_us)
        d_a_skip_frames = av_rescale(start - d_a_first_us, d_rate, 1000000);
    d_v_skip_before_us = start;
    d_logger->info("initial pts: audio {} us, video {} us -> common start {} us "
                   "(trimming {} audio sample(s); dropping video frames before it)",
                   d_a_first_us,
                   d_v_first_us,
                   start,
                   d_a_skip_frames);
}

void nut_source_impl::check_continuity_audio(const AVPacket* pkt)
{
    const int64_t p_us = pkt_pts_us(pkt);
    if (p_us != INT64_MIN && d_a_expect_valid &&
        std::llabs(p_us - d_a_expect_us) > PTS_WARN_US)
        d_logger->warn("audio pts discontinuity: expected {} us, got {} us (delta {} "
                       "us) — upstream ffmpeg should conform with aresample=async=1",
                       d_a_expect_us,
                       p_us,
                       p_us - d_a_expect_us);
    const int64_t nframes = pkt->size / (sizeof(float) * d_nchan);
    const int64_t base =
        (p_us != INT64_MIN) ? p_us : (d_a_expect_valid ? d_a_expect_us : 0);
    d_a_expect_us = base + av_rescale(nframes, 1000000, d_rate);
    d_a_expect_valid = true;
}

void nut_source_impl::check_continuity_video(const AVPacket* pkt)
{
    const int64_t p_us = pkt_pts_us(pkt);
    if (p_us != INT64_MIN && d_v_expect_valid &&
        std::llabs(p_us - d_v_expect_us) > PTS_WARN_US)
        d_logger->warn("video pts discontinuity: expected {} us, got {} us (delta {} "
                       "us) — upstream ffmpeg should conform with -fps_mode cfr",
                       d_v_expect_us,
                       p_us,
                       p_us - d_v_expect_us);
    const int64_t dur = video_frame_dur_us(pkt);
    if (dur > 0) {
        const int64_t base =
            (p_us != INT64_MIN) ? p_us : (d_v_expect_valid ? d_v_expect_us : 0);
        d_v_expect_us = base + dur;
        d_v_expect_valid = true;
    } else {
        d_v_expect_valid = false;
    }
}

void nut_source_impl::stage_packet(AVPacket* pkt)
{
    const int si = pkt->stream_index;
    if (si == d_audio_idx) {
        const size_t bpf = sizeof(float) * d_nchan;
        if (pkt->size % bpf != 0) {
            av_packet_unref(pkt);
            std::ostringstream os;
            os << "nut_source: audio packet size not a multiple of "
               << (sizeof(float) * d_nchan) << " bytes (" << d_nchan
               << " ch pcm_f32le) — mid-stream codec/layout change is forbidden";
            throw std::runtime_error(os.str());
        }
        check_continuity_audio(pkt);
        av_packet_move_ref(d_a_pkt, pkt);
        d_a_staged = true;
        d_a_off = 0;
    } else if (si == d_video_idx) {
        if (static_cast<size_t>(pkt->size) != d_frame_bytes) {
            const int size = pkt->size;
            av_packet_unref(pkt);
            std::ostringstream os;
            os << "nut_source: video packet of " << size << " bytes, expected "
               << d_frame_bytes << " (" << d_width << "x" << d_height
               << " rgb24) — mid-stream geometry/pix_fmt change is forbidden";
            throw std::runtime_error(os.str());
        }
        // Initial pts trim: drop frames earlier than the common start.
        if (d_v_skip_before_us != INT64_MIN) {
            const int64_t p_us = pkt_pts_us(pkt);
            if (p_us != INT64_MIN &&
                p_us + video_frame_dur_us(pkt) / 2 < d_v_skip_before_us) {
                av_packet_unref(pkt);
                return;
            }
            d_v_skip_before_us = INT64_MIN; // first kept frame ends the trim
        }
        check_continuity_video(pkt);
        d_v_staged_pts = (pkt->pts == AV_NOPTS_VALUE) ? 0 : pkt->pts;
        av_packet_move_ref(d_v_pkt, pkt);
        d_v_staged = true;
        d_v_off = 0;
    } else {
        av_packet_unref(pkt);
        throw std::runtime_error(
            "nut_source: a new stream appeared mid-stream — the contract forbids "
            "layout changes; restart with a fixed ffmpeg -map");
    }
}

void nut_source_impl::flush_audio(int noutput_items,
                                  gr_vector_void_star& output_items,
                                  std::vector<int>& produced)
{
    if (!d_a_staged || d_nchan == 0)
        return;
    const size_t bpf = sizeof(float) * d_nchan;
    const size_t total = d_a_pkt->size / bpf;

    if (d_a_skip_frames > 0) {
        const size_t s = std::min<size_t>(d_a_skip_frames, total - d_a_off);
        d_a_off += s;
        d_a_skip_frames -= s;
    }
    const size_t avail = total - d_a_off;
    if (avail == 0) {
        av_packet_unref(d_a_pkt);
        d_a_staged = false;
        d_a_off = 0;
        return;
    }
    const size_t space = static_cast<size_t>(noutput_items - produced[0]);
    const size_t n = std::min(avail, space);
    if (n == 0)
        return;
    const uint8_t* base = d_a_pkt->data + d_a_off * bpf;
    for (int ch = 0; ch < d_nchan; ch++) {
        float* dst = static_cast<float*>(output_items[ch]) + produced[ch];
        const uint8_t* src = base + ch * sizeof(float);
        for (size_t i = 0; i < n; i++)
            std::memcpy(&dst[i], src + i * bpf, sizeof(float));
        produced[ch] += static_cast<int>(n);
    }
    d_a_off += n;
    if (d_a_off >= total) {
        av_packet_unref(d_a_pkt);
        d_a_staged = false;
        d_a_off = 0;
    }
}

void nut_source_impl::flush_video(int noutput_items,
                                  gr_vector_void_star& output_items,
                                  std::vector<int>& produced)
{
    if (!d_v_staged || !d_emit_video)
        return;
    const size_t avail = d_frame_bytes - d_v_off;
    const size_t space = static_cast<size_t>(noutput_items - produced[d_video_port]);
    const size_t n = std::min(avail, space);
    if (n == 0)
        return;
    if (d_v_off == 0) {
        // One tag per frame, on the frame's first byte.
        const uint64_t off = nitems_written(d_video_port) + produced[d_video_port];
        add_item_tag(d_video_port, off, pmt::mp("pts"),
                     pmt::from_long(static_cast<long>(d_v_staged_pts)));
        add_item_tag(d_video_port, off, pmt::mp("width"), pmt::from_long(d_width));
        add_item_tag(d_video_port, off, pmt::mp("height"), pmt::from_long(d_height));
    }
    uint8_t* dst =
        static_cast<uint8_t*>(output_items[d_video_port]) + produced[d_video_port];
    std::memcpy(dst, d_v_pkt->data + d_v_off, n);
    produced[d_video_port] += static_cast<int>(n);
    d_v_off += n;
    if (d_v_off >= d_frame_bytes) {
        av_packet_unref(d_v_pkt);
        d_v_staged = false;
        d_v_off = 0;
    }
}

void nut_source_impl::forecast(int noutput_items, gr_vector_int& ninput_items_required)
{
    // Source block: no inputs.
    (void)noutput_items;
    (void)ninput_items_required;
}

int nut_source_impl::general_work(int noutput_items,
                                  gr_vector_int& ninput_items,
                                  gr_vector_const_void_star& input_items,
                                  gr_vector_void_star& output_items)
{
    (void)ninput_items;
    (void)input_items;

    if (d_failed.load())
        return WORK_DONE; // fatal error latched (e.g. in start()); stop cleanly

    std::vector<int> produced(d_nports, 0);
    const auto produced_any = [&produced] {
        for (int p : produced)
            if (p > 0)
                return true;
        return false;
    };

    bool done = false;
    try {
    while (true) {
        // 1. Drain the staging areas into the output buffers.
        flush_audio(noutput_items, output_items, produced);
        flush_video(noutput_items, output_items, produced);
        if (d_a_staged || d_v_staged)
            break; // an output buffer is full; resume on the next call

        // 2. Staging is empty: decide whether to read more.
        if (interrupted()) {
            done = true;
            break;
        }
        if (d_eof) {
            if (d_spawn)
                // Reap promptly; a nonzero exit status is logged distinctly
                // (EOF caused by a failing child, not by end of media).
                terminate_child(/*quiet=*/false);
            done = true;
            break;
        }
        if (produced_any())
            break; // deliver before risking a blocking read

        if (d_priming) {
            if (prime() == AVERROR_EXIT) {
                done = true;
                break;
            }
            continue;
        }

        int ret = next_packet(d_rd_pkt);
        if (ret == 0) {
            stage_packet(d_rd_pkt); // throws on mid-stream contract violations
            continue;
        }
        if (ret == AVERROR_EXIT) {
            done = true;
            break;
        }
        if (ret != AVERROR_EOF)
            d_logger->warn("read error ({}); treating as EOF — did the ffmpeg writer "
                           "die?",
                           averr(ret));
        d_eof = true;
    }
    } catch (const std::exception& e) {
        // Mid-stream contract violation (layout/geometry change, oversized
        // interleave burst, ...). Same conversion as in start(): log ERROR,
        // latch, terminate the flowgraph cleanly.
        fail(e.what());
        return WORK_DONE;
    }

    if (produced_any()) {
        for (int i = 0; i < d_nports; i++)
            produce(i, produced[i]);
        return WORK_CALLED_PRODUCE;
    }
    return done ? WORK_DONE : WORK_CALLED_PRODUCE;
}

} /* namespace nut */
} /* namespace gr */
