#pragma once

#include "common/Types.h"

#include <memory>
#include <string>
#include <vector>

namespace avsync {

// Abstract interface for AV sync offset detectors (Strategy Pattern)
class ISyncDetector {
public:
    virtual ~ISyncDetector() = default;

    // Detect AV offset for given audio/video segment.
    // Returns a list of detected offsets (may be empty if detection fails).
    virtual std::vector<SegmentOffset> Detect(
        const AudioSegment& audio,
        const VideoSegment& video
    ) = 0;

    // Check if this detector can handle the given content features.
    virtual bool CanHandle(const ContentFeatures& features) const = 0;

    // Get detector name (e.g. "syncnet", "onset_align")
    virtual std::string Name() const = 0;
};

}  // namespace avsync
