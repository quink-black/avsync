#pragma once

#include "common/Config.h"
#include "common/Types.h"

#include <string>
#include <vector>

namespace avsync {

// Main processing pipeline that orchestrates the full AV sync correction flow:
// 1. Decode media → segments
// 2. Extract content features per segment
// 3. Dispatch detection per segment
// 4. Aggregate offsets → correction decisions
// 5. Apply timestamp corrections → output file
class SyncPipeline {
public:
    SyncPipeline();
    ~SyncPipeline();

    // Configure the pipeline from a Config object
    void Configure(const Config& config);

    // Run the full pipeline on input_path, write corrected output to output_path.
    // Returns true on success.
    bool Process(const std::string& input_path, const std::string& output_path);

    // Get the detection results (available after Process completes)
    const std::vector<SegmentOffset>& GetDetectionResults() const;

    // Get the correction decisions (available after Process completes)
    const std::vector<CorrectionDecision>& GetCorrectionDecisions() const;

    // Print a summary report of detection and correction results
    void PrintReport() const;

private:
    // Extract content features from audio/video segments
    ContentFeatures ExtractFeatures(
        const AudioSegment& audio,
        const VideoSegment& video
    ) const;

    Config config_;
    std::vector<SegmentOffset> detection_results_;
    std::vector<CorrectionDecision> correction_decisions_;
};

}  // namespace avsync
