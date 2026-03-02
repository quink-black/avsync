#pragma once

#include "common/Types.h"

#include <string>
#include <functional>
#include <vector>

// Forward declarations for FFmpeg types
struct AVFormatContext;
struct AVCodecContext;

namespace avsync {

struct Config;

// Callback for receiving a fully decoded segment (audio + video).
// Called when a time window is complete; segment memory may be released after return.
using SegmentCallback = std::function<void(AudioSegment& audio, VideoSegment& video)>;

// Legacy callbacks (kept for compatibility but prefer SegmentCallback)
using AudioSegmentCallback = std::function<void(const AudioSegment& segment)>;
using VideoSegmentCallback = std::function<void(const VideoSegment& segment)>;

// Media decoder using FFmpeg.
// Handles demuxing, audio/video decoding, and streaming segmented output.
class MediaDecoder {
public:
    MediaDecoder();
    ~MediaDecoder();

    // Non-copyable
    MediaDecoder(const MediaDecoder&) = delete;
    MediaDecoder& operator=(const MediaDecoder&) = delete;

    // Open a media file for decoding
    bool Open(const std::string& path);

    // Close and release resources
    void Close();

    // Get media info
    double GetDuration() const;
    int GetAudioSampleRate() const;
    int GetAudioChannels() const;
    double GetVideoFps() const;
    int GetVideoWidth() const;
    int GetVideoHeight() const;

    // Stream-decode the file and produce overlapping segments.
    // Demuxes from start to end in a single pass (no seeking).
    // For each segment [t, t+window), calls segment_cb with audio+video data.
    // Memory for each segment is released after the callback returns.
    // The sliding window advances by step_sec each time.
    //
    // Memory usage: bounded to ~2 segments worth of audio+video data
    // (current segment + overlap with next segment).
    bool DecodeSegments(
        double window_sec,
        double step_sec,
        const SegmentCallback& segment_cb
    );

    // Legacy overload: separate audio/video callbacks (wraps SegmentCallback)
    bool DecodeSegments(
        double window_sec,
        double step_sec,
        const AudioSegmentCallback& audio_cb,
        const VideoSegmentCallback& video_cb
    );

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace avsync
