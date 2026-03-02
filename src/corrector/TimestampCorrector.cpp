#include "corrector/TimestampCorrector.h"
#include "common/Log.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
#include <libavutil/dict.h>
}

#include <cmath>
#include <cstring>

namespace avsync {

TimestampCorrector::TimestampCorrector() = default;
TimestampCorrector::~TimestampCorrector() = default;

double TimestampCorrector::FindCorrectionForTime(
    double time_sec,
    const std::vector<CorrectionDecision>& decisions
) const {
    for (const auto& d : decisions) {
        if (time_sec >= d.start_time && time_sec < d.end_time && d.apply) {
            return d.correction_ms;
        }
    }
    return 0.0;
}

bool TimestampCorrector::Correct(
    const std::string& input_path,
    const std::string& output_path,
    const std::vector<CorrectionDecision>& decisions
) {
    // Check if any corrections need to be applied
    bool has_corrections = false;
    for (const auto& d : decisions) {
        if (d.apply && std::abs(d.correction_ms) > 0.0) {
            has_corrections = true;
            break;
        }
    }

    if (!has_corrections) {
        Log::Info("TimestampCorrector: no corrections to apply");
        return true;
    }

    // Open input
    AVFormatContext* in_fmt_ctx = nullptr;
    int ret = avformat_open_input(&in_fmt_ctx, input_path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char err_buf[256];
        av_strerror(ret, err_buf, sizeof(err_buf));
        Log::Error("TimestampCorrector: failed to open input '%s': %s",
                   input_path.c_str(), err_buf);
        return false;
    }

    ret = avformat_find_stream_info(in_fmt_ctx, nullptr);
    if (ret < 0) {
        Log::Error("TimestampCorrector: failed to find stream info");
        avformat_close_input(&in_fmt_ctx);
        return false;
    }

    // Open output
    AVFormatContext* out_fmt_ctx = nullptr;
    ret = avformat_alloc_output_context2(&out_fmt_ctx, nullptr, nullptr, output_path.c_str());
    if (ret < 0 || !out_fmt_ctx) {
        Log::Error("TimestampCorrector: failed to create output context");
        avformat_close_input(&in_fmt_ctx);
        return false;
    }

    // Copy stream definitions
    int video_stream_idx = -1;
    for (unsigned int i = 0; i < in_fmt_ctx->nb_streams; ++i) {
        AVStream* in_stream = in_fmt_ctx->streams[i];
        AVStream* out_stream = avformat_new_stream(out_fmt_ctx, nullptr);
        if (!out_stream) {
            Log::Error("TimestampCorrector: failed to create output stream");
            avformat_close_input(&in_fmt_ctx);
            avformat_free_context(out_fmt_ctx);
            return false;
        }
        avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        out_stream->codecpar->codec_tag = 0;

        if (in_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_idx < 0) {
            video_stream_idx = static_cast<int>(i);

            // For HEVC/H.265 in MP4/MOV, set codec tag to 'hvc1' for
            // better compatibility (Apple devices, web players, etc.)
            if (in_stream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
                out_stream->codecpar->codec_tag = MKTAG('h','v','c','1');
                Log::Info("TimestampCorrector: set HEVC tag to hvc1");
            }
        }
    }

    // Open output file
    if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&out_fmt_ctx->pb, output_path.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            char err_buf[256];
            av_strerror(ret, err_buf, sizeof(err_buf));
            Log::Error("TimestampCorrector: failed to open output file '%s': %s",
                       output_path.c_str(), err_buf);
            avformat_close_input(&in_fmt_ctx);
            avformat_free_context(out_fmt_ctx);
            return false;
        }
    }

    // Write header with faststart (movflags +faststart) for MP4/MOV.
    // This moves the moov atom to the beginning of the file for faster
    // playback start and better streaming compatibility.
    AVDictionary* muxer_opts = nullptr;
    const char* fmt_name = out_fmt_ctx->oformat->name;
    if (fmt_name && (std::strcmp(fmt_name, "mp4") == 0 ||
                     std::strcmp(fmt_name, "mov") == 0 ||
                     std::strcmp(fmt_name, "3gp") == 0 ||
                     std::strstr(fmt_name, "mp4") != nullptr)) {
        av_dict_set(&muxer_opts, "movflags", "+faststart", 0);
        Log::Info("TimestampCorrector: enabling faststart for %s", fmt_name);
    }

    ret = avformat_write_header(out_fmt_ctx, &muxer_opts);
    av_dict_free(&muxer_opts);
    if (ret < 0) {
        Log::Error("TimestampCorrector: failed to write output header");
        avformat_close_input(&in_fmt_ctx);
        avio_closep(&out_fmt_ctx->pb);
        avformat_free_context(out_fmt_ctx);
        return false;
    }

    // Remux packets, adjusting video timestamps
    AVPacket* pkt = av_packet_alloc();
    int64_t packets_written = 0;
    int64_t packets_adjusted = 0;

    while (av_read_frame(in_fmt_ctx, pkt) >= 0) {
        AVStream* in_stream = in_fmt_ctx->streams[pkt->stream_index];
        AVStream* out_stream = out_fmt_ctx->streams[pkt->stream_index];

        // Adjust video stream timestamps based on correction decisions.
        // Convention: correction_ms uses the same sign as detection offset:
        //   positive = audio is AHEAD of video (audio plays too early)
        //   negative = audio is BEHIND video (audio plays too late)
        // To fix "audio ahead": SUBTRACT positive correction from video PTS
        // (shift video earlier so it catches up with audio).
        // To fix "audio behind": SUBTRACT negative correction from video PTS
        // = ADD to video PTS (shift video later so audio catches up).
        if (pkt->stream_index == video_stream_idx) {
            double pkt_time = pkt->pts * av_q2d(in_stream->time_base);
            double correction_ms = FindCorrectionForTime(pkt_time, decisions);

            if (std::abs(correction_ms) > 0.0) {
                // Convert correction from ms to input stream time_base units.
                // pkt->pts is still in in_stream->time_base at this point;
                // it will be rescaled to out_stream->time_base later below.
                int64_t correction_tb = av_rescale_q(
                    static_cast<int64_t>(correction_ms * 1000),
                    {1, 1000000},  // microseconds
                    in_stream->time_base
                );

                pkt->pts -= correction_tb;
                pkt->dts -= correction_tb;
                packets_adjusted++;
            }
        }

        // Rescale timestamps from input to output time base
        pkt->pts = av_rescale_q_rnd(pkt->pts, in_stream->time_base,
                                     out_stream->time_base,
                                     static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        pkt->dts = av_rescale_q_rnd(pkt->dts, in_stream->time_base,
                                     out_stream->time_base,
                                     static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base, out_stream->time_base);
        pkt->pos = -1;

        ret = av_interleaved_write_frame(out_fmt_ctx, pkt);
        if (ret < 0) {
            char err_buf[256];
            av_strerror(ret, err_buf, sizeof(err_buf));
            Log::Warn("TimestampCorrector: failed to write packet: %s", err_buf);
        }

        packets_written++;
        av_packet_unref(pkt);
    }

    // Write trailer
    av_write_trailer(out_fmt_ctx);

    // Cleanup
    av_packet_free(&pkt);
    avformat_close_input(&in_fmt_ctx);
    if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&out_fmt_ctx->pb);
    }
    avformat_free_context(out_fmt_ctx);

    Log::Info("TimestampCorrector: wrote %lld packets (%lld adjusted) to '%s'",
              static_cast<long long>(packets_written),
              static_cast<long long>(packets_adjusted),
              output_path.c_str());

    return true;
}

}  // namespace avsync
