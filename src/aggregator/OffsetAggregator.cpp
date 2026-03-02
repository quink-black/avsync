#include "aggregator/OffsetAggregator.h"
#include "common/Log.h"

#include <algorithm>
#include <cmath>

namespace avsync {

OffsetAggregator::OffsetAggregator(const Config& config)
    : config_(config) {
}

std::vector<CorrectionDecision> OffsetAggregator::Aggregate(
    const std::vector<SegmentOffset>& raw_offsets
) const {
    std::vector<CorrectionDecision> decisions;

    if (raw_offsets.empty()) {
        return decisions;
    }

    // Step 0: Find all significant clusters and select the best one.
    auto cluster = SelectBestCluster(raw_offsets, 200.0);

    // Also compute the traditional global consensus for comparison
    auto consensus = ComputeConsensus(raw_offsets);
    Log::Info("Aggregator: global median=%.1fms, MAD=%.1fms, avg_confidence=%.3f",
             consensus.median_offset_ms, consensus.mad_ms, consensus.avg_confidence);
    Log::Info("Aggregator: cluster center=%.1fms, cluster_mad=%.1fms, cluster_size=%d/%d, cluster_confidence=%.3f",
             cluster.center_ms, cluster.cluster_mad_ms, cluster.cluster_size, cluster.total_valid,
             cluster.cluster_confidence);

    // Decide which consensus to use:
    // If the dominant cluster has >= 3 members and lower MAD than the global median,
    // trust the cluster center. Otherwise fall back to global median.
    double effective_offset = consensus.median_offset_ms;
    double effective_mad = consensus.mad_ms;
    double effective_confidence = consensus.avg_confidence;
    int effective_agreement = consensus.consensus_count;
    bool using_cluster = false;

    // Use cluster if it provides better consistency than the global median.
    // Key insight: prefer clusters with LARGE offsets over small-offset clusters,
    // because a large offset cluster (e.g. ~-900ms, tight MAD) is very unlikely
    // to be noise — noise rarely agrees on a large offset value. Small offset
    // clusters (near 0ms) are more likely noise "agreeing" on "no offset".
    if (cluster.cluster_size >= 3 && cluster.cluster_mad_ms < consensus.mad_ms) {
        // Significant offset: |center| >= 2x threshold
        bool significant_offset = std::abs(cluster.center_ms) >= config_.offset_threshold_ms * 2.0;
        // Tight internal consistency: MAD <= 100ms
        bool tight_cluster = cluster.cluster_mad_ms <= 100.0;
        // The cluster must represent a much more consistent signal than the global noise
        // (cluster MAD < 20% of global MAD)
        bool much_tighter = cluster.cluster_mad_ms < consensus.mad_ms * 0.2;

        if (significant_offset && tight_cluster && much_tighter) {
            effective_offset = cluster.center_ms;
            effective_mad = cluster.cluster_mad_ms;
            effective_confidence = cluster.cluster_confidence;
            effective_agreement = cluster.cluster_size;
            using_cluster = true;
            Log::Info("Aggregator: using cluster consensus (center=%.1fms, mad=%.1fms, size=%d, "
                      "much tighter than global MAD=%.1fms)",
                      cluster.center_ms, cluster.cluster_mad_ms, cluster.cluster_size,
                      consensus.mad_ms);
        }
    }

    // SAFETY GATE 1: Global average confidence must meet minimum threshold.
    // When confidence is too low, reject everything UNLESS a strong cluster
    // was identified that meets strict criteria (cluster override).
    if (consensus.avg_confidence < config_.min_global_confidence) {
        if (using_cluster &&
            cluster.cluster_size >= 3 &&
            cluster.cluster_confidence >= config_.min_global_confidence * 0.8 &&
            cluster.cluster_mad_ms <= 100.0 &&
            std::abs(cluster.center_ms) >= 200.0) {
            // Cluster override: the cluster is strong enough to override the gate.
            Log::Info("Aggregator: global confidence low (%.3f < %.3f) but cluster override: "
                      "center=%.1fms, mad=%.1fms, size=%d, conf=%.3f",
                      consensus.avg_confidence, config_.min_global_confidence,
                      cluster.center_ms, cluster.cluster_mad_ms,
                      cluster.cluster_size, cluster.cluster_confidence);
        } else {
            Log::Info("Aggregator: SAFETY GATE - avg confidence %.3f < threshold %.3f, "
                      "rejecting ALL corrections to prevent false positives",
                      consensus.avg_confidence, config_.min_global_confidence);
            for (const auto& seg : raw_offsets) {
                CorrectionDecision decision;
                decision.start_time = seg.start_time;
                decision.end_time = seg.end_time;
                decision.correction_ms = 0.0;
                decision.apply = false;
                decision.reason = "Rejected: global avg confidence too low (" +
                                  std::to_string(consensus.avg_confidence) + " < " +
                                  std::to_string(config_.min_global_confidence) + ")";
                decisions.push_back(decision);
            }
            return decisions;
        }
    }

    // SAFETY GATE 2: Consistency check on the EFFECTIVE consensus (cluster or global).
    // For cluster-based consensus, use cluster_mad which should be much tighter.
    // Use min(): for small offsets the 100ms absolute limit applies;
    // for large offsets the 15% relative limit applies (stricter).
    double mad_limit = std::min(100.0, std::max(50.0, std::abs(effective_offset) * 0.15));
    if (effective_agreement >= 3 && effective_mad > mad_limit) {
        Log::Info("Aggregator: SAFETY GATE (consistency) - MAD %.1fms > limit %.1fms "
                  "(offset=%.1fms), detections too inconsistent, rejecting ALL corrections",
                  effective_mad, mad_limit, effective_offset);
        for (const auto& seg : raw_offsets) {
            CorrectionDecision decision;
            decision.start_time = seg.start_time;
            decision.end_time = seg.end_time;
            decision.correction_ms = 0.0;
            decision.apply = false;
            decision.reason = "Rejected: detection results too inconsistent (MAD=" +
                              std::to_string(effective_mad) + "ms > " +
                              std::to_string(mad_limit) + "ms)";
            decisions.push_back(decision);
        }
        return decisions;
    }

    // Outlier threshold: segments deviating more than 3*effective_mad from effective_offset
    double outlier_threshold = std::max(150.0, 3.0 * effective_mad);

    // Step 1: Convert raw offsets to initial correction decisions
    for (const auto& seg : raw_offsets) {
        CorrectionDecision decision;
        decision.start_time = seg.start_time;
        decision.end_time = seg.end_time;

        if (seg.skipped) {
            decision.correction_ms = 0.0;
            decision.apply = false;
            decision.reason = "Skipped by detector: " + seg.skip_reason;
            Log::Debug("Aggregator: segment [%.2f, %.2f] skipped: %s",
                       seg.start_time, seg.end_time, seg.skip_reason.c_str());
        } else if (seg.confidence < config_.confidence_threshold) {
            // Low confidence - check if it agrees with effective consensus
            double deviation = std::abs(seg.offset_ms - effective_offset);
            if (effective_agreement >= 2 && deviation < outlier_threshold) {
                // Agrees with consensus, use effective offset
                decision.correction_ms = effective_offset;
                if (std::abs(decision.correction_ms) >= config_.offset_threshold_ms) {
                    decision.apply = true;
                    decision.reason = "Low confidence (" + std::to_string(seg.confidence) +
                                      ") but agrees with consensus (" +
                                      std::to_string(effective_offset) + "ms)";
                } else {
                    decision.apply = false;
                    decision.reason = "Low confidence, consensus offset within threshold";
                }
            } else {
                decision.correction_ms = 0.0;
                decision.apply = false;
                decision.reason = "Low confidence: " + std::to_string(seg.confidence) +
                                  ", deviation=" + std::to_string(deviation) + "ms from consensus";
            }
            Log::Info("Aggregator: segment [%.2f, %.2f] confidence=%.2f, deviation=%.1fms from consensus",
                      seg.start_time, seg.end_time, seg.confidence,
                      deviation);
        } else if (std::abs(seg.offset_ms) < config_.offset_threshold_ms) {
            decision.correction_ms = 0.0;
            decision.apply = false;
            decision.reason = "Offset within threshold: |" + std::to_string(seg.offset_ms) +
                              "ms| < " + std::to_string(config_.offset_threshold_ms) + "ms";
        } else {
            // Check if this offset is an outlier relative to effective consensus
            double deviation = std::abs(seg.offset_ms - effective_offset);
            if (effective_agreement >= 3 && deviation > outlier_threshold) {
                decision.correction_ms = effective_offset;
                decision.apply = std::abs(effective_offset) >= config_.offset_threshold_ms;
                decision.reason = "Outlier replaced by consensus (was " +
                                  std::to_string(seg.offset_ms) + "ms, consensus=" +
                                  std::to_string(effective_offset) + "ms)";
                Log::Info("Aggregator: segment [%.2f, %.2f] outlier (%.1fms, deviation=%.1fms), using consensus %.1fms",
                          seg.start_time, seg.end_time, seg.offset_ms, deviation, effective_offset);
            } else {
                decision.correction_ms = seg.offset_ms;
                decision.apply = true;
                decision.reason = "Detected by " + seg.method +
                                  " (confidence=" + std::to_string(seg.confidence) + ")";
            }
        }

        decisions.push_back(decision);
    }

    // Step 2: Apply median filter to smooth remaining offsets
    std::vector<double> offsets;
    std::vector<int> valid_indices;
    for (size_t i = 0; i < decisions.size(); ++i) {
        if (decisions[i].apply) {
            offsets.push_back(decisions[i].correction_ms);
            valid_indices.push_back(static_cast<int>(i));
        }
    }

    if (offsets.size() >= 3) {
        auto smoothed = MedianFilter(offsets, 3);
        for (size_t i = 0; i < valid_indices.size(); ++i) {
            decisions[valid_indices[i]].correction_ms = smoothed[i];
        }
    }

    // Step 3: Fill skipped segments with neighboring valid offsets
    FillSkippedSegments(decisions);

    return decisions;
}

std::vector<double> OffsetAggregator::MedianFilter(
    const std::vector<double>& values,
    int kernel_size
) const {
    std::vector<double> result(values.size());
    int half = kernel_size / 2;

    for (size_t i = 0; i < values.size(); ++i) {
        std::vector<double> window;
        for (int j = -half; j <= half; ++j) {
            int idx = static_cast<int>(i) + j;
            if (idx >= 0 && idx < static_cast<int>(values.size())) {
                window.push_back(values[idx]);
            }
        }
        std::sort(window.begin(), window.end());
        result[i] = window[window.size() / 2];
    }

    return result;
}

void OffsetAggregator::FillSkippedSegments(
    std::vector<CorrectionDecision>& decisions
) const {
    // For skipped segments, use the nearest valid neighbor's offset
    for (size_t i = 0; i < decisions.size(); ++i) {
        if (decisions[i].apply) continue;

        double prev_offset = 0.0;
        double prev_dist = 1e9;
        bool found_prev = false;

        for (int j = static_cast<int>(i) - 1; j >= 0; --j) {
            if (decisions[j].apply) {
                prev_offset = decisions[j].correction_ms;
                prev_dist = decisions[i].start_time - decisions[j].end_time;
                found_prev = true;
                break;
            }
        }

        double next_offset = 0.0;
        double next_dist = 1e9;
        bool found_next = false;

        for (size_t j = i + 1; j < decisions.size(); ++j) {
            if (decisions[j].apply) {
                next_offset = decisions[j].correction_ms;
                next_dist = decisions[j].start_time - decisions[i].end_time;
                found_next = true;
                break;
            }
        }

        if (found_prev && found_next) {
            double total_dist = prev_dist + next_dist;
            if (total_dist > 0) {
                double weight_prev = 1.0 - (prev_dist / total_dist);
                decisions[i].correction_ms = weight_prev * prev_offset +
                                             (1.0 - weight_prev) * next_offset;
            } else {
                decisions[i].correction_ms = prev_offset;
            }
            if (std::abs(decisions[i].correction_ms) >= config_.offset_threshold_ms) {
                decisions[i].apply = true;
                decisions[i].reason = "Interpolated from neighboring segments";
            }
        } else if (found_prev) {
            decisions[i].correction_ms = prev_offset;
            if (std::abs(prev_offset) >= config_.offset_threshold_ms) {
                decisions[i].apply = true;
                decisions[i].reason = "Extended from previous valid segment";
            }
        } else if (found_next) {
            decisions[i].correction_ms = next_offset;
            if (std::abs(next_offset) >= config_.offset_threshold_ms) {
                decisions[i].apply = true;
                decisions[i].reason = "Extended from next valid segment";
            }
        }
    }
}

OffsetAggregator::ConsensusResult OffsetAggregator::ComputeConsensus(
    const std::vector<SegmentOffset>& raw_offsets
) const {
    ConsensusResult result;
    result.median_offset_ms = 0.0;
    result.mad_ms = 0.0;
    result.consensus_count = 0;
    result.total_valid = 0;
    result.avg_confidence = 0.0;

    // Collect all non-skipped offset values and compute average confidence
    std::vector<double> offsets;
    double total_confidence = 0.0;
    int confidence_count = 0;
    for (const auto& seg : raw_offsets) {
        if (!seg.skipped) {
            total_confidence += seg.confidence;
            confidence_count++;
            if (seg.confidence > 0.0) {
                offsets.push_back(seg.offset_ms);
            }
        }
    }
    if (confidence_count > 0) {
        result.avg_confidence = total_confidence / confidence_count;
    }

    result.total_valid = static_cast<int>(offsets.size());
    if (offsets.empty()) {
        return result;
    }

    // Compute median
    std::sort(offsets.begin(), offsets.end());
    if (offsets.size() % 2 == 0) {
        result.median_offset_ms = (offsets[offsets.size() / 2 - 1] + offsets[offsets.size() / 2]) / 2.0;
    } else {
        result.median_offset_ms = offsets[offsets.size() / 2];
    }

    // Compute MAD (Median Absolute Deviation)
    std::vector<double> abs_deviations;
    for (double v : offsets) {
        abs_deviations.push_back(std::abs(v - result.median_offset_ms));
    }
    std::sort(abs_deviations.begin(), abs_deviations.end());
    if (abs_deviations.size() % 2 == 0) {
        result.mad_ms = (abs_deviations[abs_deviations.size() / 2 - 1] +
                         abs_deviations[abs_deviations.size() / 2]) / 2.0;
    } else {
        result.mad_ms = abs_deviations[abs_deviations.size() / 2];
    }

    // Count segments agreeing with median (within 3*MAD or minimum 80ms)
    double agreement_threshold = std::max(80.0, 3.0 * result.mad_ms);
    for (double v : offsets) {
        if (std::abs(v - result.median_offset_ms) <= agreement_threshold) {
            result.consensus_count++;
        }
    }

    return result;
}

std::vector<OffsetAggregator::ClusterInfo> OffsetAggregator::FindAllClusters(
    const std::vector<SegmentOffset>& raw_offsets,
    double bin_width_ms,
    int min_size
) const {
    // Collect valid (non-skipped) offsets with their confidences
    struct OffsetEntry {
        double offset_ms;
        double confidence;
    };
    std::vector<OffsetEntry> entries;

    for (const auto& seg : raw_offsets) {
        if (!seg.skipped && seg.confidence > 0.0) {
            entries.push_back({seg.offset_ms, seg.confidence});
        }
    }

    if (static_cast<int>(entries.size()) < min_size) {
        return {};
    }

    // Sort by offset value
    std::sort(entries.begin(), entries.end(),
              [](const OffsetEntry& a, const OffsetEntry& b) {
                  return a.offset_ms < b.offset_ms;
              });

    // Use a greedy clustering approach: iterate sorted entries,
    // group consecutive entries that are within bin_width_ms of each other.
    std::vector<ClusterInfo> clusters;
    std::vector<bool> assigned(entries.size(), false);

    for (size_t i = 0; i < entries.size(); ++i) {
        if (assigned[i]) continue;

        ClusterInfo cluster;
        cluster.members.push_back(entries[i].offset_ms);
        cluster.cluster_confidence = entries[i].confidence;
        assigned[i] = true;

        // Expand cluster: include all entries within bin_width_ms of any existing member
        for (size_t j = i + 1; j < entries.size(); ++j) {
            if (assigned[j]) continue;
            // Check if within bin_width_ms of the first member (since sorted,
            // this is equivalent to checking the cluster span)
            if (entries[j].offset_ms - entries[i].offset_ms <= bin_width_ms) {
                cluster.members.push_back(entries[j].offset_ms);
                cluster.cluster_confidence += entries[j].confidence;
                assigned[j] = true;
            }
        }

        cluster.cluster_size = static_cast<int>(cluster.members.size());
        if (cluster.cluster_size < min_size) continue;

        cluster.cluster_confidence /= cluster.cluster_size;

        // Compute median
        std::sort(cluster.members.begin(), cluster.members.end());
        if (cluster.members.size() % 2 == 0) {
            cluster.center_ms = (cluster.members[cluster.members.size() / 2 - 1] +
                                 cluster.members[cluster.members.size() / 2]) / 2.0;
        } else {
            cluster.center_ms = cluster.members[cluster.members.size() / 2];
        }

        // Compute MAD within cluster
        std::vector<double> abs_devs;
        for (double v : cluster.members) {
            abs_devs.push_back(std::abs(v - cluster.center_ms));
        }
        std::sort(abs_devs.begin(), abs_devs.end());
        if (abs_devs.size() % 2 == 0) {
            cluster.cluster_mad_ms = (abs_devs[abs_devs.size() / 2 - 1] +
                                      abs_devs[abs_devs.size() / 2]) / 2.0;
        } else {
            cluster.cluster_mad_ms = abs_devs[abs_devs.size() / 2];
        }

        clusters.push_back(cluster);
    }

    // Also try sliding window approach (centered on each entry) to catch
    // clusters that the greedy approach might split
    assigned.assign(entries.size(), false);
    for (size_t i = 0; i < entries.size(); ++i) {
        double center = entries[i].offset_ms;
        double half = bin_width_ms / 2.0;

        std::vector<double> members;
        double conf_sum = 0.0;
        for (size_t j = 0; j < entries.size(); ++j) {
            if (entries[j].offset_ms >= center - half &&
                entries[j].offset_ms <= center + half) {
                members.push_back(entries[j].offset_ms);
                conf_sum += entries[j].confidence;
            }
        }

        if (static_cast<int>(members.size()) < min_size) continue;

        ClusterInfo cluster;
        cluster.members = members;
        cluster.cluster_size = static_cast<int>(members.size());
        cluster.cluster_confidence = conf_sum / cluster.cluster_size;

        std::sort(cluster.members.begin(), cluster.members.end());
        if (cluster.members.size() % 2 == 0) {
            cluster.center_ms = (cluster.members[cluster.members.size() / 2 - 1] +
                                 cluster.members[cluster.members.size() / 2]) / 2.0;
        } else {
            cluster.center_ms = cluster.members[cluster.members.size() / 2];
        }

        std::vector<double> abs_devs;
        for (double v : cluster.members) {
            abs_devs.push_back(std::abs(v - cluster.center_ms));
        }
        std::sort(abs_devs.begin(), abs_devs.end());
        if (abs_devs.size() % 2 == 0) {
            cluster.cluster_mad_ms = (abs_devs[abs_devs.size() / 2 - 1] +
                                      abs_devs[abs_devs.size() / 2]) / 2.0;
        } else {
            cluster.cluster_mad_ms = abs_devs[abs_devs.size() / 2];
        }

        // Check if this cluster is already represented (similar center)
        bool duplicate = false;
        for (const auto& existing : clusters) {
            if (std::abs(existing.center_ms - cluster.center_ms) < bin_width_ms / 2.0 &&
                existing.cluster_size >= cluster.cluster_size) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            clusters.push_back(cluster);
        }
    }

    // Log all found clusters
    for (const auto& c : clusters) {
        Log::Debug("Aggregator: found cluster center=%.1fms, mad=%.1fms, size=%d, confidence=%.3f",
                   c.center_ms, c.cluster_mad_ms, c.cluster_size, c.cluster_confidence);
    }

    return clusters;
}

OffsetAggregator::ClusterResult OffsetAggregator::SelectBestCluster(
    const std::vector<SegmentOffset>& raw_offsets,
    double bin_width_ms
) const {
    ClusterResult result;
    result.center_ms = 0.0;
    result.cluster_mad_ms = 0.0;
    result.cluster_size = 0;
    result.cluster_confidence = 0.0;
    result.total_valid = 0;
    result.avg_confidence = 0.0;

    // Compute global stats
    double total_confidence = 0.0;
    int valid_count = 0;
    for (const auto& seg : raw_offsets) {
        if (!seg.skipped && seg.confidence > 0.0) {
            total_confidence += seg.confidence;
            valid_count++;
        }
    }
    result.total_valid = valid_count;
    if (valid_count > 0) {
        result.avg_confidence = total_confidence / valid_count;
    }

    auto clusters = FindAllClusters(raw_offsets, bin_width_ms, 2);
    if (clusters.empty()) {
        return result;
    }

    // Score each cluster. The scoring formula prioritizes:
    // 1. Large offset magnitude (real sync issues are usually > 100ms)
    // 2. Low internal MAD (consistent measurements indicate real signal)
    // 3. Larger cluster size (more agreement = more reliable)
    // 4. Higher average confidence
    //
    // A cluster of 3 segments all saying -860±70ms is much more trustworthy
    // than a cluster of 3 segments saying -50±15ms (which could be noise near 0).
    double best_score = -1.0;
    int best_idx = -1;

    for (size_t i = 0; i < clusters.size(); ++i) {
        const auto& c = clusters[i];

        // Offset magnitude factor: larger offsets get higher score
        // Normalize: 0 at 0ms, 1.0 at 500ms, plateau at ~1.5
        double offset_factor = std::min(1.5, std::abs(c.center_ms) / 500.0);

        // Tightness factor: lower MAD gets higher score
        // Perfect (0 MAD) = 1.0, 100ms MAD = 0.5, 200ms MAD = 0.0
        double tightness_factor = std::max(0.0, 1.0 - c.cluster_mad_ms / 200.0);

        // Size factor: more members = better
        double size_factor = std::log2(1.0 + c.cluster_size);

        // Confidence factor
        double conf_factor = c.cluster_confidence;

        double score = offset_factor * 2.0 + tightness_factor * 1.5 +
                       size_factor * 1.0 + conf_factor * 0.5;

        Log::Debug("Aggregator: cluster[%zu] center=%.1fms score=%.2f "
                   "(offset=%.2f, tight=%.2f, size=%.2f, conf=%.2f)",
                   i, c.center_ms, score,
                   offset_factor, tightness_factor, size_factor, conf_factor);

        if (score > best_score) {
            best_score = score;
            best_idx = static_cast<int>(i);
        }
    }

    if (best_idx >= 0) {
        const auto& best = clusters[best_idx];
        result.center_ms = best.center_ms;
        result.cluster_mad_ms = best.cluster_mad_ms;
        result.cluster_size = best.cluster_size;
        result.cluster_confidence = best.cluster_confidence;
    }

    return result;
}

}  // namespace avsync
