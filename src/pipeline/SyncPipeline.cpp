#include "pipeline/SyncPipeline.h"
#include "common/Log.h"
#include "decoder/MediaDecoder.h"
#include "detector/SyncDetectorDispatcher.h"
#include "detector/OnsetAlignDetector.h"
#ifdef AVSYNC_ENABLE_SYNCNET
#include "detector/SyncNetDetector.h"
#endif
#include "aggregator/OffsetAggregator.h"
#include "corrector/TimestampCorrector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <numeric>

namespace avsync {

SyncPipeline::SyncPipeline() = default;
SyncPipeline::~SyncPipeline() = default;

void SyncPipeline::Configure(const Config& config) {
    config_ = config;

    // Set log level
    if (config_.log_level == "debug") {
        Log::SetLevel(LogLevel::Debug);
    } else if (config_.log_level == "warn") {
        Log::SetLevel(LogLevel::Warn);
    } else if (config_.log_level == "error") {
        Log::SetLevel(LogLevel::Error);
    } else {
        Log::SetLevel(LogLevel::Info);
    }
}

bool SyncPipeline::Process(const std::string& input_path, const std::string& output_path) {
    Log::Info("=== AV Auto-Sync Pipeline ===");
    Log::Info("Input:  %s", input_path.c_str());
    Log::Info("Output: %s", output_path.c_str());

    detection_results_.clear();
    correction_decisions_.clear();

    // ---- Manual offset mode: skip detection entirely ----
    if (!std::isnan(config_.manual_offset_ms)) {
        Log::Info("--- Manual offset mode: %.1fms ---", config_.manual_offset_ms);

        // Get duration from input file
        MediaDecoder decoder;
        if (!decoder.Open(input_path)) {
            Log::Error("Failed to open input file");
            return false;
        }
        double duration = decoder.GetDuration();

        // Create a single correction covering the entire file
        CorrectionDecision decision;
        decision.start_time = 0.0;
        decision.end_time = duration;
        decision.correction_ms = config_.manual_offset_ms;
        decision.apply = std::abs(config_.manual_offset_ms) >= config_.offset_threshold_ms;
        decision.reason = "Manual offset: " + std::to_string(config_.manual_offset_ms) + "ms";
        correction_decisions_.push_back(decision);

        PrintReport();

        // Apply correction
        TimestampCorrector corrector;
        if (!corrector.Correct(input_path, output_path, correction_decisions_)) {
            Log::Error("Failed to apply manual correction");
            return false;
        }

        Log::Info("=== Pipeline completed (manual mode) ===");
        return true;
    }

    // ---- Auto detection mode ----

    // Phase 1: Decode media
    Log::Info("--- Phase 1: Decoding media ---");
    MediaDecoder decoder;
    if (!decoder.Open(input_path)) {
        Log::Error("Failed to open input file");
        return false;
    }

    // Phase 2: Setup detectors and dispatcher
    Log::Info("--- Phase 2: Setting up detectors ---");
    SyncDetectorDispatcher dispatcher(config_);

    // Register available detectors
    auto onset_detector = std::make_shared<OnsetAlignDetector>(config_.onset_align);
    dispatcher.RegisterDetector(onset_detector);

#ifdef AVSYNC_ENABLE_SYNCNET
    // Register SyncNet detector (face/lip-based)
    auto syncnet_detector = std::make_shared<SyncNetDetector>(config_.syncnet);
    dispatcher.RegisterDetector(syncnet_detector);
#endif

    // Phase 2b: Stream-decode segments and run detection inline
    Log::Info("--- Phase 2b: Detecting offsets per segment (streaming) ---");

    // Use the streaming SegmentCallback: each segment is decoded on-the-fly
    // in a single continuous demux pass (no seeking). Detection runs inline
    // within the callback, and segment memory is released when it returns.
    // Peak memory = ~2 segments worth of audio + video (current + overlap).
    size_t segment_count = 0;
    bool decode_ok = decoder.DecodeSegments(
        config_.segment_window_sec,
        config_.segment_step_sec,
        [&](AudioSegment& audio, VideoSegment& video) {
            segment_count++;

            // Extract features and run detection for this segment
            auto features = ExtractFeatures(audio, video);
            auto offsets = dispatcher.Dispatch(audio, video, features);
            detection_results_.insert(detection_results_.end(), offsets.begin(), offsets.end());

            Log::Debug("Processed segment %zu [%.2f, %.2f)s: %zu audio, %zu frames",
                       segment_count, audio.start_time, audio.end_time,
                       audio.samples.size(), video.frames.size());
        }
    );

    if (!decode_ok) {
        Log::Error("Failed to decode segments");
        return false;
    }

    Log::Info("Processed %zu segments (window=%.1fs, step=%.1fs)",
              segment_count, config_.segment_window_sec, config_.segment_step_sec);

    // Phase 3: Aggregate offsets
    Log::Info("--- Phase 3: Aggregating offsets ---");
    OffsetAggregator aggregator(config_);
    correction_decisions_ = aggregator.Aggregate(detection_results_);

    // Phase 3b: If global constant offset is enabled, unify all corrections
    // to use the single consensus offset for the entire video.
    // SAFETY: Also verify that the detection results have sufficient overall confidence.
    if (config_.use_global_offset && !correction_decisions_.empty()) {
        // Compute average confidence from raw detection results
        double total_confidence = 0.0;
        int confidence_count = 0;
        for (const auto& r : detection_results_) {
            if (!r.skipped) {
                total_confidence += r.confidence;
                confidence_count++;
            }
        }
        double avg_confidence = (confidence_count > 0) ? total_confidence / confidence_count : 0.0;

        // Compute the global consensus offset from all applied corrections
        int count = 0;
        std::vector<double> applied_offsets;
        for (const auto& d : correction_decisions_) {
            if (d.apply) {
                applied_offsets.push_back(d.correction_ms);
                count++;
            }
        }

        if (count > 0) {
            // Use median of applied offsets as the global constant
            std::sort(applied_offsets.begin(), applied_offsets.end());
            double global_offset;
            if (applied_offsets.size() % 2 == 0) {
                global_offset = (applied_offsets[applied_offsets.size() / 2 - 1] +
                                 applied_offsets[applied_offsets.size() / 2]) / 2.0;
            } else {
                global_offset = applied_offsets[applied_offsets.size() / 2];
            }

            // SAFETY CHECK: If average confidence is below the global threshold,
            // do NOT apply corrections. Better to miss than to corrupt.
            if (avg_confidence < config_.min_global_confidence) {
                Log::Info("Global offset mode: SAFETY REJECT - avg confidence %.3f < %.3f, "
                          "NOT applying global offset %.1fms to prevent false positives",
                          avg_confidence, config_.min_global_confidence, global_offset);
                for (auto& d : correction_decisions_) {
                    d.correction_ms = 0.0;
                    d.apply = false;
                    d.reason = "Safety rejected: avg confidence too low (" +
                               std::to_string(avg_confidence) + " < " +
                               std::to_string(config_.min_global_confidence) + ")";
                }
            } else {
                Log::Info("Global constant offset mode: using %.1fms for all segments "
                          "(from %d valid segments, avg_confidence=%.3f)",
                          global_offset, count, avg_confidence);

                // Apply global offset to ALL segments (even previously skipped ones)
                for (auto& d : correction_decisions_) {
                    if (std::abs(global_offset) >= config_.offset_threshold_ms) {
                        d.correction_ms = global_offset;
                        d.apply = true;
                        d.reason = "Global constant offset: " + std::to_string(global_offset) + "ms";
                    } else {
                        d.correction_ms = 0.0;
                        d.apply = false;
                        d.reason = "Global offset within threshold";
                    }
                }
            }
        }
    }

    // Print report before correction
    PrintReport();

    // Phase 4: Apply corrections
    Log::Info("--- Phase 4: Applying timestamp corrections ---");
    TimestampCorrector corrector;
    if (!corrector.Correct(input_path, output_path, correction_decisions_)) {
        Log::Error("Failed to apply corrections");
        return false;
    }

    Log::Info("=== Pipeline completed successfully ===");
    return true;
}

ContentFeatures SyncPipeline::ExtractFeatures(
    const AudioSegment& audio,
    const VideoSegment& video
) const {
    ContentFeatures features;

    // Audio energy
    if (!audio.samples.empty()) {
        double energy = 0.0;
        for (float s : audio.samples) {
            energy += static_cast<double>(s) * s;
        }
        features.audio_energy = energy / audio.samples.size();
    }

    // Estimate audio onset count (simple zero-crossing + energy approach)
    if (!audio.samples.empty() && audio.sample_rate > 0) {
        // Simple energy-based onset estimation
        int hop = audio.sample_rate / 20;  // 50ms hop
        int window = audio.sample_rate / 10;  // 100ms window
        int channels = std::max(1, audio.channels);
        int total_mono_samples = static_cast<int>(audio.samples.size()) / channels;

        std::vector<double> frame_energies;
        for (int i = 0; i + window <= total_mono_samples; i += hop) {
            double e = 0.0;
            for (int j = 0; j < window; ++j) {
                float val = audio.samples[(i + j) * channels];
                e += val * val;
            }
            frame_energies.push_back(e / window);
        }

        // Compute positive energy flux (half-wave rectified differences)
        std::vector<double> flux;
        for (size_t i = 1; i < frame_energies.size(); ++i) {
            double diff = frame_energies[i] - frame_energies[i - 1];
            flux.push_back(std::max(0.0, diff));
        }

        // Adaptive threshold: mean + 1.0 * stddev of positive flux
        if (!flux.empty()) {
            double flux_mean = std::accumulate(flux.begin(), flux.end(), 0.0) / flux.size();
            double flux_sq_sum = 0.0;
            for (double f : flux) {
                flux_sq_sum += (f - flux_mean) * (f - flux_mean);
            }
            double flux_stddev = std::sqrt(flux_sq_sum / flux.size());
            double onset_threshold = flux_mean + 1.0 * flux_stddev;

            int onset_count = 0;
            for (size_t i = 1; i + 1 < flux.size(); ++i) {
                // Peak picking: local maximum above threshold
                if (flux[i] > onset_threshold &&
                    flux[i] > flux[i - 1] &&
                    flux[i] > flux[i + 1]) {
                    onset_count++;
                }
            }
            features.audio_onset_count = onset_count;
        }
    }

    // Video motion estimation (average frame difference)
    if (video.frames.size() >= 2) {
        double total_motion = 0.0;
        size_t expected_size = static_cast<size_t>(video.width) * video.height * 3;
        int comparisons = 0;

        for (size_t i = 1; i < video.frames.size(); ++i) {
            const auto& curr_frame = *video.frames[i];
            const auto& prev_frame = *video.frames[i - 1];
            if (curr_frame.size() == expected_size &&
                prev_frame.size() == expected_size) {
                double diff = 0.0;
                for (size_t p = 0; p < expected_size; p += 3) {
                    // Use luminance approximation
                    int lum_curr = static_cast<int>(curr_frame[p]) * 77 +
                                   static_cast<int>(curr_frame[p + 1]) * 150 +
                                   static_cast<int>(curr_frame[p + 2]) * 29;
                    int lum_prev = static_cast<int>(prev_frame[p]) * 77 +
                                   static_cast<int>(prev_frame[p + 1]) * 150 +
                                   static_cast<int>(prev_frame[p + 2]) * 29;
                    diff += std::abs(lum_curr - lum_prev);
                }
                total_motion += diff / (expected_size / 3);
                comparisons++;
            }
        }

        if (comparisons > 0) {
            features.video_motion = total_motion / comparisons;
        }

        // Count visual events (frames with above-average motion)
        if (comparisons > 0) {
            double avg_motion = total_motion / comparisons;
            int event_count = 0;
            double prev_diff = 0.0;
            for (size_t i = 1; i < video.frames.size(); ++i) {
                const auto& curr_frame = *video.frames[i];
                const auto& prev_frame = *video.frames[i - 1];
                if (curr_frame.size() == expected_size &&
                    prev_frame.size() == expected_size) {
                    double diff = 0.0;
                    for (size_t p = 0; p < expected_size; p += 3) {
                        int lum_curr = static_cast<int>(curr_frame[p]) * 77 +
                                       static_cast<int>(curr_frame[p + 1]) * 150 +
                                       static_cast<int>(curr_frame[p + 2]) * 29;
                        int lum_prev = static_cast<int>(prev_frame[p]) * 77 +
                                       static_cast<int>(prev_frame[p + 1]) * 150 +
                                       static_cast<int>(prev_frame[p + 2]) * 29;
                        diff += std::abs(lum_curr - lum_prev);
                    }
                    diff /= (expected_size / 3);
                    if (diff > avg_motion * 2.0 && diff > prev_diff) {
                        event_count++;
                    }
                    prev_diff = diff;
                }
            }
            features.video_event_count = event_count;
        }
    }

    // Face detection placeholder (requires OpenCV, enabled with AVSYNC_ENABLE_SYNCNET)
    features.has_face = false;
    features.has_speech = false;

    Log::Debug("Features: energy=%.6f, onsets=%d, motion=%.1f, events=%d, face=%d, speech=%d",
               features.audio_energy, features.audio_onset_count,
               features.video_motion, features.video_event_count,
               features.has_face, features.has_speech);

    return features;
}

const std::vector<SegmentOffset>& SyncPipeline::GetDetectionResults() const {
    return detection_results_;
}

const std::vector<CorrectionDecision>& SyncPipeline::GetCorrectionDecisions() const {
    return correction_decisions_;
}

void SyncPipeline::PrintReport() const {
    std::printf("\n========== AV Sync Detection Report ==========\n");
    std::printf("Offset sign convention: positive = audio ahead of video\n");
    std::printf("Correction: positive = shift video earlier to fix\n\n");

    // Detection results
    std::printf("Detection Results (%zu segments):\n", detection_results_.size());
    std::printf("%-12s %-12s %-12s %-12s %-8s %s\n",
                "Start(s)", "End(s)", "Offset(ms)", "Confidence", "Status", "Method");
    std::printf("-------------------------------------------------------------------\n");

    for (const auto& r : detection_results_) {
        const char* status = r.skipped ? "SKIP" : "OK";
        std::printf("%-12.2f %-12.2f %-12.1f %-12.2f %-8s %s",
                    r.start_time, r.end_time, r.offset_ms, r.confidence, status, r.method.c_str());
        if (r.skipped) {
            std::printf(" (%s)", r.skip_reason.c_str());
        }
        std::printf("\n");
    }

    // Correction decisions
    std::printf("\nCorrection Decisions (%zu segments):\n", correction_decisions_.size());
    std::printf("%-12s %-12s %-15s %-8s %s\n",
                "Start(s)", "End(s)", "Correction(ms)", "Apply", "Reason");
    std::printf("-------------------------------------------------------------------\n");

    int applied_count = 0;
    int skipped_count = 0;

    for (const auto& d : correction_decisions_) {
        const char* apply_str = d.apply ? "YES" : "NO";
        std::printf("%-12.2f %-12.2f %-15.1f %-8s %s\n",
                    d.start_time, d.end_time, d.correction_ms, apply_str, d.reason.c_str());
        if (d.apply) applied_count++;
        else skipped_count++;
    }

    std::printf("\nSummary: %d segments corrected, %d segments skipped\n",
                applied_count, skipped_count);
    std::printf("================================================\n\n");
}

}  // namespace avsync
