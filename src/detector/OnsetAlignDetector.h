#pragma once

#include "common/Config.h"
#include "detector/ISyncDetector.h"

namespace avsync {

// Onset + visual event alignment detector.
// Detects audio onsets (energy/spectral flux peaks) and visual events
// (frame difference peaks), then cross-correlates the two event sequences
// to find the AV offset.
class OnsetAlignDetector : public ISyncDetector {
public:
    explicit OnsetAlignDetector(const OnsetAlignConfig& config);

    std::vector<SegmentOffset> Detect(
        const AudioSegment& audio,
        const VideoSegment& video
    ) override;

    bool CanHandle(const ContentFeatures& features) const override;

    std::string Name() const override { return "onset_align"; }

    // Exposed for unit testing of core algorithms

    // Detect audio onset times using spectral flux
    std::vector<double> DetectAudioOnsets(const AudioSegment& audio) const;

    // Detect visual event times using frame differencing
    std::vector<double> DetectVisualEvents(const VideoSegment& video) const;

    // Cross-correlate two event time sequences to find optimal offset
    // Returns offset in milliseconds and a confidence score
    struct AlignResult {
        double offset_ms;
        double confidence;
        int matched_pairs;   // number of matched event pairs
        int total_events;    // total events in smaller set
    };
    AlignResult CrossCorrelateEvents(
        const std::vector<double>& audio_events,
        const std::vector<double>& video_events
    ) const;

    // Compute spectral flux for audio frames
    std::vector<double> ComputeSpectralFlux(
        const std::vector<float>& samples,
        int sample_rate,
        int hop_size = 512,
        int window_size = 1024
    ) const;

    // Compute frame differences for video
    std::vector<double> ComputeFrameDifferences(const VideoSegment& video) const;

private:
    OnsetAlignConfig config_;
};

}  // namespace avsync
