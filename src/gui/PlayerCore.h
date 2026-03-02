#pragma once

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <SDL.h>

extern "C" {
#include <sonic.h>
}

namespace avsync {
namespace gui {

// ---------------------------------------------------------------------------
// Clock – faithful port of ffplay's Clock for AV sync
// ---------------------------------------------------------------------------
// Simplified Clock — no speed field.
// Speed is handled by changing SDL audio output sample rate.
// The audio callback naturally advances audio_clock at the right pace.
struct Clock {
    double pts        = NAN;   // clock base
    double pts_drift  = 0.0;   // pts minus wall-clock at last update
    double last_updated = 0.0;
    int    serial     = 0;
    int    paused     = 0;
    int*   queue_serial = nullptr;

    void Init(int* qs) {
        queue_serial = qs;
        Set(NAN, -1);
    }

    void Set(double p, int ser) {
        double time = av_gettime_relative() / 1000000.0;
        SetAt(p, ser, time);
    }

    void SetAt(double p, int ser, double time) {
        pts = p;
        last_updated = time;
        pts_drift = pts - time;
        serial = ser;
    }

    double Get() const {
        if (queue_serial && *queue_serial != serial)
            return NAN;
        if (paused)
            return pts;
        double time = av_gettime_relative() / 1000000.0;
        return pts_drift + time;
    }
};

// ---------------------------------------------------------------------------
// Thread-safe decoded frame buffer (non-blocking Peek for audio callback)
// ---------------------------------------------------------------------------
struct DecodedFrame {
    AVFrame* frame = nullptr;
    double   pts   = 0.0;
    double   duration = 0.0;
    int      serial = 0;

    DecodedFrame() = default;
    ~DecodedFrame() { if (frame) av_frame_free(&frame); }

    // Move only
    DecodedFrame(DecodedFrame&& o) noexcept
        : frame(o.frame), pts(o.pts), duration(o.duration), serial(o.serial)
    { o.frame = nullptr; }
    DecodedFrame& operator=(DecodedFrame&& o) noexcept {
        if (this != &o) {
            if (frame) av_frame_free(&frame);
            frame = o.frame; pts = o.pts;
            duration = o.duration; serial = o.serial;
            o.frame = nullptr;
        }
        return *this;
    }
    DecodedFrame(const DecodedFrame&) = delete;
    DecodedFrame& operator=(const DecodedFrame&) = delete;
};

template<int CAPACITY>
class FrameQueue {
public:
    // Blocking push (producer waits if full)
    bool Push(DecodedFrame&& f) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_not_full_.wait(lk, [&]{ return size_ < CAPACITY || abort_; });
        if (abort_) return false;
        buf_[windex_] = std::move(f);
        windex_ = (windex_ + 1) % CAPACITY;
        ++size_;
        cv_not_empty_.notify_one();
        return true;
    }

    // Blocking peek (waits until frame available or abort)
    bool PeekBlocking(DecodedFrame*& out) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_not_empty_.wait(lk, [&]{ return size_ > 0 || abort_; });
        if (abort_ && size_ == 0) return false;
        out = &buf_[rindex_];
        return true;
    }

    // Non-blocking peek (returns false immediately if empty)
    bool PeekNonBlocking(DecodedFrame*& out) {
        std::lock_guard<std::mutex> lk(mu_);
        if (size_ == 0) return false;
        out = &buf_[rindex_];
        return true;
    }

    // Number of readable frames
    int Readable() {
        std::lock_guard<std::mutex> lk(mu_);
        return size_;
    }

    void Pop() {
        std::lock_guard<std::mutex> lk(mu_);
        if (size_ > 0) {
            buf_[rindex_] = DecodedFrame();  // free the frame
            rindex_ = (rindex_ + 1) % CAPACITY;
            --size_;
            cv_not_full_.notify_one();
        }
    }

    void Abort() {
        std::lock_guard<std::mutex> lk(mu_);
        abort_ = true;
        cv_not_empty_.notify_all();
        cv_not_full_.notify_all();
    }

    void Flush() {
        std::lock_guard<std::mutex> lk(mu_);
        for (int i = 0; i < CAPACITY; ++i)
            buf_[i] = DecodedFrame();
        rindex_ = windex_ = size_ = 0;
        // Wake producers that may be blocked in Push() waiting for space.
        // Without this, decode threads stall after seek.
        cv_not_full_.notify_all();
    }

    void Reset() {
        std::lock_guard<std::mutex> lk(mu_);
        for (int i = 0; i < CAPACITY; ++i)
            buf_[i] = DecodedFrame();
        rindex_ = windex_ = size_ = 0;
        abort_ = false;
        cv_not_full_.notify_all();
    }

private:
    DecodedFrame buf_[CAPACITY];
    int rindex_ = 0, windex_ = 0, size_ = 0;
    bool abort_ = false;
    std::mutex mu_;
    std::condition_variable cv_not_empty_, cv_not_full_;
};

// ---------------------------------------------------------------------------
// PlayerCore – the media playback engine
// ---------------------------------------------------------------------------
class PlayerCore {
public:
    PlayerCore();
    ~PlayerCore();

    // Open a media file. Returns true on success.
    bool Open(const std::string& path);

    // Close and release all resources.
    void Close();

    // Playback controls
    void Play();
    void Pause();
    void TogglePause();
    bool IsPaused() const { return paused_; }
    bool IsPlaying() const { return playing_; }

    // Seek to position in seconds
    void Seek(double position_sec);

    // Playback speed (0.1x to 4.0x)
    void SetSpeed(double speed);
    double GetSpeed() const { return play_speed_; }

    // AV offset adjustment in milliseconds (positive = audio ahead)
    // This is applied in real-time during playback without re-encoding.
    void SetOffsetMs(double offset_ms);
    double GetOffsetMs() const { return offset_ms_; }

    // Video frame refresh – call from main loop.
    // Returns remaining time (seconds) before next refresh needed.
    // Updates the SDL texture with the current video frame.
    double VideoRefresh(SDL_Renderer* renderer, SDL_Texture** texture,
                        int* tex_w, int* tex_h);

    // Fill audio buffer – called from SDL audio callback.
    void AudioCallback(uint8_t* stream, int len);

    // Media info
    double GetDuration() const;
    double GetCurrentTime() const;
    int GetVideoWidth() const { return video_width_; }
    int GetVideoHeight() const { return video_height_; }
    double GetVideoFps() const { return video_fps_; }
    int GetAudioSampleRate() const { return audio_sample_rate_; }
    bool HasVideo() const { return video_stream_idx_ >= 0; }
    bool HasAudio() const { return audio_stream_idx_ >= 0; }
    const std::string& GetFilePath() const { return file_path_; }

private:
    // Thread functions
    void DemuxThread();
    void VideoDecodeThread();
    void AudioDecodeThread();

    // Audio decode frame – fills audio_buf_ with data, returns size or -1.
    // Modeled after ffplay's audio_decode_frame().
    int AudioDecodeFrame();

    // Internal helpers
    void FlushQueues();

    // Format context and codecs
    AVFormatContext* fmt_ctx_       = nullptr;
    AVCodecContext*  video_dec_ctx_ = nullptr;
    AVCodecContext*  audio_dec_ctx_ = nullptr;
    int video_stream_idx_ = -1;
    int audio_stream_idx_ = -1;

    // Video conversion
    SwsContext*    sws_ctx_     = nullptr;
    int            video_width_ = 0;
    int            video_height_ = 0;
    double         video_fps_   = 25.0;

    // Audio resampling
    SwrContext*    swr_ctx_     = nullptr;
    int            audio_sample_rate_ = 44100;
    int            audio_channels_    = 2;

    // SDL audio
    SDL_AudioDeviceID audio_dev_ = 0;
    int audio_hw_buf_size_       = 0;

    // Sonic time-stretch (speed change without pitch shift)
    sonicStream sonic_stream_     = nullptr;
    std::vector<int16_t> sonic_out_buf_;   // buffer for sonic output
    uint8_t* sonic_out_ptr_       = nullptr;  // current read position in sonic output
    int sonic_out_remaining_      = 0;        // remaining bytes in sonic output

    // Packet queues (thread-safe)
    struct PacketEntry {
        AVPacket* pkt = nullptr;
        int serial = 0;
    };

    class PacketQueue {
    public:
        void Push(AVPacket* pkt, int serial);
        bool Pop(AVPacket* pkt, int& serial);
        void Flush();
        void Abort();
        void Reset();
        int Size();
        int serial = 0;
    private:
        std::deque<PacketEntry> queue_;
        std::mutex mu_;
        std::condition_variable cv_;
        bool abort_ = false;
    };

    PacketQueue video_pkt_q_;
    PacketQueue audio_pkt_q_;

    // Decoded frame queues
    static constexpr int VIDEO_QUEUE_SIZE = 8;
    static constexpr int AUDIO_QUEUE_SIZE = 24;
    FrameQueue<VIDEO_QUEUE_SIZE> video_fq_;
    FrameQueue<AUDIO_QUEUE_SIZE> audio_fq_;

    // Clocks (modeled after ffplay)
    Clock audclk_;
    Clock vidclk_;
    int   audio_serial_ = 0;
    int   video_serial_ = 0;

    // Audio playback buffer – owned copy of data (like ffplay's audio_buf1)
    uint8_t*      audio_buf_       = nullptr;  // points to audio_buf1_
    uint8_t*      audio_buf1_      = nullptr;  // owned buffer
    unsigned int  audio_buf1_size_ = 0;        // allocated size
    unsigned int  audio_buf_size_  = 0;        // valid data size
    unsigned int  audio_buf_index_ = 0;        // read position
    double        audio_clock_     = NAN;
    int           audio_clock_serial_ = -1;

    // Video display state
    double frame_timer_    = 0.0;
    double last_video_pts_ = NAN;     // PTS of last displayed frame (NAN = first frame)

    // Track serial per decode thread for flush-on-seek
    int last_video_serial_ = 0;
    int last_audio_serial_ = 0;

    // After seek, video decode fast-forward: skip frames far before target
    double video_seek_skip_target_ = 0.0;
    bool   video_seek_skipping_ = false;

    // Threading
    std::thread demux_thread_;
    std::thread video_dec_thread_;
    std::thread audio_dec_thread_;
    std::atomic<bool> abort_{false};

    // State
    std::string file_path_;
    std::atomic<bool> playing_{false};
    std::atomic<bool> paused_{false};
    std::atomic<double> play_speed_{1.0};
    std::atomic<double> offset_ms_{0.0};

    // EOF tracking
    std::atomic<bool> eof_reached_{false};

    // Seek
    std::atomic<bool> seek_req_{false};
    std::atomic<double> seek_target_{0.0};
};

}  // namespace gui
}  // namespace avsync
