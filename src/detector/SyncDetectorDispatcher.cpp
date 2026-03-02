#include "detector/SyncDetectorDispatcher.h"
#include "common/Log.h"

#include <algorithm>

namespace avsync {

SyncDetectorDispatcher::SyncDetectorDispatcher(const Config& config)
    : config_(config) {
}

void SyncDetectorDispatcher::RegisterDetector(std::shared_ptr<ISyncDetector> detector) {
    Log::Info("Registered detector: %s", detector->Name().c_str());
    detectors_.push_back(std::move(detector));
}

std::vector<SegmentOffset> SyncDetectorDispatcher::Dispatch(
    const AudioSegment& audio,
    const VideoSegment& video,
    const ContentFeatures& features
) {
    switch (config_.dispatch_mode) {
    case DispatchMode::Force:
        return DispatchForce(audio, video);
    case DispatchMode::Cascade:
        return DispatchCascade(audio, video, features);
    case DispatchMode::Auto:
    default:
        return DispatchAuto(audio, video, features);
    }
}

std::vector<SegmentOffset> SyncDetectorDispatcher::DispatchAuto(
    const AudioSegment& audio,
    const VideoSegment& video,
    const ContentFeatures& features
) {
    // Priority: SyncNet (if face+speech detected) > OnsetAlign (if events detected) > skip
    std::vector<SegmentOffset> best_results;
    double best_confidence = -1.0;

    for (auto& detector : detectors_) {
        if (detector->CanHandle(features)) {
            Log::Debug("Auto dispatch: trying %s for segment [%.2f, %.2f]",
                       detector->Name().c_str(), audio.start_time, audio.end_time);
            auto results = detector->Detect(audio, video);
            if (!results.empty()) {
                // If meets confidence threshold, use immediately
                if (MeetsConfidenceThreshold(results)) {
                    return results;
                }
                // Track the best result even if below threshold
                double avg_conf = 0.0;
                for (const auto& r : results) avg_conf += r.confidence;
                avg_conf /= results.size();
                if (avg_conf > best_confidence) {
                    best_confidence = avg_conf;
                    best_results = results;
                }
                Log::Debug("Auto dispatch: %s confidence %.2f below threshold %.2f, trying next",
                           detector->Name().c_str(), avg_conf, config_.confidence_threshold);
            }
        }
    }

    // If we have a best result (even low confidence), pass it through
    // so the aggregator can make the final decision
    if (!best_results.empty() && best_confidence > 0.0) {
        Log::Info("Auto dispatch: using best available result for [%.2f, %.2f] (confidence=%.2f)",
                  audio.start_time, audio.end_time, best_confidence);
        return best_results;
    }

    // No suitable detector found at all — mark as skipped
    Log::Info("No suitable detector for segment [%.2f, %.2f], skipping",
              audio.start_time, audio.end_time);
    SegmentOffset skipped;
    skipped.start_time = audio.start_time;
    skipped.end_time = audio.end_time;
    skipped.offset_ms = 0.0;
    skipped.confidence = 0.0;
    skipped.method = "none";
    skipped.skipped = true;
    skipped.skip_reason = "No suitable detector: insufficient content features (silent/static segment)";
    return {skipped};
}

std::vector<SegmentOffset> SyncDetectorDispatcher::DispatchForce(
    const AudioSegment& audio,
    const VideoSegment& video
) {
    for (auto& detector : detectors_) {
        if (detector->Name() == config_.forced_detector) {
            Log::Debug("Force dispatch: using %s", detector->Name().c_str());
            return detector->Detect(audio, video);
        }
    }

    Log::Error("Forced detector '%s' not found", config_.forced_detector.c_str());
    SegmentOffset skipped;
    skipped.start_time = audio.start_time;
    skipped.end_time = audio.end_time;
    skipped.offset_ms = 0.0;
    skipped.confidence = 0.0;
    skipped.method = "none";
    skipped.skipped = true;
    skipped.skip_reason = "Forced detector '" + config_.forced_detector + "' not registered";
    return {skipped};
}

std::vector<SegmentOffset> SyncDetectorDispatcher::DispatchCascade(
    const AudioSegment& audio,
    const VideoSegment& video,
    const ContentFeatures& features
) {
    // Try detectors in cascade_order, use first one that meets confidence threshold
    for (const auto& detector_name : config_.cascade_order) {
        for (auto& detector : detectors_) {
            if (detector->Name() == detector_name && detector->CanHandle(features)) {
                Log::Debug("Cascade dispatch: trying %s for segment [%.2f, %.2f]",
                           detector->Name().c_str(), audio.start_time, audio.end_time);
                auto results = detector->Detect(audio, video);
                if (!results.empty() && MeetsConfidenceThreshold(results)) {
                    Log::Debug("Cascade dispatch: %s succeeded", detector->Name().c_str());
                    return results;
                }
                Log::Debug("Cascade dispatch: %s confidence too low, falling back",
                           detector->Name().c_str());
                break;
            }
        }
    }

    // All detectors failed or below threshold — mark as skipped
    Log::Info("Cascade dispatch: all detectors failed for segment [%.2f, %.2f], skipping",
              audio.start_time, audio.end_time);
    SegmentOffset skipped;
    skipped.start_time = audio.start_time;
    skipped.end_time = audio.end_time;
    skipped.offset_ms = 0.0;
    skipped.confidence = 0.0;
    skipped.method = "none";
    skipped.skipped = true;
    skipped.skip_reason = "All cascade detectors failed or below confidence threshold";
    return {skipped};
}

bool SyncDetectorDispatcher::MeetsConfidenceThreshold(
    const std::vector<SegmentOffset>& results
) const {
    if (results.empty()) return false;
    // Check if the average confidence meets threshold
    double total_confidence = 0.0;
    for (const auto& r : results) {
        total_confidence += r.confidence;
    }
    return (total_confidence / results.size()) >= config_.confidence_threshold;
}

}  // namespace avsync
