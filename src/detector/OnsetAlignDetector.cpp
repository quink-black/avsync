#include "detector/OnsetAlignDetector.h"
#include "common/Log.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace avsync {

OnsetAlignDetector::OnsetAlignDetector(const OnsetAlignConfig& config)
    : config_(config) {
}

std::vector<SegmentOffset> OnsetAlignDetector::Detect(
    const AudioSegment& audio,
    const VideoSegment& video
) {
    Log::Debug("OnsetAlignDetector: analyzing segment [%.2f, %.2f]",
               audio.start_time, audio.end_time);

    // Step 1: detect audio onsets
    auto audio_onsets = DetectAudioOnsets(audio);
    Log::Debug("OnsetAlignDetector: found %zu audio onsets", audio_onsets.size());

    // Step 2: detect visual events
    auto visual_events = DetectVisualEvents(video);
    Log::Debug("OnsetAlignDetector: found %zu visual events", visual_events.size());

    // Step 3: check if we have enough events to correlate
    // Need at least 2 events in EACH modality for meaningful cross-correlation
    if (audio_onsets.size() < 2 || visual_events.empty()) {
        // If we have enough audio onsets but few visual events (or vice versa),
        // still try if at least one modality has >= 2 events
        if (audio_onsets.empty() && visual_events.empty()) {
            Log::Debug("OnsetAlignDetector: no events detected in either modality");
            SegmentOffset result;
            result.start_time = audio.start_time;
            result.end_time = audio.end_time;
            result.offset_ms = 0.0;
            result.confidence = 0.0;
            result.method = Name();
            result.skipped = true;
            result.skip_reason = "No audio onsets or visual events detected";
            return {result};
        }
        // If one modality is weak, still proceed but with lower confidence expectation
        Log::Debug("OnsetAlignDetector: limited events (audio=%zu, video=%zu), proceeding with caution",
                   audio_onsets.size(), visual_events.size());
    }

    // Step 4: cross-correlate event sequences to find offset
    auto align_result = CrossCorrelateEvents(audio_onsets, visual_events);

    SegmentOffset result;
    result.start_time = audio.start_time;
    result.end_time = audio.end_time;
    result.offset_ms = align_result.offset_ms;
    result.confidence = align_result.confidence;
    result.method = Name();

    // Check minimum match ratio — if too few events matched, mark as low confidence
    if (align_result.total_events > 0) {
        double match_ratio = static_cast<double>(align_result.matched_pairs) / align_result.total_events;
        if (match_ratio < config_.min_match_ratio) {
            result.skipped = true;
            result.skip_reason = "Low match ratio: " + std::to_string(align_result.matched_pairs) +
                                 "/" + std::to_string(align_result.total_events) +
                                 " (" + std::to_string(match_ratio) + " < " +
                                 std::to_string(config_.min_match_ratio) + ")";
            Log::Info("OnsetAlignDetector: segment [%.2f, %.2f] low match ratio %.2f, skipping",
                      audio.start_time, audio.end_time, match_ratio);
            return {result};
        }
    }

    result.skipped = false;

    Log::Info("OnsetAlignDetector: segment [%.2f, %.2f] offset=%.1fms confidence=%.2f matched=%d/%d",
              audio.start_time, audio.end_time, align_result.offset_ms, align_result.confidence,
              align_result.matched_pairs, align_result.total_events);

    return {result};
}

bool OnsetAlignDetector::CanHandle(const ContentFeatures& features) const {
    // Can handle segments with sufficient audio onsets OR visual events
    // At least one modality must have enough events for correlation
    bool has_audio = features.audio_onset_count >= config_.min_onset_count;
    bool has_video = features.video_event_count >= 2;
    bool has_energy = features.audio_energy > 1e-6;  // not silent
    bool has_motion = features.video_motion > 1.0;    // not static
    return (has_audio || has_video) && (has_energy || has_motion);
}

std::vector<double> OnsetAlignDetector::DetectAudioOnsets(const AudioSegment& audio) const {
    std::vector<double> onsets;

    if (audio.samples.empty() || audio.sample_rate <= 0) {
        return onsets;
    }

    // Convert to mono if multi-channel
    std::vector<float> mono_samples;
    if (audio.channels > 1) {
        mono_samples.resize(audio.samples.size() / audio.channels);
        for (size_t i = 0; i < mono_samples.size(); ++i) {
            float sum = 0.0f;
            for (int ch = 0; ch < audio.channels; ++ch) {
                sum += audio.samples[i * audio.channels + ch];
            }
            mono_samples[i] = sum / audio.channels;
        }
    } else {
        mono_samples = audio.samples;
    }

    // Compute spectral flux
    constexpr int kHopSize = 512;
    constexpr int kWindowSize = 1024;
    auto spectral_flux = ComputeSpectralFlux(mono_samples, audio.sample_rate, kHopSize, kWindowSize);

    if (spectral_flux.empty()) {
        return onsets;
    }

    // Peak picking: find local maxima above threshold
    // Use adaptive threshold: mean + threshold_factor * stddev
    double mean = std::accumulate(spectral_flux.begin(), spectral_flux.end(), 0.0) / spectral_flux.size();
    double sq_sum = 0.0;
    for (double v : spectral_flux) {
        sq_sum += (v - mean) * (v - mean);
    }
    double stddev = std::sqrt(sq_sum / spectral_flux.size());
    double threshold = mean + config_.spectral_flux_threshold * stddev;

    double hop_duration = static_cast<double>(kHopSize) / audio.sample_rate;

    // Collect all onset candidates with their peak strength
    struct OnsetCandidate {
        double time;
        double strength;
    };
    std::vector<OnsetCandidate> candidates;

    for (size_t i = 1; i + 1 < spectral_flux.size(); ++i) {
        if (spectral_flux[i] > threshold &&
            spectral_flux[i] > spectral_flux[i - 1] &&
            spectral_flux[i] > spectral_flux[i + 1]) {
            double onset_time = audio.start_time + i * hop_duration;
            candidates.push_back({onset_time, spectral_flux[i]});
        }
    }

    // Keep only the top-N strongest onsets to avoid over-saturated matching
    // When too many onsets exist, cross-correlation becomes undiscriminating
    constexpr int kMaxOnsets = 20;
    if (static_cast<int>(candidates.size()) > kMaxOnsets) {
        std::sort(candidates.begin(), candidates.end(),
                  [](const OnsetCandidate& a, const OnsetCandidate& b) {
                      return a.strength > b.strength;
                  });
        candidates.resize(kMaxOnsets);
        // Re-sort by time for cross-correlation
        std::sort(candidates.begin(), candidates.end(),
                  [](const OnsetCandidate& a, const OnsetCandidate& b) {
                      return a.time < b.time;
                  });
    }

    for (const auto& c : candidates) {
        onsets.push_back(c.time);
    }

    return onsets;
}

std::vector<double> OnsetAlignDetector::DetectVisualEvents(const VideoSegment& video) const {
    std::vector<double> events;

    if (video.frames.size() < 2 || video.fps <= 0) {
        return events;
    }

    auto frame_diffs = ComputeFrameDifferences(video);

    if (frame_diffs.empty()) {
        return events;
    }

    // Adaptive threshold for visual events: mean + factor * stddev
    double mean = std::accumulate(frame_diffs.begin(), frame_diffs.end(), 0.0) / frame_diffs.size();
    double sq_sum = 0.0;
    for (double v : frame_diffs) {
        sq_sum += (v - mean) * (v - mean);
    }
    double stddev = std::sqrt(sq_sum / frame_diffs.size());
    // Use stddev-based adaptive threshold (frame_diff_threshold is used as stddev multiplier)
    // Typical pixel diff mean is 2-10, stddev 1-5, so mean + 1.5*stddev is reasonable
    double threshold = mean + 1.5 * stddev;
    // Ensure minimum threshold to avoid noise
    if (threshold < mean * 1.5) {
        threshold = mean * 1.5;
    }

    double frame_duration = 1.0 / video.fps;

    for (size_t i = 1; i + 1 < frame_diffs.size(); ++i) {
        if (frame_diffs[i] > threshold &&
            frame_diffs[i] > frame_diffs[i - 1] &&
            frame_diffs[i] > frame_diffs[i + 1]) {
            // Event time = start_time + frame_index * frame_duration
            double event_time = video.start_time + (i + 1) * frame_duration;
            events.push_back(event_time);
        }
    }

    return events;
}

OnsetAlignDetector::AlignResult OnsetAlignDetector::CrossCorrelateEvents(
    const std::vector<double>& audio_events,
    const std::vector<double>& video_events
) const {
    AlignResult best;
    best.offset_ms = 0.0;
    best.confidence = 0.0;
    best.matched_pairs = 0;
    best.total_events = 0;

    if (audio_events.empty() || video_events.empty()) {
        return best;
    }

    // Use config parameters instead of hardcoded values
    double search_range = config_.search_range_ms / 1000.0;
    double resolution = config_.resolution_ms / 1000.0;
    double tolerance = config_.match_tolerance_ms / 1000.0;

    double best_score = 0.0;
    double best_offset = 0.0;
    int best_matches = 0;
    double second_best_score = 0.0;

    int num_steps = static_cast<int>(2.0 * search_range / resolution) + 1;
    int min_events = static_cast<int>(std::min(audio_events.size(), video_events.size()));

    // Track all scores for non-neighboring second-best analysis
    std::vector<double> all_scores(num_steps, 0.0);
    std::vector<int> all_matches(num_steps, 0);

    for (int step = 0; step < num_steps; ++step) {
        double candidate_offset = -search_range + step * resolution;
        double score = 0.0;
        int matches = 0;

        // For each audio event, find the best matching video event
        for (double a_time : audio_events) {
            double shifted_time = a_time + candidate_offset;
            double best_diff = tolerance + 1.0;  // sentinel

            for (double v_time : video_events) {
                double diff = std::abs(shifted_time - v_time);
                if (diff < best_diff) {
                    best_diff = diff;
                }
            }

            if (best_diff < tolerance) {
                // Gaussian-weighted score: closer matches get higher score
                double normalized = best_diff / tolerance;
                score += std::exp(-2.0 * normalized * normalized);
                matches++;
            }
        }

        all_scores[step] = score;
        all_matches[step] = matches;

        if (score > best_score) {
            best_score = score;
            best_offset = candidate_offset;
            best_matches = matches;
        }
    }

    // Find the best step index
    int best_step = 0;
    for (int i = 0; i < num_steps; ++i) {
        if (all_scores[i] == best_score) {
            best_step = i;
            break;
        }
    }

    // Find second-best score OUTSIDE the neighborhood of the best peak.
    // Neighborhood = ±200ms in offset space (±200/resolution steps).
    // This ensures we measure the peak's distinctness against unrelated offsets,
    // not just nearby noise in the same peak.
    int neighborhood_steps = static_cast<int>(200.0 / (config_.resolution_ms)) + 1;
    for (int step = 0; step < num_steps; ++step) {
        if (std::abs(step - best_step) > neighborhood_steps) {
            if (all_scores[step] > second_best_score) {
                second_best_score = all_scores[step];
            }
        }
    }

    // Cap matched_pairs to min_events (a single video event can't match
    // multiple audio events in a valid pairing)
    best_matches = std::min(best_matches, min_events);

    // Confidence calculation with emphasis on peak distinctness:
    // 1. Match ratio: what fraction of events found a match
    double match_ratio = (min_events > 0) ? static_cast<double>(best_matches) / min_events : 0.0;

    // 2. Peak distinctness: how much better is the best offset vs the best NON-NEIGHBORING offset.
    //    A reliable detection needs a clear, isolated peak. peak_ratio < 1.1 means noise.
    double peak_ratio = (second_best_score > 0.0) ? best_score / second_best_score : 2.0;
    // Stricter normalization: need peak_ratio >= 1.3 for full credit, < 1.05 gives zero
    double peak_factor = std::min(1.0, std::max(0.0, (peak_ratio - 1.05) / 0.25));

    // 3. Score density: average match quality
    double score_density = (best_matches > 0) ? (best_score / best_matches) : 0.0;

    // Combined confidence — peak distinctness gets highest weight because
    // it's the strongest indicator of a real vs. noise-driven result.
    best.confidence = match_ratio * 0.3 + peak_factor * 0.5 + score_density * 0.2;
    best.confidence = std::min(best.confidence, 1.0);
    best.confidence = std::max(best.confidence, 0.0);

    // Penalize if very few matches
    if (best_matches < 2) {
        best.confidence *= 0.3;
    } else if (best_matches < 4) {
        best.confidence *= 0.6;
    }

    // Heavily penalize when there are very few events in either modality.
    // With only 1-3 events, even random alignment produces high match_ratio,
    // making the result unreliable. This prevents low-event segments from
    // forming false clusters with spuriously high confidence.
    if (min_events <= 2) {
        best.confidence *= 0.2;
    } else if (min_events <= 4) {
        best.confidence *= 0.5;
    }

    best.offset_ms = best_offset * 1000.0;
    best.matched_pairs = best_matches;
    best.total_events = min_events;

    Log::Debug("CrossCorrelate: offset=%.1fms score=%.2f matches=%d/%d peak_ratio=%.2f confidence=%.2f",
               best.offset_ms, best_score, best_matches, min_events, peak_ratio, best.confidence);

    return best;
}

std::vector<double> OnsetAlignDetector::ComputeSpectralFlux(
    const std::vector<float>& samples,
    int sample_rate,
    int hop_size,
    int window_size
) const {
    std::vector<double> flux;

    if (static_cast<int>(samples.size()) < window_size) {
        return flux;
    }

    // Simple spectral flux using energy difference between consecutive frames
    // (Full FFT-based spectral flux can be added later for better accuracy)
    int num_frames = (static_cast<int>(samples.size()) - window_size) / hop_size + 1;
    if (num_frames < 2) {
        return flux;
    }

    // Compute energy per frame
    std::vector<double> frame_energy(num_frames);
    for (int f = 0; f < num_frames; ++f) {
        int offset = f * hop_size;
        double energy = 0.0;
        for (int i = 0; i < window_size; ++i) {
            double val = samples[offset + i];
            energy += val * val;
        }
        frame_energy[f] = energy / window_size;
    }

    // Spectral flux = positive difference in energy between consecutive frames
    flux.resize(num_frames - 1);
    for (int f = 0; f < num_frames - 1; ++f) {
        double diff = frame_energy[f + 1] - frame_energy[f];
        flux[f] = std::max(0.0, diff);  // half-wave rectification
    }

    return flux;
}

std::vector<double> OnsetAlignDetector::ComputeFrameDifferences(const VideoSegment& video) const {
    std::vector<double> diffs;

    if (video.frames.size() < 2) {
        return diffs;
    }

    size_t expected_size = static_cast<size_t>(video.width) * video.height * 3;

    for (size_t i = 1; i < video.frames.size(); ++i) {
        const auto& prev = *video.frames[i - 1];
        const auto& curr = *video.frames[i];

        if (prev.size() != expected_size || curr.size() != expected_size) {
            diffs.push_back(0.0);
            continue;
        }

        // Sum of absolute pixel differences, normalized
        double total_diff = 0.0;
        for (size_t p = 0; p < expected_size; ++p) {
            total_diff += std::abs(static_cast<int>(curr[p]) - static_cast<int>(prev[p]));
        }
        diffs.push_back(total_diff / expected_size);
    }

    return diffs;
}

}  // namespace avsync
