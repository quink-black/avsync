#pragma once

#include "common/Config.h"
#include "common/Types.h"
#include "detector/ISyncDetector.h"

#include <memory>
#include <string>
#include <vector>

namespace avsync {

// Dispatcher that manages multiple detectors and selects the appropriate one
// based on content features and configuration.
class SyncDetectorDispatcher {
public:
    explicit SyncDetectorDispatcher(const Config& config);

    // Register a detector implementation
    void RegisterDetector(std::shared_ptr<ISyncDetector> detector);

    // Dispatch detection to the appropriate detector(s) based on mode and features.
    // Returns detected offsets; marks skipped segments with reason.
    std::vector<SegmentOffset> Dispatch(
        const AudioSegment& audio,
        const VideoSegment& video,
        const ContentFeatures& features
    );

private:
    // Auto mode: pick the best detector based on content features
    std::vector<SegmentOffset> DispatchAuto(
        const AudioSegment& audio,
        const VideoSegment& video,
        const ContentFeatures& features
    );

    // Force mode: use the specified detector
    std::vector<SegmentOffset> DispatchForce(
        const AudioSegment& audio,
        const VideoSegment& video
    );

    // Cascade mode: try detectors in order, fallback on low confidence
    std::vector<SegmentOffset> DispatchCascade(
        const AudioSegment& audio,
        const VideoSegment& video,
        const ContentFeatures& features
    );

    // Check if results meet confidence threshold
    bool MeetsConfidenceThreshold(const std::vector<SegmentOffset>& results) const;

    Config config_;
    std::vector<std::shared_ptr<ISyncDetector>> detectors_;
};

}  // namespace avsync
