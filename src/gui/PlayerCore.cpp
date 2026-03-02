#include "PlayerCore.h"
#include <algorithm>
#include <cstring>
#include <iostream>

extern "C" {
#include <sonic.h>
}

namespace avsync {
namespace gui {

// No complex sync thresholds — simplified for AV sync calibration.
// We display video frames at their original PTS pace, adjusted only
// by offset. Audio is the master clock; speed is handled by SDL
// audio output sample rate.

// ===========================================================================
// PacketQueue implementation
// ===========================================================================
void PlayerCore::PacketQueue::Push(AVPacket* pkt, int ser) {
    std::lock_guard<std::mutex> lk(mu_);
    PacketEntry e;
    e.pkt = av_packet_alloc();
    av_packet_move_ref(e.pkt, pkt);
    e.serial = ser;
    queue_.push_back(e);
    cv_.notify_one();
}

bool PlayerCore::PacketQueue::Pop(AVPacket* pkt, int& ser) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [&]{ return !queue_.empty() || abort_; });
    if (abort_ && queue_.empty()) return false;
    auto& e = queue_.front();
    av_packet_move_ref(pkt, e.pkt);
    av_packet_free(&e.pkt);
    ser = e.serial;
    queue_.pop_front();
    return true;
}

void PlayerCore::PacketQueue::Flush() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& e : queue_) {
        av_packet_free(&e.pkt);
    }
    queue_.clear();
    serial++;
}

void PlayerCore::PacketQueue::Abort() {
    std::lock_guard<std::mutex> lk(mu_);
    abort_ = true;
    cv_.notify_all();
}

void PlayerCore::PacketQueue::Reset() {
    Flush();
    std::lock_guard<std::mutex> lk(mu_);
    abort_ = false;
}

int PlayerCore::PacketQueue::Size() {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int>(queue_.size());
}

// ===========================================================================
// PlayerCore
// ===========================================================================
PlayerCore::PlayerCore() {}

PlayerCore::~PlayerCore() { Close(); }

bool PlayerCore::Open(const std::string& path) {
    Close();

    file_path_ = path;
    abort_ = false;

    // Open input
    fmt_ctx_ = nullptr;
    if (avformat_open_input(&fmt_ctx_, path.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "[PlayerCore] Failed to open: " << path << "\n";
        return false;
    }
    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
        std::cerr << "[PlayerCore] Failed to find stream info\n";
        Close();
        return false;
    }

    // Find best video stream
    video_stream_idx_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_idx_ >= 0) {
        auto* par = fmt_ctx_->streams[video_stream_idx_]->codecpar;
        auto* codec = avcodec_find_decoder(par->codec_id);
        if (codec) {
            video_dec_ctx_ = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(video_dec_ctx_, par);
            video_dec_ctx_->thread_count = 2;
            avcodec_open2(video_dec_ctx_, codec, nullptr);
            video_width_ = par->width;
            video_height_ = par->height;
            auto* st = fmt_ctx_->streams[video_stream_idx_];
            if (st->avg_frame_rate.den > 0 && st->avg_frame_rate.num > 0)
                video_fps_ = av_q2d(st->avg_frame_rate);
            else if (st->r_frame_rate.den > 0 && st->r_frame_rate.num > 0)
                video_fps_ = av_q2d(st->r_frame_rate);
        }
    }

    // Find best audio stream
    audio_stream_idx_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_stream_idx_ >= 0) {
        auto* par = fmt_ctx_->streams[audio_stream_idx_]->codecpar;
        auto* codec = avcodec_find_decoder(par->codec_id);
        if (codec) {
            audio_dec_ctx_ = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(audio_dec_ctx_, par);
            avcodec_open2(audio_dec_ctx_, codec, nullptr);
            audio_sample_rate_ = par->sample_rate;
            audio_channels_ = par->ch_layout.nb_channels;
        }
    }

    if (video_stream_idx_ < 0 && audio_stream_idx_ < 0) {
        std::cerr << "[PlayerCore] No audio or video streams found\n";
        Close();
        return false;
    }

    // Initialize audio resampler (output: S16 stereo 44100)
    if (audio_dec_ctx_) {
        AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
        int ret = swr_alloc_set_opts2(&swr_ctx_,
            &out_layout, AV_SAMPLE_FMT_S16, 44100,
            &audio_dec_ctx_->ch_layout, audio_dec_ctx_->sample_fmt, audio_dec_ctx_->sample_rate,
            0, nullptr);
        if (ret >= 0) swr_init(swr_ctx_);
    }

    // Initialize video scaler (output: YUV420P for SDL texture)
    if (video_dec_ctx_) {
        sws_ctx_ = sws_getContext(
            video_width_, video_height_, video_dec_ctx_->pix_fmt,
            video_width_, video_height_, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
    }

    // Open SDL audio
    if (audio_dec_ctx_) {
        SDL_AudioSpec wanted_spec{}, obtained_spec{};
        wanted_spec.freq = 44100;
        wanted_spec.format = AUDIO_S16SYS;
        wanted_spec.channels = 2;
        wanted_spec.silence = 0;
        // From ffplay: SDL_AUDIO_MAX_CALLBACKS_PER_SEC = 30
        wanted_spec.samples = std::max(512, 2 << static_cast<int>(log2(44100.0 / 30)));
        wanted_spec.callback = [](void* ud, Uint8* stream, int len) {
            static_cast<PlayerCore*>(ud)->AudioCallback(stream, len);
        };
        wanted_spec.userdata = this;

        audio_dev_ = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &obtained_spec,
                                          SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
        if (audio_dev_ == 0) {
            std::cerr << "[PlayerCore] SDL_OpenAudioDevice failed: " << SDL_GetError() << "\n";
        } else {
            audio_hw_buf_size_ = obtained_spec.size;
        }
    }

    // Create sonic stream for time-stretching (speed change without pitch shift)
    // Input/output: S16 stereo at 44100Hz
    if (audio_dec_ctx_) {
        sonic_stream_ = sonicCreateStream(44100, 2);
        sonicSetSpeed(sonic_stream_, static_cast<float>(play_speed_.load()));
    }

    // Init clocks
    video_pkt_q_.serial = 0;
    audio_pkt_q_.serial = 0;
    audio_serial_ = 0;
    video_serial_ = 0;
    audclk_.Init(&audio_pkt_q_.serial);
    vidclk_.Init(&video_pkt_q_.serial);
    // Reset queues
    video_pkt_q_.Reset();
    audio_pkt_q_.Reset();
    video_fq_.Reset();
    audio_fq_.Reset();

    // Reset audio buffer
    audio_buf_ = nullptr;
    audio_buf_size_ = 0;
    audio_buf_index_ = 0;
    audio_clock_ = NAN;
    audio_clock_serial_ = -1;

    frame_timer_ = av_gettime_relative() / 1000000.0;
    last_video_pts_ = NAN;
    eof_reached_ = false;

    // Start threads
    playing_ = true;
    paused_ = false;
    demux_thread_ = std::thread(&PlayerCore::DemuxThread, this);
    if (video_dec_ctx_)
        video_dec_thread_ = std::thread(&PlayerCore::VideoDecodeThread, this);
    if (audio_dec_ctx_)
        audio_dec_thread_ = std::thread(&PlayerCore::AudioDecodeThread, this);

    // Unpause SDL audio
    if (audio_dev_)
        SDL_PauseAudioDevice(audio_dev_, 0);

    return true;
}

void PlayerCore::Close() {
    abort_ = true;

    // IMPORTANT: Close SDL audio device FIRST to stop audio callback
    // before we destroy any data it might be accessing.
    if (audio_dev_) {
        SDL_PauseAudioDevice(audio_dev_, 1);
        SDL_CloseAudioDevice(audio_dev_);
        audio_dev_ = 0;
    }

    // Destroy sonic stream
    if (sonic_stream_) {
        sonicDestroyStream(sonic_stream_);
        sonic_stream_ = nullptr;
    }
    sonic_out_buf_.clear();
    sonic_out_ptr_ = nullptr;
    sonic_out_remaining_ = 0;

    // Now abort queues to unblock threads
    video_pkt_q_.Abort();
    audio_pkt_q_.Abort();
    video_fq_.Abort();
    audio_fq_.Abort();

    if (demux_thread_.joinable()) demux_thread_.join();
    if (video_dec_thread_.joinable()) video_dec_thread_.join();
    if (audio_dec_thread_.joinable()) audio_dec_thread_.join();

    if (sws_ctx_) { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }
    if (swr_ctx_) { swr_free(&swr_ctx_); }

    if (video_dec_ctx_) { avcodec_free_context(&video_dec_ctx_); }
    if (audio_dec_ctx_) { avcodec_free_context(&audio_dec_ctx_); }
    if (fmt_ctx_) { avformat_close_input(&fmt_ctx_); }

    // Free owned audio buffer
    if (audio_buf1_) {
        av_free(audio_buf1_);
        audio_buf1_ = nullptr;
        audio_buf1_size_ = 0;
    }
    audio_buf_ = nullptr;
    audio_buf_size_ = 0;
    audio_buf_index_ = 0;

    video_pkt_q_.Flush();
    audio_pkt_q_.Flush();
    video_fq_.Flush();
    audio_fq_.Flush();

    playing_ = false;
    file_path_.clear();
    video_stream_idx_ = -1;
    audio_stream_idx_ = -1;
}

void PlayerCore::Play() {
    if (paused_) TogglePause();
}

void PlayerCore::Pause() {
    if (!paused_) TogglePause();
}

void PlayerCore::TogglePause() {
    bool was_paused = paused_.load();
    paused_ = !was_paused;

    if (!was_paused) {
        // Pausing: freeze clocks at current value
        audclk_.Set(audclk_.Get(), audclk_.serial);
        vidclk_.Set(vidclk_.Get(), vidclk_.serial);
    } else {
        // Resuming: re-anchor clocks to current wall time so they
        // continue from where they were paused (no jump).
        audclk_.Set(audclk_.pts, audclk_.serial);
        vidclk_.Set(vidclk_.pts, vidclk_.serial);
        frame_timer_ = av_gettime_relative() / 1000000.0;
    }

    audclk_.paused = paused_ ? 1 : 0;
    vidclk_.paused = paused_ ? 1 : 0;
    if (audio_dev_)
        SDL_PauseAudioDevice(audio_dev_, paused_ ? 1 : 0);
}

void PlayerCore::Seek(double position_sec) {
    if (!playing_) return;
    seek_target_ = position_sec;
    seek_req_ = true;
}

void PlayerCore::SetSpeed(double speed) {
    speed = std::clamp(speed, 0.1, 4.0);
    play_speed_ = speed;

    // Update sonic time-stretch speed. Sonic handles speed change
    // without pitch shift (PSOLA algorithm). The SDL audio device
    // stays at 44100Hz; sonic stretches/compresses the audio data.
    if (sonic_stream_) {
        sonicSetSpeed(sonic_stream_, static_cast<float>(speed));
    }
}

void PlayerCore::SetOffsetMs(double ms) {
    offset_ms_ = ms;
    // No timing state reset needed — offset is applied directly in
    // VideoRefresh when comparing video PTS to audio clock.
    // Convention: positive = audio ahead of video (same as detection).
    // Preview: video is displayed earlier (shifted toward audio).
}

double PlayerCore::GetDuration() const {
    if (!fmt_ctx_) return 0.0;
    return fmt_ctx_->duration > 0
        ? static_cast<double>(fmt_ctx_->duration) / AV_TIME_BASE
        : 0.0;
}

double PlayerCore::GetCurrentTime() const {
    double t = 0.0;
    if (HasAudio()) t = audclk_.Get();
    else if (HasVideo()) t = vidclk_.Get();

    // Clamp to [0, duration] so the displayed time never exceeds the
    // media length (e.g. after EOF when the clock keeps drifting).
    double dur = GetDuration();
    if (std::isnan(t) || t < 0.0) t = 0.0;
    if (dur > 0.0 && t > dur) t = dur;
    return t;
}

// ===========================================================================
// Demux thread
// ===========================================================================
void PlayerCore::DemuxThread() {
    AVPacket* pkt = av_packet_alloc();

    while (!abort_) {
        // Handle seek
        if (seek_req_) {
            double target = seek_target_.load();
            int64_t ts = static_cast<int64_t>(target * AV_TIME_BASE);

            // IMPORTANT: Pause SDL audio FIRST to prevent audio callback from
            // accessing freed memory during flush. This eliminates the race
            // condition that causes "Incorrect checksum for freed object" crash.
            if (audio_dev_)
                SDL_PauseAudioDevice(audio_dev_, 1);

            int ret = av_seek_frame(fmt_ctx_, -1, ts, AVSEEK_FLAG_BACKWARD);
            if (ret < 0) {
                std::cerr << "[PlayerCore] av_seek_frame failed: " << ret << "\n";
            }

            // Flush packet queues — this bumps serial so decode threads
            // will detect the serial mismatch and flush their own decoders.
            video_pkt_q_.Flush();
            audio_pkt_q_.Flush();

            // Flush decoded frame queues (also wakes blocked Push() calls).
            // Audio callback is paused, so no race on frame data.
            video_fq_.Flush();
            audio_fq_.Flush();

            // Reset audio buffer so callback doesn't read stale data
            audio_buf_index_ = 0;
            audio_buf_size_ = 0;
            audio_buf_ = nullptr;

            // Clear EOF flag — we're seeking to a new position
            eof_reached_ = false;
            audclk_.paused = paused_ ? 1 : 0;
            vidclk_.paused = paused_ ? 1 : 0;

            // Flush sonic stream to discard any buffered stretched audio
            if (sonic_stream_) {
                sonicFlushStream(sonic_stream_);
                int avail = sonicSamplesAvailable(sonic_stream_);
                if (avail > 0) {
                    std::vector<int16_t> drain(avail * 2);
                    sonicReadShortFromStream(sonic_stream_, drain.data(), avail);
                }
            }

            // Reset timing state for smooth playback after seek
            frame_timer_ = av_gettime_relative() / 1000000.0;
            last_video_pts_ = NAN;

            // Reset clocks to the seek target
            audclk_.Set(target, audio_pkt_q_.serial);
            vidclk_.Set(target, video_pkt_q_.serial);
            audio_clock_ = target;
            audio_clock_serial_ = audio_pkt_q_.serial;

            seek_req_ = false;

            // Resume SDL audio AFTER everything is cleaned up
            if (audio_dev_ && !paused_.load())
                SDL_PauseAudioDevice(audio_dev_, 0);
        }

        // Limit queue size
        if (video_pkt_q_.Size() > 60 || audio_pkt_q_.Size() > 120) {
            SDL_Delay(10);
            continue;
        }

        int ret = av_read_frame(fmt_ctx_, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF || avio_feof(fmt_ctx_->pb)) {
                // Mark EOF reached. When both frame queues are drained,
                // freeze the clocks so the displayed time stops advancing.
                if (!eof_reached_) {
                    eof_reached_ = true;
                }
                // Once all decoded frames have been consumed, freeze the
                // clocks at the media duration to stop time display drift.
                if (audio_fq_.Readable() == 0 && video_fq_.Readable() == 0) {
                    double dur = GetDuration();
                    if (dur > 0) {
                        audclk_.Set(dur, audio_pkt_q_.serial);
                        vidclk_.Set(dur, video_pkt_q_.serial);
                        audclk_.paused = 1;
                        vidclk_.paused = 1;
                    }
                }
                SDL_Delay(100);
                continue;
            }
            break;
        }

        if (pkt->stream_index == video_stream_idx_) {
            video_pkt_q_.Push(pkt, video_pkt_q_.serial);
        } else if (pkt->stream_index == audio_stream_idx_) {
            audio_pkt_q_.Push(pkt, audio_pkt_q_.serial);
        } else {
            av_packet_unref(pkt);
        }
    }

    av_packet_free(&pkt);
}

// ===========================================================================
// Video decode thread
// ===========================================================================
void PlayerCore::VideoDecodeThread() {
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    while (!abort_) {
        int serial = 0;
        if (!video_pkt_q_.Pop(pkt, serial)) break;

        // Discard packets from old serial (before seek)
        if (serial != video_pkt_q_.serial) {
            av_packet_unref(pkt);
            continue;
        }

        // Detect serial change (seek happened) — flush decoder to discard
        // internal buffered frames from before the seek point.
        if (serial != last_video_serial_) {
            avcodec_flush_buffers(video_dec_ctx_);
            last_video_serial_ = serial;
            // After seek, fast-forward: skip decoded frames whose PTS is
            // well before the seek target so the frame queue only receives
            // frames near the target position. This prevents the display
            // from appearing frozen on large-GOP (infrequent keyframe)
            // videos, where the decoder must decode many frames from the
            // previous keyframe before reaching the target.
            video_seek_skip_target_ = seek_target_.load();
            video_seek_skipping_ = true;
        }

        avcodec_send_packet(video_dec_ctx_, pkt);
        av_packet_unref(pkt);

        while (!abort_) {
            int ret = avcodec_receive_frame(video_dec_ctx_, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            // Calculate PTS
            double pts = 0.0;
            if (frame->pts != AV_NOPTS_VALUE) {
                auto* st = fmt_ctx_->streams[video_stream_idx_];
                pts = frame->pts * av_q2d(st->time_base);
            }

            // After a seek, skip frames whose PTS is far before the target.
            // Keep the frame if it is within 0.5s of the target, or if it is
            // the last decodable frame before the target (so we always show
            // something). This allows fast-forward through large GOPs.
            if (video_seek_skipping_) {
                double margin = 0.5;  // show frame if within 0.5s of target
                if (pts < video_seek_skip_target_ - margin) {
                    av_frame_unref(frame);
                    continue;  // skip this frame, decode next
                }
                video_seek_skipping_ = false;  // done skipping
            }

            double duration = (video_fps_ > 0) ? (1.0 / video_fps_) : 0.04;

            // Convert to YUV420P
            AVFrame* yuv = av_frame_alloc();
            yuv->format = AV_PIX_FMT_YUV420P;
            yuv->width = video_width_;
            yuv->height = video_height_;
            av_frame_get_buffer(yuv, 0);

            sws_scale(sws_ctx_,
                       frame->data, frame->linesize, 0, video_height_,
                       yuv->data, yuv->linesize);

            DecodedFrame df;
            df.frame = yuv;
            df.pts = pts;
            df.duration = duration;
            df.serial = serial;

            if (!video_fq_.Push(std::move(df))) break;
            av_frame_unref(frame);
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
}

// ===========================================================================
// Audio decode thread
// ===========================================================================
void PlayerCore::AudioDecodeThread() {
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    while (!abort_) {
        int serial = 0;
        if (!audio_pkt_q_.Pop(pkt, serial)) break;

        // Discard packets from old serial (before seek)
        if (serial != audio_pkt_q_.serial) {
            av_packet_unref(pkt);
            continue;
        }

        // Detect serial change (seek happened) — flush decoder
        if (serial != last_audio_serial_) {
            avcodec_flush_buffers(audio_dec_ctx_);
            last_audio_serial_ = serial;
        }

        avcodec_send_packet(audio_dec_ctx_, pkt);
        av_packet_unref(pkt);

        while (!abort_) {
            int ret = avcodec_receive_frame(audio_dec_ctx_, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            double pts = 0.0;
            if (frame->pts != AV_NOPTS_VALUE) {
                auto* st = fmt_ctx_->streams[audio_stream_idx_];
                pts = frame->pts * av_q2d(st->time_base);
            }

            double duration = static_cast<double>(frame->nb_samples) /
                              frame->sample_rate;

            // Resample to S16 stereo 44100
            int out_samples_max = swr_get_out_samples(swr_ctx_, frame->nb_samples);
            if (out_samples_max <= 0) out_samples_max = frame->nb_samples * 2;

            AVFrame* resampled = av_frame_alloc();
            resampled->format = AV_SAMPLE_FMT_S16;
            resampled->sample_rate = 44100;
            AVChannelLayout stereo_layout = AV_CHANNEL_LAYOUT_STEREO;
            av_channel_layout_copy(&resampled->ch_layout, &stereo_layout);
            resampled->nb_samples = out_samples_max;
            av_frame_get_buffer(resampled, 0);

            int out_samples = swr_convert(swr_ctx_,
                resampled->data, out_samples_max,
                (const uint8_t**)frame->extended_data, frame->nb_samples);

            if (out_samples > 0) {
                resampled->nb_samples = out_samples;

                DecodedFrame df;
                df.frame = resampled;
                df.pts = pts;
                df.duration = duration;
                df.serial = serial;

                if (!audio_fq_.Push(std::move(df))) {
                    av_frame_unref(frame);
                    break;
                }
            } else {
                av_frame_free(&resampled);
            }

            av_frame_unref(frame);
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
}

// ===========================================================================
// AudioDecodeFrame – reads a decoded audio frame, passes it through sonic
// for time-stretching, and fills audio_buf_ with the output.
// Returns byte count of data in audio_buf_, or -1 if no data.
// ===========================================================================
int PlayerCore::AudioDecodeFrame() {
    if (paused_) return -1;

    // If we have leftover sonic output from a previous call, serve it first.
    // (This happens when sonic produces more output than one callback needs.)
    // --- This case is handled in AudioCallback, not here ---

    // Feed decoded frames into sonic until it produces output
    while (true) {
        DecodedFrame* af = nullptr;
        if (!audio_fq_.PeekNonBlocking(af) || !af || !af->frame)
            return -1;

        AVFrame* frame = af->frame;
        int in_samples = frame->nb_samples;
        int data_size = in_samples * 2 * 2;  // S16 stereo

        // Feed audio into sonic for time-stretching
        if (sonic_stream_ && frame->data[0]) {
            sonicWriteShortToStream(sonic_stream_,
                reinterpret_cast<short*>(frame->data[0]), in_samples);
        }

        // Update audio clock: advance by the media duration of input samples
        // (not output samples, since we're tracking media time).
        if (!std::isnan(af->pts)) {
            audio_clock_ = af->pts + static_cast<double>(in_samples) / 44100.0;
        } else {
            audio_clock_ = NAN;
        }
        audio_clock_serial_ = af->serial;

        // Consume the decoded frame
        audio_fq_.Pop();

        // Check if sonic has output available
        if (sonic_stream_) {
            int avail = sonicSamplesAvailable(sonic_stream_);
            if (avail > 0) {
                sonic_out_buf_.resize(avail * 2);  // stereo
                int got = sonicReadShortFromStream(sonic_stream_,
                    sonic_out_buf_.data(), avail);
                if (got > 0) {
                    int out_bytes = got * 2 * 2;  // S16 stereo
                    // Ensure owned buffer is large enough
                    if (audio_buf1_size_ < static_cast<unsigned int>(out_bytes) || !audio_buf1_) {
                        av_free(audio_buf1_);
                        audio_buf1_ = (uint8_t*)av_malloc(out_bytes + 256);
                        audio_buf1_size_ = out_bytes + 256;
                    }
                    memcpy(audio_buf1_, sonic_out_buf_.data(), out_bytes);
                    audio_buf_ = audio_buf1_;
                    return out_bytes;
                }
            }
        }
        // Sonic hasn't produced enough output yet; feed more input.
    }
}

// ===========================================================================
// Video refresh – called from main loop.
//
// SIMPLIFIED for AV sync calibration:
//   - Audio is master clock. Video frame display is controlled entirely
//     by comparing video PTS against (audio_clock - offset).
//   - No frame dropping, no frame duplication, no complex sync thresholds.
//   - Offset convention: offset_ms uses the same sign as detection/correction:
//     positive offset_ms = audio is ahead of video = shift video earlier.
//     In preview: we show the corrected result, so when offset > 0,
//     video is displayed EARLIER (at a LOWER audio clock time).
//     Formula: show video frame at PTS t when audio_clock >= (t - offset_sec).
//   - Speed is handled by sonic time-stretch in the audio path.
//     Video display simply follows audio clock, so it auto-adjusts.
// ===========================================================================
double PlayerCore::VideoRefresh(SDL_Renderer* renderer, SDL_Texture** texture,
                                int* tex_w, int* tex_h) {
    double remaining_time = 0.01;  // 10ms default refresh

    if (!HasVideo() || paused_) return remaining_time;

    if (video_fq_.Readable() == 0) {
        // If no frames available after seek, remain at last displayed frame.
        // Use a shorter poll interval to pick up new frames quickly.
        return 0.005;
    }

    DecodedFrame* vp = nullptr;
    if (!video_fq_.PeekNonBlocking(vp) || !vp || !vp->frame)
        return remaining_time;

    double video_pts = vp->pts;

    // --- Core sync logic: display video based on audio clock - offset ---
    // Preview shows the CORRECTED result. offset_ms uses the same sign
    // convention as detection: positive = audio ahead = shift video earlier.
    // So we show frame at PTS t when audio_clock >= (t - offset_sec).
    if (HasAudio()) {
        double offset_sec = offset_ms_.load() / 1000.0;
        double audio_time = audclk_.Get();

        if (!std::isnan(audio_time)) {
            // Show frame at PTS t when audio_clock >= (t - offset_sec).
            // positive offset → target is smaller → video appears earlier.
            // negative offset → target is larger → video appears later.
            double target_audio_time = video_pts - offset_sec;

            if (audio_time < target_audio_time - 0.040) {
                // Audio hasn't reached the target time yet — wait.
                // Use a shorter remaining time to poll more aggressively
                // (avoids video appearing frozen after seek).
                remaining_time = std::min(target_audio_time - audio_time, 0.005);
                return remaining_time;
            }
            // Audio has reached or passed the target — display this frame.

            // If we're very far behind (audio_time >> target), skip frames
            // to catch up, but keep at least the current one to show something.
            if (audio_time > target_audio_time + 0.5) {
                // Drop stale frames to catch up
                while (video_fq_.Readable() > 1) {
                    video_fq_.Pop();
                    if (!video_fq_.PeekNonBlocking(vp) || !vp || !vp->frame)
                        return 0.005;
                    double next_target = vp->pts - offset_sec;
                    if (audio_time < next_target + 0.5)
                        break;  // This frame is close enough, display it
                }
                video_pts = vp->pts;
            }
        }
    } else {
        // No audio — use wall clock to pace video at natural frame rate.
        double time = av_gettime_relative() / 1000000.0;
        double frame_duration = vp->duration;
        double last_duration = frame_duration;
        if (!std::isnan(last_video_pts_)) {
            double d = video_pts - last_video_pts_;
            if (d > 0 && d < 10.0) last_duration = d;
        }
        if (time < frame_timer_ + last_duration) {
            remaining_time = std::min(frame_timer_ + last_duration - time, remaining_time);
            return remaining_time;
        }
        frame_timer_ += last_duration;
        if (time - frame_timer_ > 0.5)
            frame_timer_ = time;
    }

    // Record this frame's PTS
    last_video_pts_ = video_pts;

    // Update video clock
    vidclk_.Set(video_pts, vp->serial);

    // Upload frame to SDL texture
    AVFrame* f = vp->frame;
    if (f && f->data[0]) {
        if (!*texture || *tex_w != f->width || *tex_h != f->height) {
            if (*texture) SDL_DestroyTexture(*texture);
            *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
                                          SDL_TEXTUREACCESS_STREAMING,
                                          f->width, f->height);
            *tex_w = f->width;
            *tex_h = f->height;
        }
        if (*texture) {
            SDL_UpdateYUVTexture(*texture, nullptr,
                                 f->data[0], f->linesize[0],
                                 f->data[1], f->linesize[1],
                                 f->data[2], f->linesize[2]);
        }
    }

    // Consume the frame
    video_fq_.Pop();

    return remaining_time;
}

// ===========================================================================
// Audio callback – called from SDL audio thread.
// SIMPLIFIED: no muting, no volume mixing. Just copy decoded audio
// and update audio clock.
// ===========================================================================
void PlayerCore::AudioCallback(uint8_t* stream, int len) {
    while (len > 0) {
        if (audio_buf_index_ >= audio_buf_size_) {
            int audio_size = AudioDecodeFrame();
            if (audio_size < 0) {
                // No data available — fill remainder with silence and return.
                // Do NOT loop with a fake audio_buf_size_ pointing to null data.
                memset(stream, 0, len);
                return;
            }
            audio_buf_size_ = audio_size;
            audio_buf_index_ = 0;
        }

        int len1 = audio_buf_size_ - audio_buf_index_;
        if (len1 > len) len1 = len;

        if (audio_buf_ && len1 > 0) {
            memcpy(stream, audio_buf_ + audio_buf_index_, len1);
        } else {
            memset(stream, 0, len1);
        }

        len -= len1;
        stream += len1;
        audio_buf_index_ += len1;
    }

    // Update audio clock, accounting for hardware buffer latency.
    // Audio clock = PTS of next sample to be written.
    // Actual playback position = audio_clock - buffered_duration.
    // buffered_duration = (2 * hw_buf + unread portion of current buf) / bytes_per_sec.
    // NOTE: audio_clock_ tracks MEDIA time (at source pace). The audio clock
    // needs to also account for sonic's speed ratio: sonic stretches the audio,
    // so 1 second of media time produces (1/speed) seconds of output. The
    // callback's byte consumption rate is always 44100*2*2 bytes/sec (real-time).
    // The clock adjustment formula works correctly because:
    //   - audio_clock_ = media_pts of the NEXT decoded sample
    //   - bytes in buffer = output bytes after sonic stretch
    //   - bytes_per_sec at hardware = 44100*2*2 (output rate)
    //   - buffered_time_sec (wall time) = bytes / bytes_per_sec
    //   - Since speed = media_time / wall_time, we need:
    //     media_clock = audio_clock_ - buffered_wall_time * speed
    if (!std::isnan(audio_clock_)) {
        int audio_write_buf_size = audio_buf_size_ - audio_buf_index_;
        double bytes_per_sec = 44100.0 * 2 * 2;  // S16 stereo at 44100Hz output
        double buffered_wall_time = static_cast<double>(2 * audio_hw_buf_size_ + audio_write_buf_size) / bytes_per_sec;
        double speed = play_speed_.load();
        double audio_callback_time = av_gettime_relative() / 1000000.0;
        audclk_.SetAt(
            audio_clock_ - buffered_wall_time * speed,
            audio_clock_serial_,
            audio_callback_time);
    }
}

void PlayerCore::FlushQueues() {
    video_pkt_q_.Flush();
    audio_pkt_q_.Flush();
    video_fq_.Flush();
    audio_fq_.Flush();

    // Flush sonic stream so stale audio doesn't bleed into post-seek playback
    if (sonic_stream_) {
        sonicFlushStream(sonic_stream_);
        // Drain any remaining output
        int avail = sonicSamplesAvailable(sonic_stream_);
        if (avail > 0) {
            std::vector<int16_t> drain(avail * 2);
            sonicReadShortFromStream(sonic_stream_, drain.data(), avail);
        }
    }
}

}  // namespace gui
}  // namespace avsync
