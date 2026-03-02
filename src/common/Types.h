#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace avsync {

// Audio-video offset detection result for one segment.
//
// Convention for offset_ms (detection offset):
//   positive = audio is AHEAD of video (audio plays too early)
//   negative = audio is BEHIND video (audio plays too late)
//
// Example: offset_ms = +200 means audio leads video by 200ms.
// To fix this, the corrector will shift video timestamps earlier
// (subtract from video PTS) so that video catches up with audio.
struct SegmentOffset {
    double start_time;   // segment start time (seconds)
    double end_time;     // segment end time (seconds)
    double offset_ms;    // detected offset in ms (positive = audio ahead of video)
    double confidence;   // detection confidence [0.0, 1.0]
    std::string method;  // which detector produced this result

    // Whether this segment was skipped (low confidence or below threshold)
    bool skipped = false;
    std::string skip_reason;
};

// Raw audio data for a segment
struct AudioSegment {
    double start_time;
    double end_time;
    int sample_rate;
    int channels;
    std::vector<float> samples;  // interleaved PCM float samples
};

// A single video frame stored as contiguous RGB bytes (width * height * 3).
// Wrapped in shared_ptr to allow zero-copy sharing between overlapping segments.
using FrameData = std::shared_ptr<std::vector<uint8_t>>;

inline FrameData MakeFrame(std::vector<uint8_t>&& data) {
    return std::make_shared<std::vector<uint8_t>>(std::move(data));
}

// Raw video data for a segment
struct VideoSegment {
    double start_time;
    double end_time;
    double fps;
    int width;
    int height;
    // Each frame is a shared_ptr to RGB bytes, enabling zero-copy slicing
    // from the rolling decode buffer into overlapping segment windows.
    std::vector<FrameData> frames;
};

// Content features extracted from a segment (used by dispatcher for auto-selection)
struct ContentFeatures {
    bool has_face = false;          // face detected in video frames
    bool has_speech = false;        // speech detected in audio
    int audio_onset_count = 0;      // number of audio onsets detected
    int video_event_count = 0;      // number of visual events detected
    double audio_energy = 0.0;      // average audio energy
    double video_motion = 0.0;      // average frame-to-frame motion
};

// Correction decision for one segment.
//
// Convention for correction_ms:
//   This uses the SAME sign convention as detection offset_ms:
//   positive = audio is ahead, so we subtract from video PTS (shift video earlier).
//   negative = audio is behind, so we add to video PTS (shift video later).
//
// In other words, correction_ms = detected offset_ms. The TimestampCorrector
// applies it by doing: video_pts -= correction_ms (converted to time_base units).
struct CorrectionDecision {
    double start_time;
    double end_time;
    double correction_ms;  // how much to adjust (same sign as detection offset; 0 = no correction)
    bool apply;            // whether to apply correction
    std::string reason;    // explanation for the decision
};

}  // namespace avsync
