#pragma once

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace avsync {

// Dispatch mode for detector selection
enum class DispatchMode {
    Auto,      // auto-detect content features, choose best detector
    Force,     // force use specified detector
    Cascade    // try detectors in priority order, fallback on low confidence
};

// Configuration for onset-align detector
struct OnsetAlignConfig {
    double spectral_flux_threshold = 2.0;   // higher = fewer but stronger onsets
    double frame_diff_threshold = 30.0;
    int min_onset_count = 3;

    // Cross-correlation parameters
    double search_range_ms = 1200.0;   // search range for cross-correlation (ms), supports up to ~1.2s offset
    double resolution_ms = 5.0;        // step size in cross-correlation (ms)
    double match_tolerance_ms = 50.0;  // tolerance window for event matching (ms)
    double min_match_ratio = 0.15;     // minimum ratio of matched events for valid result
};

// Configuration for SyncNet detector
struct SyncNetConfig {
    std::string face_detect_model = "models/face_detection_yunet_2023mar.onnx";
    double confidence_threshold = 0.8;  // SyncNet requires higher confidence
    int face_min_size = 100;            // minimum face region size in pixels
    double lip_region_ratio = 0.3;      // lower-face ratio for lip region extraction
};

// Top-level configuration
struct Config {
    // Dispatch settings
    DispatchMode dispatch_mode = DispatchMode::Auto;
    std::string forced_detector;
    std::vector<std::string> cascade_order = {"syncnet", "onset_align"};

    // Thresholds
    double confidence_threshold = 0.3;
    double offset_threshold_ms = 40.0;

    // Safety threshold: minimum average confidence across all segments required
    // to apply ANY correction. Set moderately high to prevent false positives.
    // Cluster override can bypass this when a strong signal cluster is found.
    double min_global_confidence = 0.45;

    // Segmentation
    double segment_window_sec = 10.0;
    double segment_step_sec = 5.0;

    // Detector-specific configs
    OnsetAlignConfig onset_align;
    SyncNetConfig syncnet;

    // Manual offset mode: if set (not NaN), skip detection and apply this value directly.
    // Uses the same sign convention as detection offset:
    //   positive = audio is ahead of video (will shift video earlier to fix)
    //   negative = audio is behind video (will shift video later to fix)
    double manual_offset_ms = std::numeric_limits<double>::quiet_NaN();

    // Global constant offset strategy: assume the entire video has the same offset.
    // When true, all segments use the global consensus median rather than per-segment values.
    bool use_global_offset = true;

    // Logging
    std::string log_level = "info";
};

// Load configuration from JSON file. Falls back to defaults on failure.
Config LoadConfig(const std::string& path);

// Convert string to DispatchMode
DispatchMode ParseDispatchMode(const std::string& mode);

}  // namespace avsync
