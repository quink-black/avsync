#pragma once

#include "common/Config.h"
#include "common/Types.h"

#include <vector>

namespace avsync {

// Aggregates per-segment offset detection results into final correction decisions.
// Handles:
// - Global confidence gating (reject all corrections if avg confidence too low)
// - Filtering by confidence threshold
// - Filtering by offset threshold (offsets smaller than ±threshold are skipped)
// - Filling skipped/silent segments with neighboring valid offsets
// - Median filtering to smooth outliers
class OffsetAggregator {
public:
    explicit OffsetAggregator(const Config& config);

    // Aggregate raw detection results into correction decisions.
    // Uses global median consensus to filter outliers before per-segment decisions.
    std::vector<CorrectionDecision> Aggregate(
        const std::vector<SegmentOffset>& raw_offsets
    ) const;

private:
    // Apply median filter to smooth outlier offsets
    std::vector<double> MedianFilter(
        const std::vector<double>& values,
        int kernel_size = 3
    ) const;

    // Fill gaps (skipped segments) by interpolating from neighboring valid offsets
    void FillSkippedSegments(std::vector<CorrectionDecision>& decisions) const;

    // Compute the global median offset from all valid segments
    // Returns the median and the MAD (median absolute deviation)
    struct ConsensusResult {
        double median_offset_ms;
        double mad_ms;           // median absolute deviation
        int consensus_count;     // segments agreeing with median
        int total_valid;         // total valid (non-skipped) segments
        double avg_confidence;   // average confidence across all non-skipped segments
    };
    ConsensusResult ComputeConsensus(
        const std::vector<SegmentOffset>& raw_offsets
    ) const;

    // Cluster information for a group of nearby offset values.
    struct ClusterInfo {
        double center_ms;        // center (median) of the cluster
        double cluster_mad_ms;   // MAD within the cluster
        int cluster_size;        // number of segments in the cluster
        double cluster_confidence; // average confidence of cluster members
        std::vector<double> members; // individual offset values in this cluster
    };

    // Find ALL significant clusters (>= min_size members) using sliding window.
    // Returns clusters sorted by a composite score (size * confidence / MAD).
    std::vector<ClusterInfo> FindAllClusters(
        const std::vector<SegmentOffset>& raw_offsets,
        double bin_width_ms = 200.0,
        int min_size = 2
    ) const;

    // Select the best cluster from all candidates for use as consensus.
    // Considers: cluster size, internal consistency (MAD), offset magnitude,
    // and relationship to global statistics.
    struct ClusterResult {
        double center_ms;        // center (median) of the dominant cluster
        double cluster_mad_ms;   // MAD within the cluster
        int cluster_size;        // number of segments in the cluster
        double cluster_confidence; // average confidence of cluster members
        int total_valid;
        double avg_confidence;   // global average confidence (all valid segments)
    };
    ClusterResult SelectBestCluster(
        const std::vector<SegmentOffset>& raw_offsets,
        double bin_width_ms = 200.0
    ) const;

    Config config_;
};

}  // namespace avsync
