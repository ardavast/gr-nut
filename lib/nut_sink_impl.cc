/* -*- c++ -*- */
/*
 * Copyright 2026 Ardavast Dayleryan.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nut_sink_impl.h"
#include <gnuradio/block_detail.h>
#include <gnuradio/buffer_reader.h>
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
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
}

namespace gr {
namespace nut {

namespace {

constexpr int AVIO_BUF_SIZE = 32768;
constexpr int POLL_INTERVAL_MS = 100; // deadline poll period while blocked
// Interleave-queue bound (our knob; the mirror of the source's advice to
// always pass -max_interleave_delta to the writing ffmpeg). Verified: the
// queue never blocks and never errors on skewed feeding — it buffers up
// to this much media time and force-flushes beyond it, so it is both the
// reorder buffer and the anti-deadlock cushion.
constexpr int64_t MAX_INTERLEAVE_DELTA_US = 1000000; // 1 s
// Quiet window of the shutdown salvage drain (see drain_inputs): how long
// all inputs must stay empty before the backlog is considered fully
// delivered. Upstream blocks hand over their tails at CPU speed, so this
// only needs to cover scheduling hiccups.
constexpr int SALVAGE_QUIET_MS = 250;

std::string averr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

// Block SIGPIPE for the calling thread: a write into a pipe whose reader
// died must surface as EPIPE (which the error model turns into a proper
// ERROR + clean termination), not as a process-killing signal — pure-C++
// GR hosts do not ignore SIGPIPE the way Python does.
void suppress_sigpipe()
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

// Consume a pending (blocked) SIGPIPE so it cannot fire later if the
// thread's mask is ever restored.
void drain_sigpipe()
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    struct timespec ts = { 0, 0 };
    while (sigtimedwait(&set, nullptr, &ts) > 0)
        ;
}

// Validates the port-defining parameters and builds the input signature
// (N float audio ports, then one byte port per video stream).
std::vector<int> in_sizes(int audio_channels, int video_streams)
{
    if (audio_channels < 0)
        throw std::invalid_argument("nut_sink: audio_channels must be >= 0");
    if (video_streams < 0)
        throw std::invalid_argument("nut_sink: video_streams must be >= 0");
    if (audio_channels == 0 && video_streams == 0)
        throw std::invalid_argument(
            "nut_sink: block would have no inputs — set audio_channels > 0 and/or "
            "video_streams > 0");
    std::vector<int> sizes(audio_channels, sizeof(float));
    sizes.insert(sizes.end(), video_streams, sizeof(unsigned char));
    return sizes;
}

// Broadcast rule: a per-video-stream vector must have length
// video_streams, or length 1 (replicated to every stream).
template <typename T>
std::vector<T> broadcast(const std::vector<T>& v, int video_streams, const char* name)
{
    if (video_streams == 0) {
        if (!v.empty())
            throw std::invalid_argument(std::string("nut_sink: ") + name +
                                        " given but video_streams is 0");
        return {};
    }
    if (static_cast<int>(v.size()) == video_streams)
        return v;
    if (v.size() == 1)
        return std::vector<T>(video_streams, v[0]);
    std::ostringstream os;
    os << "nut_sink: " << name << " must have length video_streams (" << video_streams
       << ") or length 1 (applied to every stream), got length " << v.size();
    throw std::invalid_argument(os.str());
}

// Strict fps parser: ^[0-9]+(/[0-9]+)?$ with a nonzero denominator.
// Decimals are rejected with the exact-rational alternative spelled out —
// 29.97 is NOT 30000/1001, and the rational discipline (docs/clocking.md
// §2) forbids the approximation.
AVRational parse_fps(const std::string& s, int stream)
{
    const auto reject = [&](const std::string& why) {
        std::ostringstream os;
        os << "nut_sink: fps '" << s << "' for video stream " << stream << " is "
           << why;
        throw std::invalid_argument(os.str());
    };

    if (s.find('.') != std::string::npos) {
        // Actionable decimal rejection: name the exact rational for the
        // NTSC family, generic advice otherwise.
        const char* exact = nullptr;
        if (s == "23.976" || s == "23.98")
            exact = "24000/1001";
        else if (s == "29.97")
            exact = "30000/1001";
        else if (s == "59.94")
            exact = "60000/1001";
        std::ostringstream os;
        os << "not an exact rational — decimals are rejected; ";
        if (exact)
            os << s << " is not exact; use " << exact;
        else
            os << "spell the frame rate as an integer or as num/den (e.g. 29.97 "
                  "is not exact; use 30000/1001)";
        reject(os.str());
    }

    const size_t slash = s.find('/');
    const std::string num_s = s.substr(0, slash);
    const std::string den_s =
        (slash == std::string::npos) ? std::string("1") : s.substr(slash + 1);
    const auto all_digits = [](const std::string& t) {
        return !t.empty() &&
               std::all_of(t.begin(), t.end(), [](unsigned char c) {
                   return c >= '0' && c <= '9';
               });
    };
    if (!all_digits(num_s) || !all_digits(den_s) ||
        den_s.find('/') != std::string::npos)
        reject("not a valid frame rate — use an integer (\"25\") or an exact "
               "rational (\"30000/1001\")");

    long long num = 0, den = 0;
    try {
        num = std::stoll(num_s);
        den = std::stoll(den_s);
    } catch (const std::exception&) {
        reject("out of range");
    }
    if (den == 0)
        reject("a division by zero — the denominator must be nonzero");
    if (num == 0)
        reject("zero — the frame rate must be positive");
    if (num > INT32_MAX || den > INT32_MAX)
        reject("out of range");
    return AVRational{ static_cast<int>(num), static_cast<int>(den) };
}

} // namespace

nut_sink::sptr nut_sink::make(const std::string& uri,
                              int audio_channels,
                              int audio_rate,
                              int video_streams,
                              const std::vector<int>& widths,
                              const std::vector<int>& heights,
                              const std::vector<std::string>& fps,
                              const std::string& command,
                              double flush_timeout)
{
    return gnuradio::make_block_sptr<nut_sink_impl>(uri,
                                                    audio_channels,
                                                    audio_rate,
                                                    video_streams,
                                                    widths,
                                                    heights,
                                                    fps,
                                                    command,
                                                    flush_timeout);
}

nut_sink_impl::nut_sink_impl(const std::string& uri,
                             int audio_channels,
                             int audio_rate,
                             int video_streams,
                             const std::vector<int>& widths,
                             const std::vector<int>& heights,
                             const std::vector<std::string>& fps,
                             const std::string& command,
                             double flush_timeout)
    : gr::block("nut_sink",
                gr::io_signature::makev(audio_channels + video_streams,
                                        audio_channels + video_streams,
                                        in_sizes(audio_channels, video_streams)),
                gr::io_signature::make(0, 0, 0)),
      d_uri(uri),
      d_nchan(audio_channels),
      d_rate(audio_rate),
      d_nvideo(video_streams),
      d_command(command),
      d_spawn(uri.empty()),
      d_nports(audio_channels + video_streams),
      d_flush_timeout(flush_timeout),
      d_video(video_streams)
{
    if (uri.empty() && command.empty())
        throw std::invalid_argument(
            "nut_sink: no output given — set uri (path of a FIFO/file to write "
            "the NUT stream to) or command (a shell command reading NUT on "
            "stdin, e.g. 'ffmpeg -y -i pipe:0 out.flac', to be spawned by the "
            "block)");
    if (!uri.empty() && !command.empty())
        throw std::invalid_argument(
            "nut_sink: uri and command are mutually exclusive — set uri to write "
            "a FIFO/file for an external NUT reader, or leave it empty and set "
            "command to have the block spawn the reader itself");
    if (d_nchan > 0 && d_rate <= 0)
        throw std::invalid_argument(
            "nut_sink: audio_rate must be > 0 when audio_channels > 0 — the sink "
            "writes the stream headers, so the rate must be declared");
    if (!(flush_timeout >= 0.0))
        throw std::invalid_argument("nut_sink: flush_timeout must be >= 0");

    const std::vector<int> w = broadcast(widths, d_nvideo, "widths");
    const std::vector<int> h = broadcast(heights, d_nvideo, "heights");
    const std::vector<std::string> f = broadcast(fps, d_nvideo, "fps");
    for (int v = 0; v < d_nvideo; v++) {
        if (w[v] <= 0 || h[v] <= 0) {
            std::ostringstream os;
            os << "nut_sink: video stream " << v << " geometry " << w[v] << "x"
               << h[v] << " is invalid — width and height must be > 0";
            throw std::invalid_argument(os.str());
        }
        video_state& vs = d_video[v];
        vs.width = w[v];
        vs.height = h[v];
        vs.fps = parse_fps(f[v], v);
        vs.frame_bytes = size_t(w[v]) * h[v] * 3;
        vs.stage.resize(vs.frame_bytes);
    }
}

nut_sink_impl::~nut_sink_impl()
{
    finalize(); // last resort; normally stop() already ran it
    close_output();
}

// ---------------------------------------------------------------- deadline

void nut_sink_impl::arm_deadline()
{
    if (d_deadline_set.load(std::memory_order_acquire))
        return;
    d_deadline = std::chrono::steady_clock::now() +
                 std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                     std::chrono::duration<double>(d_flush_timeout));
    d_deadline_set.store(true, std::memory_order_release);
}

bool nut_sink_impl::deadline_expired() const
{
    return d_deadline_set.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() > d_deadline;
}

int nut_sink_impl::deadline_wait_ms(int cap) const
{
    if (!d_deadline_set.load(std::memory_order_acquire))
        return cap;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          d_deadline - std::chrono::steady_clock::now())
                          .count();
    return std::max(0, std::min<int>(cap, static_cast<int>(left)));
}

// ------------------------------------------------------------------- avio

// Custom AVIO write: non-blocking fd + poll loop. During a normal run a
// full pipe simply blocks us — that is the backpressure that paces the
// flowgraph in K2-style chains. Once the flush deadline is armed (stop
// requested / finalize entered), the wait is bounded by it. A vanished
// reader is EPIPE: an ERROR (the recording is lost), never an EOF.
#if LIBAVFORMAT_VERSION_MAJOR >= 61
int nut_sink_impl::write_cb(void* opaque, const uint8_t* buf, int buf_size)
#else
int nut_sink_impl::write_cb(void* opaque, uint8_t* buf, int buf_size)
#endif
{
    return static_cast<nut_sink_impl*>(opaque)->write_bytes(buf, buf_size);
}

int nut_sink_impl::write_bytes(const uint8_t* buf, int buf_size)
{
    suppress_sigpipe();
    int written = 0;
    while (written < buf_size) {
        if (d_abort.load(std::memory_order_relaxed))
            return AVERROR_EXIT;
        if (deadline_expired()) {
            d_abort.store(true);
            return AVERROR_EXIT;
        }
        const ssize_t n = ::write(d_fd, buf + written, buf_size - written);
        if (n > 0) {
            written += static_cast<int>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Blocked on backpressure. If a stop has been requested
            // (tb.stop() raises the boost interruption flag), arm the
            // flush deadline so the wait — and everything after it — is
            // bounded by flush_timeout instead of hanging forever on a
            // stalled reader.
            if (boost::this_thread::interruption_requested())
                arm_deadline();
            struct pollfd p = { d_fd, POLLOUT, 0 };
            ::poll(&p, 1, deadline_wait_ms(POLL_INTERVAL_MS));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EPIPE) {
            drain_sigpipe();
            return AVERROR(EPIPE);
        }
        return AVERROR(errno ? errno : EIO);
    }
    return buf_size;
}

int64_t nut_sink_impl::seek_cb(void* opaque, int64_t offset, int whence)
{
    auto* self = static_cast<nut_sink_impl*>(opaque);
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

// ------------------------------------------------------------------ spawn

// Spawn mode (POSIX-only): run the user-authored command via /bin/sh -c
// and feed the NUT stream to an anonymous pipe on its STDIN. stdout and
// stderr are inherited so the reader's diagnostics stay visible. Same
// trust model as the source's spawn mode: the command carries the trust
// level of a shell script the user would write anyway.
void nut_sink_impl::spawn_child()
{
    d_logger->info("spawning: /bin/sh -c \"{}\"", d_command);

    int in_pipe[2];  // our writer -> child's stdin
    int err_pipe[2]; // exec-failure reporting (CLOEXEC survives on success)
    if (pipe2(in_pipe, O_CLOEXEC) != 0)
        throw std::runtime_error(std::string("nut_sink: pipe2 failed: ") +
                                 std::strerror(errno));
    if (pipe2(err_pipe, O_CLOEXEC) != 0) {
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        throw std::runtime_error(std::string("nut_sink: pipe2 failed: ") +
                                 std::strerror(errno));
    }

    const pid_t pid = fork();
    if (pid < 0) {
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        throw std::runtime_error(std::string("nut_sink: fork failed: ") +
                                 std::strerror(errno));
    }
    if (pid == 0) {
        // Child. Own process group so shutdown can signal the whole
        // pipeline the shell may create. stdout/stderr are inherited.
        setpgid(0, 0);
        if (dup2(in_pipe[0], STDIN_FILENO) < 0)
            _exit(127);
        execl("/bin/sh", "sh", "-c", d_command.c_str(), (char*)nullptr);
        const int e = errno;
        ssize_t n = ::write(err_pipe[1], &e, sizeof(e));
        (void)n;
        _exit(127);
    }

    // Parent. Also set the pgid here to close the fork/kill race.
    setpgid(pid, pid);
    ::close(in_pipe[0]);
    ::close(err_pipe[1]);
    int exec_errno = 0;
    const ssize_t n = ::read(err_pipe[0], &exec_errno, sizeof(exec_errno));
    ::close(err_pipe[0]);
    if (n > 0) {
        // exec of /bin/sh failed; the child has already exited.
        int status = 0;
        waitpid(pid, &status, 0);
        ::close(in_pipe[1]);
        throw std::runtime_error(std::string("nut_sink: cannot exec /bin/sh: ") +
                                 std::strerror(exec_errno));
    }

    fcntl(in_pipe[1], F_SETFL, O_NONBLOCK);
    d_fd = in_pipe[1];
    {
        std::lock_guard<std::mutex> lock(d_child_mutex);
        d_child = pid;
    }
}

// The mirror of the source's terminate_child, with the polarity flipped:
// the child EXITING ON ITS OWN is the desired end state (encoder flushed,
// container finalized); signals are the escalation path, not the default.
// Must be called after the pipe's write end is closed (that is its EOF).
void nut_sink_impl::wait_child_then_escalate() noexcept
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
    const auto log_exit = [&](bool killed) {
        if (killed)
            d_logger->warn("spawned command did not exit within flush_timeout "
                           "({} s) after EOF; killed — the recording may be "
                           "truncated/corrupt",
                           d_flush_timeout);
        else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            d_logger->warn("spawned command exited with status {} — check its "
                           "stderr above (bad path, encoder error, ...)",
                           WEXITSTATUS(status));
    };

    // Phase 1: wait for a voluntary exit until the deadline.
    for (;;) {
        if (waitpid(pid, &status, WNOHANG) == pid) {
            // Exited on its own — the happy path. Sweep any stragglers of
            // its process group (a shell pipeline may have left some).
            ::kill(-pid, SIGTERM);
            log_exit(false);
            return;
        }
        const int wait_ms = deadline_wait_ms(50);
        if (deadline_expired())
            break;
        ::poll(nullptr, 0, std::max(1, std::min(wait_ms, 50)));
    }

    // Phase 2: deadline passed — escalate. Signal the whole process group.
    ::kill(-pid, SIGTERM);
    for (int i = 0; i < 40; i++) { // up to ~2 s of grace
        if (waitpid(pid, &status, WNOHANG) == pid) {
            log_exit(true);
            return;
        }
        ::poll(nullptr, 0, 50);
    }
    ::kill(-pid, SIGKILL);
    waitpid(pid, &status, 0);
    log_exit(true);
}

// ----------------------------------------------------------------- output

void nut_sink_impl::reset_stream_state()
{
    d_a_count = 0;
    d_a_warned_rx_time = false;
    d_a_time_base = { 0, 1 };
    d_audio_idx = -1;
    for (video_state& vs : d_video) {
        vs.count = 0;
        vs.stage_off = 0;
        vs.warned_rx_time = false;
        vs.stream_idx = -1;
        vs.time_base = { 0, 1 };
    }
}

void nut_sink_impl::close_output() noexcept
{
    if (d_oc) {
        // The muxer's private data was released by av_write_trailer if the
        // finalize path ran; avformat_free_context handles both cases.
        avformat_free_context(d_oc);
        d_oc = nullptr;
    }
    if (d_avio) {
        av_freep(&d_avio->buffer);
        avio_context_free(&d_avio);
    }
    if (d_fd >= 0) {
        ::close(d_fd);
        d_fd = -1;
    }
    d_header_written = false;
    reset_stream_state();
}

void nut_sink_impl::open_output()
{
    close_output();
    d_finalized = false;

    if (d_spawn) {
        spawn_child(); // sets d_fd to the pipe's write end; throws on failure
        d_is_fifo = true; // anonymous pipe: FIFO write semantics
    } else {
        // A FIFO must be opened without O_CREAT/O_TRUNC, and O_WRONLY on a
        // FIFO with no reader yet gives ENXIO with O_NONBLOCK — poll until
        // the reader appears (mirrors the source waiting for its writer).
        struct stat st;
        const bool exists = (::stat(d_uri.c_str(), &st) == 0);
        d_is_fifo = exists && S_ISFIFO(st.st_mode);
        if (d_is_fifo) {
            for (;;) {
                d_fd = ::open(d_uri.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
                if (d_fd >= 0)
                    break;
                if (errno != ENXIO)
                    throw std::runtime_error("nut_sink: cannot open '" + d_uri +
                                             "': " + std::strerror(errno));
                if (boost::this_thread::interruption_requested() ||
                    d_abort.load())
                    throw std::runtime_error(
                        "nut_sink: interrupted while waiting for a reader on "
                        "FIFO '" +
                        d_uri + "'");
                ::poll(nullptr, 0, POLL_INTERVAL_MS);
            }
        } else {
            d_fd = ::open(d_uri.c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK | O_CLOEXEC,
                          0644);
            if (d_fd < 0)
                throw std::runtime_error("nut_sink: cannot open '" + d_uri +
                                         "': " + std::strerror(errno));
        }
    }

    auto* iobuf = static_cast<unsigned char*>(av_malloc(AVIO_BUF_SIZE));
    if (!iobuf) {
        close_output();
        throw std::runtime_error("nut_sink: av_malloc failed");
    }
    d_avio = avio_alloc_context(iobuf,
                                AVIO_BUF_SIZE,
                                1 /* write */,
                                this,
                                nullptr,
                                &nut_sink_impl::write_cb,
                                d_is_fifo ? nullptr : &nut_sink_impl::seek_cb);
    if (!d_avio) {
        av_free(iobuf);
        close_output();
        throw std::runtime_error("nut_sink: avio_alloc_context failed");
    }

    int ret = avformat_alloc_output_context2(&d_oc, nullptr, "nut", nullptr);
    if (ret < 0 || !d_oc) {
        close_output();
        throw std::runtime_error(
            "nut_sink: libavformat has no NUT muxer (broken ffmpeg build?)");
    }
    d_oc->pb = d_avio;
    d_oc->flags |= AVFMT_FLAG_CUSTOM_IO;
    // Our knob (mirror of the source's -max_interleave_delta advice): the
    // interleave queue reorders greedy, skewed input and force-flushes
    // beyond this bound, so memory stays bounded and nothing ever blocks.
    d_oc->max_interleave_delta = MAX_INTERLEAVE_DELTA_US;

    std::ostringstream declared;
    if (d_nchan > 0) {
        AVStream* st = avformat_new_stream(d_oc, nullptr);
        if (!st) {
            close_output();
            throw std::runtime_error("nut_sink: avformat_new_stream failed");
        }
        st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        st->codecpar->codec_id = AV_CODEC_ID_PCM_F32LE;
        st->codecpar->sample_rate = d_rate;
        st->codecpar->format = AV_SAMPLE_FMT_FLT;
        av_channel_layout_default(&st->codecpar->ch_layout, d_nchan);
        st->time_base = AVRational{ 1, d_rate };
        d_audio_idx = st->index;
        declared << "audio " << d_rate << " Hz x" << d_nchan << "ch";
    }
    for (int v = 0; v < d_nvideo; v++) {
        video_state& vs = d_video[v];
        AVStream* st = avformat_new_stream(d_oc, nullptr);
        if (!st) {
            close_output();
            throw std::runtime_error("nut_sink: avformat_new_stream failed");
        }
        st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        st->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
        st->codecpar->width = vs.width;
        st->codecpar->height = vs.height;
        st->codecpar->format = AV_PIX_FMT_RGB24;
        st->codecpar->codec_tag = avcodec_pix_fmt_to_codec_tag(AV_PIX_FMT_RGB24);
        st->time_base = av_inv_q(vs.fps);
        // MANDATORY (verified): without avg_frame_rate set before
        // avformat_write_header, NUT demuxes the video packets with no
        // duration and nut_source's continuity watchdog fires per frame.
        st->avg_frame_rate = vs.fps;
        vs.stream_idx = st->index;
        if (declared.tellp() > 0)
            declared << ", ";
        declared << "video" << v << " " << vs.width << "x" << vs.height
                 << " rgb24 @ " << vs.fps.num << "/" << vs.fps.den << " fps";
    }

    const std::string label =
        d_spawn ? "spawned command \"" + d_command + "\"" : "'" + d_uri + "'";
    ret = avformat_write_header(d_oc, nullptr);
    if (ret < 0) {
        const std::string why = averr(ret);
        close_output();
        throw std::runtime_error(
            "nut_sink: cannot write the NUT stream headers to " + label + " (" +
            why + ")" +
            (d_spawn ? " — check your spawn command: it likely failed to start "
                       "or exited immediately (see its stderr above)"
                     : ""));
    }
    d_header_written = true;

    // The NUT muxer overrides the declared stream timebases at
    // write_header (verified: it chose 1/51200 for 25 fps video); all pts
    // must be rescaled into the post-header st->time_base.
    if (d_nchan > 0)
        d_a_time_base = d_oc->streams[d_audio_idx]->time_base;
    for (video_state& vs : d_video)
        vs.time_base = d_oc->streams[vs.stream_idx]->time_base;

    d_logger->info("muxing: {} -> {}", declared.str(), label);
}

// GR's scheduler declares a sink done as soon as ANY input is empty with
// a finished upstream (block_executor's sink path), without ever showing
// work() the backlog still queued on the OTHER inputs — and on tb.stop()
// even a single input can hold unconsumed items. For a recorder that
// backlog is recorded media; drain it straight from the input buffers
// here (stop() runs in the block's own thread after the scheduler loop
// has exited, so the readers are ours alone). Upstream blocks that are
// still finishing keep writing into the buffers while we drain; wait for
// their writers to mark the buffers done, bounded by the deadline.
void nut_sink_impl::drain_inputs()
{
    block_detail* det = detail().get();
    if (!det || static_cast<int>(det->ninputs()) != d_nports)
        return;

    auto last_progress = std::chrono::steady_clock::now();
    for (;;) {
        bool progressed = false;
        if (d_nchan > 0) {
            int n = det->input(0)->items_available();
            for (int ch = 1; ch < d_nchan; ch++)
                n = std::min(n, det->input(ch)->items_available());
            if (n > 0) {
                gr_vector_const_void_star items(d_nchan);
                for (int ch = 0; ch < d_nchan; ch++)
                    items[ch] = det->input(ch)->read_pointer();
                write_audio(items, n);
                for (int ch = 0; ch < d_nchan; ch++)
                    det->input(ch)->update_read_pointer(n);
                progressed = true;
            }
        }
        for (int v = 0; v < d_nvideo; v++) {
            buffer_reader* rd = det->input(d_nchan + v).get();
            const int n = rd->items_available();
            if (n > 0) {
                write_video(v, static_cast<const uint8_t*>(rd->read_pointer()), n);
                rd->update_read_pointer(n);
                progressed = true;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (progressed) {
            last_progress = now;
            continue;
        }
        if (d_abort.load() || deadline_expired())
            break;
        // Nothing available. The buffers' done flags cannot be trusted
        // here: when the scheduler declared THIS block done it marked all
        // its input buffers done too (buffer_reader::set_done writes the
        // shared buffer's flag), poisoning the "upstream writer finished"
        // signal — while upstream sources may still be delivering their
        // tails. Heuristic: if every input reads done, wait out a short
        // quiet window for in-flight data and then finish; if some input
        // is demonstrably still alive (done not set), keep waiting — the
        // deadline bounds everything either way.
        bool all_done = true;
        for (int i = 0; i < d_nports; i++)
            if (!det->input(i)->done())
                all_done = false;
        if (all_done &&
            now - last_progress > std::chrono::milliseconds(SALVAGE_QUIET_MS))
            break;
        ::poll(nullptr, 0, 10);
    }
}

void nut_sink_impl::finalize() noexcept
{
    if (d_finalized)
        return;
    d_finalized = true;
    // Everything below shares one deadline of flush_timeout seconds.
    arm_deadline();

    if (d_oc && d_header_written && !d_abort.load()) {
        try {
            drain_inputs();
        } catch (const std::exception& e) {
            fail(e.what()); // e.g. EPIPE while salvaging; skip the flush
        }
    }

    if (d_oc && d_header_written && !d_abort.load()) {
        // 1. Flush the interleave queue, 2. write the NUT trailer,
        // 3. flush the AVIO buffer. (av_write_trailer also drains the
        // interleave queue; the explicit flush keeps the two failure
        // points distinguishable in the logs.)
        int ret = av_interleaved_write_frame(d_oc, nullptr);
        if (ret < 0)
            d_logger->warn("flush of the interleave queue failed ({}) — the "
                           "recording may be incomplete",
                           averr(ret));
        ret = av_write_trailer(d_oc);
        if (ret < 0)
            d_logger->warn("av_write_trailer failed ({}) — the recording may "
                           "be incomplete",
                           averr(ret));
        avio_flush(d_avio);
    }

    // 4. Close the pipe/FIFO/file: this is the reader's EOF.
    if (d_fd >= 0) {
        ::close(d_fd);
        d_fd = -1;
    }

    // 5. Spawn mode: wait for the child to finish on its own (encoder
    // flushing, container finalization), escalate only past the deadline.
    wait_child_then_escalate();

    // Report dropped partial frames (an input byte count that is not a
    // multiple of the frame size usually means a producer bug).
    for (int v = 0; v < d_nvideo; v++)
        if (d_video[v].stage_off > 0)
            d_logger->warn("video stream {}: dropping a partial trailing frame "
                           "({} of {} bytes) at shutdown",
                           v,
                           d_video[v].stage_off,
                           d_video[v].frame_bytes);
}

// ------------------------------------------------------------ error model

void nut_sink_impl::fail(const std::string& msg)
{
    d_logger->error("{}", msg);
    {
        std::lock_guard<std::mutex> lock(d_error_mutex);
        d_error = msg;
    }
    d_failed.store(true);
}

std::string nut_sink_impl::last_error() const
{
    std::lock_guard<std::mutex> lock(d_error_mutex);
    return d_error;
}

// -------------------------------------------------------------- lifecycle

bool nut_sink_impl::start()
{
    d_failed.store(false);
    d_abort.store(false);
    d_deadline_set.store(false);
    {
        std::lock_guard<std::mutex> lock(d_error_mutex);
        d_error.clear();
    }
    // start() runs inside the block's scheduler thread; an exception here
    // would be swallowed by GR's thread wrapper, killing only this thread
    // while the rest of the flowgraph hangs. Convert failures to a logged
    // ERROR + clean flowgraph termination instead (see header docs).
    try {
        open_output();
    } catch (const std::exception& e) {
        d_abort.store(true); // nothing valid was written; do not flush
        fail(e.what());
    }
    return true;
}

bool nut_sink_impl::stop()
{
    // stop() runs in the block's own work thread after the work loop has
    // exited (verified for GR 3.10; on tb.stop() the boost interruption
    // flag is already raised here) — so there is no concurrency with
    // general_work(), and this is the right place for the shutdown
    // sequence: flush -> trailer -> close (EOF) -> wait for the child ->
    // escalate. The whole sequence is bounded by flush_timeout.
    finalize();
    return true;
}

// ------------------------------------------------------------------ work

void nut_sink_impl::send_packet(AVPacket* pkt, const char* what)
{
    // av_interleaved_write_frame takes ownership of (and unrefs) pkt in
    // all cases. Verified behavior (design doc §7): it never blocks and
    // never errors on interleave skew — errors here are real I/O errors.
    const int ret = av_interleaved_write_frame(d_oc, pkt);
    if (ret >= 0)
        return;
    d_abort.store(true); // the byte stream is broken; do not flush at stop
    std::ostringstream os;
    if (ret == AVERROR(EPIPE))
        os << "nut_sink: the NUT reader is gone (EPIPE while writing " << what
           << ") — "
           << (d_spawn ? "the spawned command exited mid-stream; check its "
                         "stderr above"
                       : "the process reading '" + d_uri + "' died")
           << ". A vanished reader loses the recording, so this is an error, "
              "not an EOF";
    else
        os << "nut_sink: error writing " << what << " (" << averr(ret) << ")";
    throw std::runtime_error(os.str());
}

void nut_sink_impl::check_overflow_tags(int port, int nitems, bool& warned_latch)
{
    if (warned_latch || nitems <= 0)
        return;
    static const pmt::pmt_t RX_TIME = pmt::mp("rx_time");
    std::vector<gr::tag_t> tags;
    get_tags_in_range(tags,
                      port,
                      nitems_read(port),
                      nitems_read(port) + nitems,
                      RX_TIME);
    for (const gr::tag_t& t : tags) {
        if (t.offset == 0)
            continue; // stream start: normal, not an overflow
        warned_latch = true;
        d_logger->warn(
            "input port {}: rx_time tag mid-stream (offset {}) — an upstream "
            "overflow dropped samples. The sink synthesizes pts from sample "
            "counts, so the recorded timeline silently compresses across the "
            "gap; fix the overflow (rate, buffering) upstream",
            port,
            t.offset);
        break;
    }
}

void nut_sink_impl::write_audio(gr_vector_const_void_star& input_items, int n)
{
    AVPacket* pkt = av_packet_alloc();
    if (!pkt)
        throw std::runtime_error("nut_sink: av_packet_alloc failed");
    const size_t bpf = sizeof(float) * d_nchan;
    if (av_new_packet(pkt, static_cast<int>(n * bpf)) < 0) {
        av_packet_free(&pkt);
        throw std::runtime_error("nut_sink: av_new_packet failed");
    }
    // Interleave the per-channel float inputs into one pcm_f32le payload.
    uint8_t* base = pkt->data;
    for (int ch = 0; ch < d_nchan; ch++) {
        const float* src = static_cast<const float*>(input_items[ch]);
        uint8_t* dst = base + ch * sizeof(float);
        for (int i = 0; i < n; i++)
            std::memcpy(dst + i * bpf, &src[i], sizeof(float));
    }
    pkt->stream_index = d_audio_idx;
    // pts synthesis: the running sample count in 1/rate, rescaled into the
    // post-header muxer timebase. Always av_rescale_q — never hand-rolled
    // (intermediate overflow).
    const AVRational atb = { 1, d_rate };
    pkt->pts = pkt->dts = av_rescale_q(d_a_count, atb, d_a_time_base);
    pkt->duration =
        av_rescale_q(d_a_count + n, atb, d_a_time_base) - pkt->pts;
    pkt->flags |= AV_PKT_FLAG_KEY;
    d_a_count += n;
    try {
        send_packet(pkt, "audio");
    } catch (...) {
        av_packet_free(&pkt);
        throw;
    }
    av_packet_free(&pkt);
}

void nut_sink_impl::write_video(int v, const uint8_t* in, int n)
{
    video_state& vs = d_video[v];
    const AVRational vtb = av_inv_q(vs.fps); // frame index timebase
    size_t off = 0;
    while (off < static_cast<size_t>(n)) {
        const size_t take =
            std::min(vs.frame_bytes - vs.stage_off, static_cast<size_t>(n) - off);
        std::memcpy(vs.stage.data() + vs.stage_off, in + off, take);
        vs.stage_off += take;
        off += take;
        if (vs.stage_off < vs.frame_bytes)
            break; // partial frame: stays staged until more bytes arrive

        AVPacket* pkt = av_packet_alloc();
        if (!pkt)
            throw std::runtime_error("nut_sink: av_packet_alloc failed");
        if (av_new_packet(pkt, static_cast<int>(vs.frame_bytes)) < 0) {
            av_packet_free(&pkt);
            throw std::runtime_error("nut_sink: av_new_packet failed");
        }
        std::memcpy(pkt->data, vs.stage.data(), vs.frame_bytes);
        vs.stage_off = 0;
        pkt->stream_index = vs.stream_idx;
        pkt->pts = pkt->dts = av_rescale_q(vs.count, vtb, vs.time_base);
        pkt->duration =
            av_rescale_q(vs.count + 1, vtb, vs.time_base) - pkt->pts;
        pkt->flags |= AV_PKT_FLAG_KEY;
        vs.count++;
        try {
            send_packet(pkt, "video");
        } catch (...) {
            av_packet_free(&pkt);
            throw;
        }
        av_packet_free(&pkt);
    }
}

void nut_sink_impl::forecast(int noutput_items, gr_vector_int& ninput_items_required)
{
    // Greedy consumer: require nothing, take whatever any input has. The
    // interleave queue absorbs and reorders the skew (bounded by
    // max_interleave_delta), so no input can ever starve another.
    (void)noutput_items;
    for (size_t i = 0; i < ninput_items_required.size(); i++)
        ninput_items_required[i] = 0;
}

int nut_sink_impl::general_work(int noutput_items,
                                gr_vector_int& ninput_items,
                                gr_vector_const_void_star& input_items,
                                gr_vector_void_star& output_items)
{
    (void)noutput_items;
    (void)output_items;

    if (d_failed.load())
        return WORK_DONE; // fatal error latched (e.g. in start()); stop cleanly
    if (d_finalized)
        return WORK_DONE; // trailer already written; nothing may follow it

    try {
        if (d_nchan > 0) {
            // The channels of the one audio stream advance in lockstep.
            int n = ninput_items[0];
            for (int ch = 1; ch < d_nchan; ch++)
                n = std::min(n, ninput_items[ch]);
            if (n > 0) {
                check_overflow_tags(0, n, d_a_warned_rx_time);
                write_audio(input_items, n);
                for (int ch = 0; ch < d_nchan; ch++)
                    consume(ch, n);
            }
        }
        for (int v = 0; v < d_nvideo; v++) {
            const int port = d_nchan + v;
            const int n = ninput_items[port];
            if (n > 0) {
                check_overflow_tags(port, n, d_video[v].warned_rx_time);
                write_video(
                    v, static_cast<const uint8_t*>(input_items[port]), n);
                consume(port, n);
            }
        }
    } catch (const std::exception& e) {
        // Write failure (EPIPE, I/O error, allocation). Same conversion as
        // in start(): log ERROR, latch, terminate the flowgraph cleanly.
        fail(e.what());
        return WORK_DONE;
    }
    return 0;
}

} /* namespace nut */
} /* namespace gr */
