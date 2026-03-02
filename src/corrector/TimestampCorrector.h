#pragma once

#include "common/Types.h"

#include <string>
#include <vector>

namespace avsync {

// Adjusts video timestamps and remuxes the output file to correct AV sync.
// Uses FFmpeg stream copy (no re-encoding) for efficiency.
class TimestampCorrector {
public:
    TimestampCorrector();
    ~TimestampCorrector();

    // Non-copyable
    TimestampCorrector(const TimestampCorrector&) = delete;
    TimestampCorrector& operator=(const TimestampCorrector&) = delete;

    // Apply corrections and produce the output file.
    // - input_path: original video file
    // - output_path: corrected output file
    // - decisions: per-segment correction decisions
    // Returns true on success.
    bool Correct(
        const std::string& input_path,
        const std::string& output_path,
        const std::vector<CorrectionDecision>& decisions
    );

private:
    // Find the correction amount for a given timestamp
    double FindCorrectionForTime(
        double time_sec,
        const std::vector<CorrectionDecision>& decisions
    ) const;
};

}  // namespace avsync
