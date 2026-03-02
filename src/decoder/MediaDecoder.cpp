#include "decoder/MediaDecoder.h"
#include "common/Log.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstring>

namespace avsync {

struct MediaDecoder::Impl {
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* audio_codec_ctx = nullptr;
    AVCodecContext* video_codec_ctx = nullptr;
    SwrContext* swr_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;

    int audio_stream_idx = -1;
    int video_stream_idx = -1;

    int audio_sample_rate = 0;
    int audio_channels = 0;
    double video_fps = 0.0;
    int video_width = 0;
    int video_height = 0;
    double duration = 0.0;
};

MediaDecoder::MediaDecoder() : impl_(new Impl()) {
}

MediaDecoder::~MediaDecoder() {
    Close();
    delete impl_;
}

bool MediaDecoder::Open(const std::string& path) {
    Close();

    // Open input file
    int ret = avformat_open_input(&impl_->fmt_ctx, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char err_buf[256];
        av_strerror(ret, err_buf, sizeof(err_buf));
        Log::Error("MediaDecoder: failed to open '%s': %s", path.c_str(), err_buf);
        return false;
    }

    // Find stream info
    ret = avformat_find_stream_info(impl_->fmt_ctx, nullptr);
    if (ret < 0) {
        Log::Error("MediaDecoder: failed to find stream info");
        Close();
        return false;
    }

    impl_->duration = impl_->fmt_ctx->duration / static_cast<double>(AV_TIME_BASE);

    // Find audio and video streams
    for (unsigned int i = 0; i < impl_->fmt_ctx->nb_streams; ++i) {
        AVStream* stream = impl_->fmt_ctx->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
            impl_->audio_stream_idx < 0) {
            impl_->audio_stream_idx = static_cast<int>(i);
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                   impl_->video_stream_idx < 0) {
            impl_->video_stream_idx = static_cast<int>(i);
        }
    }

    // Open audio decoder
    if (impl_->audio_stream_idx >= 0) {
        AVStream* audio_stream = impl_->fmt_ctx->streams[impl_->audio_stream_idx];
        const AVCodec* audio_codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
        if (!audio_codec) {
            Log::Error("MediaDecoder: unsupported audio codec");
            Close();
            return false;
        }
        impl_->audio_codec_ctx = avcodec_alloc_context3(audio_codec);
        avcodec_parameters_to_context(impl_->audio_codec_ctx, audio_stream->codecpar);
        ret = avcodec_open2(impl_->audio_codec_ctx, audio_codec, nullptr);
        if (ret < 0) {
            Log::Error("MediaDecoder: failed to open audio codec");
            Close();
            return false;
        }

        impl_->audio_sample_rate = impl_->audio_codec_ctx->sample_rate;
        impl_->audio_channels = impl_->audio_codec_ctx->ch_layout.nb_channels;

        // Setup resampler to output float interleaved
        AVChannelLayout out_ch_layout;
        av_channel_layout_default(&out_ch_layout, impl_->audio_channels);

        ret = swr_alloc_set_opts2(
            &impl_->swr_ctx,
            &out_ch_layout,
            AV_SAMPLE_FMT_FLT,
            impl_->audio_sample_rate,
            &impl_->audio_codec_ctx->ch_layout,
            impl_->audio_codec_ctx->sample_fmt,
            impl_->audio_codec_ctx->sample_rate,
            0, nullptr
        );
        if (ret < 0 || !impl_->swr_ctx) {
            Log::Error("MediaDecoder: failed to create resampler");
            Close();
            return false;
        }
        swr_init(impl_->swr_ctx);
    }

    // Open video decoder
    if (impl_->video_stream_idx >= 0) {
        AVStream* video_stream = impl_->fmt_ctx->streams[impl_->video_stream_idx];
        const AVCodec* video_codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
        if (!video_codec) {
            Log::Error("MediaDecoder: unsupported video codec");
            Close();
            return false;
        }
        impl_->video_codec_ctx = avcodec_alloc_context3(video_codec);
        avcodec_parameters_to_context(impl_->video_codec_ctx, video_stream->codecpar);
        ret = avcodec_open2(impl_->video_codec_ctx, video_codec, nullptr);
        if (ret < 0) {
            Log::Error("MediaDecoder: failed to open video codec");
            Close();
            return false;
        }

        impl_->video_width = impl_->video_codec_ctx->width;
        impl_->video_height = impl_->video_codec_ctx->height;

        // Calculate FPS
        AVRational fr = video_stream->avg_frame_rate;
        if (fr.num > 0 && fr.den > 0) {
            impl_->video_fps = av_q2d(fr);
        } else {
            fr = video_stream->r_frame_rate;
            impl_->video_fps = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 25.0;
        }

        // Setup scaler to convert to RGB24
        impl_->sws_ctx = sws_getContext(
            impl_->video_width, impl_->video_height,
            impl_->video_codec_ctx->pix_fmt,
            impl_->video_width, impl_->video_height,
            AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
    }

    Log::Info("MediaDecoder: opened '%s' (duration=%.2fs, audio=%dHz/%dch, video=%dx%d@%.2ffps)",
              path.c_str(), impl_->duration,
              impl_->audio_sample_rate, impl_->audio_channels,
              impl_->video_width, impl_->video_height, impl_->video_fps);

    return true;
}

void MediaDecoder::Close() {
    if (impl_->sws_ctx) {
        sws_freeContext(impl_->sws_ctx);
        impl_->sws_ctx = nullptr;
    }
    if (impl_->swr_ctx) {
        swr_free(&impl_->swr_ctx);
    }
    if (impl_->audio_codec_ctx) {
        avcodec_free_context(&impl_->audio_codec_ctx);
    }
    if (impl_->video_codec_ctx) {
        avcodec_free_context(&impl_->video_codec_ctx);
    }
    if (impl_->fmt_ctx) {
        avformat_close_input(&impl_->fmt_ctx);
    }
    impl_->audio_stream_idx = -1;
    impl_->video_stream_idx = -1;
}

double MediaDecoder::GetDuration() const { return impl_->duration; }
int MediaDecoder::GetAudioSampleRate() const { return impl_->audio_sample_rate; }
int MediaDecoder::GetAudioChannels() const { return impl_->audio_channels; }
double MediaDecoder::GetVideoFps() const { return impl_->video_fps; }
int MediaDecoder::GetVideoWidth() const { return impl_->video_width; }
int MediaDecoder::GetVideoHeight() const { return impl_->video_height; }

// ============================================================================
// Streaming DecodeSegments: single-pass demux with sliding window
// ============================================================================
//
// Architecture:
//   One continuous av_read_frame() loop from start to end (no seeking).
//   Audio and video frames are decoded and appended to rolling buffers.
//   Each frame is tagged with its PTS (seconds).
//   When the highest video PTS crosses the current segment boundary,
//   we slice the buffers, fire the callback, and advance the window.
//   Overlap data (for step < window) is preserved by only trimming
//   samples/frames that fall before the next segment's start.
//
// Memory: at most ~2 segments of data at any time (current + overlap).
// ============================================================================

bool MediaDecoder::DecodeSegments(
    double window_sec,
    double step_sec,
    const SegmentCallback& segment_cb
) {
    if (!impl_->fmt_ctx) {
        Log::Error("MediaDecoder: file not opened");
        return false;
    }

    const double duration = impl_->duration;
    const int sample_rate = impl_->audio_sample_rate;
    const int channels = impl_->audio_channels;
    const int samples_per_sec = sample_rate * channels;
    const int width = impl_->video_width;
    const int height = impl_->video_height;
    const int rgb_size = width * height * 3;

    // Rolling buffers: audio samples with timeline, video frames with PTS
    struct TimedFrame {
        double pts;
        FrameData data;  // shared_ptr for zero-copy segment slicing
    };

    std::vector<float> audio_buf;           // interleaved PCM
    double audio_time_start = 0.0;          // timeline position of audio_buf[0]
    double audio_time_cursor = 0.0;         // current audio end time

    std::vector<TimedFrame> video_buf;      // decoded RGB frames with PTS

    // Current segment window
    double seg_start = 0.0;
    double seg_end = std::min(window_sec, duration);

    // Prepare video conversion buffer (reused)
    AVFrame* rgb_frame = nullptr;
    std::vector<uint8_t> rgb_buffer;
    if (impl_->video_stream_idx >= 0) {
        rgb_frame = av_frame_alloc();
        rgb_buffer.resize(rgb_size);
        rgb_frame->width = width;
        rgb_frame->height = height;
        rgb_frame->format = AV_PIX_FMT_RGB24;
        av_image_fill_arrays(
            rgb_frame->data, rgb_frame->linesize,
            rgb_buffer.data(), AV_PIX_FMT_RGB24,
            width, height, 1
        );
    }

    AVStream* audio_stream = (impl_->audio_stream_idx >= 0) ?
        impl_->fmt_ctx->streams[impl_->audio_stream_idx] : nullptr;
    AVStream* video_stream = (impl_->video_stream_idx >= 0) ?
        impl_->fmt_ctx->streams[impl_->video_stream_idx] : nullptr;

    double audio_time_base = audio_stream ? av_q2d(audio_stream->time_base) : 0.0;
    double video_time_base = video_stream ? av_q2d(video_stream->time_base) : 0.0;

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    // Helper lambda: slice buffers and fire callback for [seg_start, seg_end)
    auto emit_segment = [&]() {
        AudioSegment audio_seg;
        audio_seg.start_time = seg_start;
        audio_seg.end_time = seg_end;
        audio_seg.sample_rate = sample_rate;
        audio_seg.channels = channels;

        // Slice audio: samples in [seg_start, seg_end)
        int buf_start_sample = static_cast<int>((seg_start - audio_time_start) * samples_per_sec);
        int buf_end_sample = static_cast<int>((seg_end - audio_time_start) * samples_per_sec);
        buf_start_sample = std::max(0, std::min(buf_start_sample, static_cast<int>(audio_buf.size())));
        buf_end_sample = std::max(buf_start_sample, std::min(buf_end_sample, static_cast<int>(audio_buf.size())));
        audio_seg.samples.assign(
            audio_buf.begin() + buf_start_sample,
            audio_buf.begin() + buf_end_sample
        );

        // Slice video: frames with PTS in [seg_start, seg_end)
        VideoSegment video_seg;
        video_seg.start_time = seg_start;
        video_seg.end_time = seg_end;
        video_seg.fps = impl_->video_fps;
        video_seg.width = width;
        video_seg.height = height;
        for (auto& tf : video_buf) {
            if (tf.pts >= seg_start - 0.001 && tf.pts < seg_end + 0.001) {
                video_seg.frames.push_back(tf.data);  // zero-copy: shared_ptr ref
            }
        }

        Log::Debug("MediaDecoder: emitting segment [%.2f, %.2f)s: %zu audio samples, %zu video frames",
                    seg_start, seg_end, audio_seg.samples.size(), video_seg.frames.size());

        segment_cb(audio_seg, video_seg);
    };

    // Helper lambda: advance the sliding window after emitting
    auto advance_window = [&]() {
        double next_start = seg_start + step_sec;

        // Trim audio buffer: discard samples before next_start
        int discard_samples = static_cast<int>((next_start - audio_time_start) * samples_per_sec);
        if (discard_samples > 0 && discard_samples < static_cast<int>(audio_buf.size())) {
            audio_buf.erase(audio_buf.begin(), audio_buf.begin() + discard_samples);
            audio_time_start = next_start;
        } else if (discard_samples >= static_cast<int>(audio_buf.size())) {
            audio_buf.clear();
            audio_time_start = next_start;
        }

        // Trim video buffer: discard frames before next_start
        auto it = std::remove_if(video_buf.begin(), video_buf.end(),
            [next_start](const TimedFrame& tf) { return tf.pts < next_start - 0.001; });
        video_buf.erase(it, video_buf.end());

        seg_start = next_start;
        seg_end = std::min(seg_start + window_sec, duration);
    };

    // Track the maximum PTS seen (to know when we've passed a segment boundary)
    double max_media_pts = 0.0;

    // ---- Main demux loop ----
    while (av_read_frame(impl_->fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == impl_->audio_stream_idx && impl_->audio_codec_ctx) {
            int ret = avcodec_send_packet(impl_->audio_codec_ctx, pkt);
            while (ret >= 0) {
                ret = avcodec_receive_frame(impl_->audio_codec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) break;

                // Compute PTS for this audio frame
                double pts = (frame->pts != AV_NOPTS_VALUE) ?
                    frame->pts * audio_time_base : audio_time_cursor;
                double frame_dur = static_cast<double>(frame->nb_samples) / sample_rate;

                // Resample to float interleaved
                int out_samples = swr_get_out_samples(impl_->swr_ctx, frame->nb_samples);
                std::vector<float> tmp(out_samples * channels);
                uint8_t* out_buf = reinterpret_cast<uint8_t*>(tmp.data());
                int converted = swr_convert(
                    impl_->swr_ctx,
                    &out_buf, out_samples,
                    const_cast<const uint8_t**>(frame->extended_data),
                    frame->nb_samples
                );
                if (converted > 0) {
                    tmp.resize(converted * channels);
                    audio_buf.insert(audio_buf.end(), tmp.begin(), tmp.end());
                }

                audio_time_cursor = pts + frame_dur;
                if (audio_time_cursor > max_media_pts)
                    max_media_pts = audio_time_cursor;
            }
        }
        else if (pkt->stream_index == impl_->video_stream_idx && impl_->video_codec_ctx) {
            int ret = avcodec_send_packet(impl_->video_codec_ctx, pkt);
            while (ret >= 0) {
                ret = avcodec_receive_frame(impl_->video_codec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) break;

                double pts = (frame->pts != AV_NOPTS_VALUE) ?
                    frame->pts * video_time_base : 0.0;

                // Convert to RGB24
                sws_scale(
                    impl_->sws_ctx,
                    frame->data, frame->linesize, 0, height,
                    rgb_frame->data, rgb_frame->linesize
                );

                // Copy RGB data into a shared FrameData
                TimedFrame tf;
                tf.pts = pts;
                auto frame_data = std::make_shared<std::vector<uint8_t>>(rgb_size);
                for (int y = 0; y < height; ++y) {
                    std::memcpy(
                        frame_data->data() + y * width * 3,
                        rgb_frame->data[0] + y * rgb_frame->linesize[0],
                        width * 3
                    );
                }
                tf.data = std::move(frame_data);
                video_buf.push_back(std::move(tf));

                if (pts > max_media_pts)
                    max_media_pts = pts;
            }
        }
        av_packet_unref(pkt);

        // Check if we've passed the current segment boundary
        // Emit segment(s) as long as we have enough data
        while (max_media_pts >= seg_end && seg_start < duration) {
            emit_segment();
            advance_window();
        }
    }

    // ---- Flush decoders ----
    // Flush audio decoder
    if (impl_->audio_codec_ctx) {
        avcodec_send_packet(impl_->audio_codec_ctx, nullptr);
        int ret;
        while ((ret = avcodec_receive_frame(impl_->audio_codec_ctx, frame)) >= 0) {
            double frame_dur = static_cast<double>(frame->nb_samples) / sample_rate;
            int out_samples = swr_get_out_samples(impl_->swr_ctx, frame->nb_samples);
            std::vector<float> tmp(out_samples * channels);
            uint8_t* out_buf = reinterpret_cast<uint8_t*>(tmp.data());
            int converted = swr_convert(
                impl_->swr_ctx,
                &out_buf, out_samples,
                const_cast<const uint8_t**>(frame->extended_data),
                frame->nb_samples
            );
            if (converted > 0) {
                tmp.resize(converted * channels);
                audio_buf.insert(audio_buf.end(), tmp.begin(), tmp.end());
            }
            audio_time_cursor += frame_dur;
        }
    }

    // Flush video decoder
    if (impl_->video_codec_ctx) {
        avcodec_send_packet(impl_->video_codec_ctx, nullptr);
        int ret;
        while ((ret = avcodec_receive_frame(impl_->video_codec_ctx, frame)) >= 0) {
            double pts = (frame->pts != AV_NOPTS_VALUE) ?
                frame->pts * video_time_base : 0.0;
            sws_scale(
                impl_->sws_ctx,
                frame->data, frame->linesize, 0, height,
                rgb_frame->data, rgb_frame->linesize
            );
            TimedFrame tf;
            tf.pts = pts;
            auto frame_data = std::make_shared<std::vector<uint8_t>>(rgb_size);
            for (int y = 0; y < height; ++y) {
                std::memcpy(
                    frame_data->data() + y * width * 3,
                    rgb_frame->data[0] + y * rgb_frame->linesize[0],
                    width * 3
                );
            }
            tf.data = std::move(frame_data);
            video_buf.push_back(std::move(tf));
        }
    }

    // Emit remaining segment(s) that haven't been emitted yet
    while (seg_start < duration) {
        emit_segment();
        if (seg_start + step_sec >= duration) break;
        advance_window();
    }

    // Cleanup
    if (rgb_frame) av_frame_free(&rgb_frame);
    av_frame_free(&frame);
    av_packet_free(&pkt);

    Log::Info("MediaDecoder: streaming decode complete (%.2fs)", duration);
    return true;
}

// Legacy overload: wraps the unified SegmentCallback
bool MediaDecoder::DecodeSegments(
    double window_sec,
    double step_sec,
    const AudioSegmentCallback& audio_cb,
    const VideoSegmentCallback& video_cb
) {
    return DecodeSegments(window_sec, step_sec,
        [&](AudioSegment& audio, VideoSegment& video) {
            audio_cb(audio);
            video_cb(video);
        }
    );
}

}  // namespace avsync
