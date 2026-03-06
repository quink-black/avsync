#include "detector/SyncNetDetector.h"

#ifdef AVSYNC_ENABLE_SYNCNET

#include "common/Log.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numeric>

namespace avsync {

SyncNetDetector::SyncNetDetector(const SyncNetConfig& config)
    : config_(config) {
    // Load YuNet DNN face detection model.
    // The model file should be bundled in the project under models/.
    const std::string& model_path = config.face_detect_model;

    if (!std::filesystem::exists(model_path)) {
        Log::Warn("SyncNetDetector: face detection model not found at %s", model_path.c_str());
        return;
    }

    try {
        // Create YuNet face detector with a default input size;
        // actual size is set per-frame in DetectFaces().
        face_detector_ = cv::FaceDetectorYN::create(
            model_path,
            "",                    // config (not needed for YuNet ONNX model)
            cv::Size(320, 320),    // default input size, overridden per-frame
            0.7f,                  // score threshold
            0.3f,                  // NMS threshold
            5000                   // top-K before NMS
        );
        face_detector_loaded_ = true;
        Log::Info("SyncNetDetector: loaded YuNet face detector from %s", model_path.c_str());
    } catch (const cv::Exception& e) {
        Log::Warn("SyncNetDetector: failed to load YuNet model: %s", e.what());
    }
}

bool SyncNetDetector::CanHandle(const ContentFeatures& features) const {
    // SyncNet can handle if:
    // 1. Face detector is loaded
    // 2. There's enough audio energy (implies speech)
    // 3. There's some video motion (not a still image)
    if (!face_detector_loaded_) return false;
    if (features.audio_energy < 1e-6) return false;
    if (features.video_motion < 10.0) return false;
    return true;
}

std::vector<SyncNetDetector::FaceRect> SyncNetDetector::DetectFaces(
    const std::vector<uint8_t>& frame, int img_width, int img_height
) {
    std::vector<FaceRect> results;
    if (!face_detector_loaded_ || !face_detector_) return results;

    // YuNet expects BGR input
    cv::Mat rgb(img_height, img_width, CV_8UC3, const_cast<uint8_t*>(frame.data()));
    cv::Mat bgr;
    cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);

    // Update input size to match actual frame dimensions
    face_detector_->setInputSize(cv::Size(img_width, img_height));

    // Detect faces: output is a Mat of shape [num_faces, 15]
    // Columns 0-3: x, y, width, height of bounding box
    // Column 14: face score
    cv::Mat faces_mat;
    face_detector_->detect(bgr, faces_mat);

    if (faces_mat.empty()) return results;

    for (int i = 0; i < faces_mat.rows; ++i) {
        int w = static_cast<int>(faces_mat.at<float>(i, 2));
        int h = static_cast<int>(faces_mat.at<float>(i, 3));

        // Filter by minimum face size
        if (w < config_.face_min_size || h < config_.face_min_size) continue;

        FaceRect rect;
        rect.x = static_cast<int>(faces_mat.at<float>(i, 0));
        rect.y = static_cast<int>(faces_mat.at<float>(i, 1));
        rect.width = w;
        rect.height = h;
        results.push_back(rect);
    }

    return results;
}

SyncNetDetector::LipRegion SyncNetDetector::ExtractLipRegion(const FaceRect& face) const {
    // Lip region: lower portion of the face bounding box
    double ratio = config_.lip_region_ratio;
    int lip_y = face.y + static_cast<int>(face.height * (1.0 - ratio));
    int lip_height = static_cast<int>(face.height * ratio);
    // Slightly narrow the width to focus on mouth area
    int lip_x = face.x + face.width / 6;
    int lip_width = face.width * 2 / 3;

    return {lip_x, lip_y, lip_width, lip_height};
}

double SyncNetDetector::ComputeIoU(const FaceRect& a, const FaceRect& b) {
    int x1 = std::max(a.x, b.x);
    int y1 = std::max(a.y, b.y);
    int x2 = std::min(a.x + a.width, b.x + b.width);
    int y2 = std::min(a.y + a.height, b.y + b.height);

    if (x2 <= x1 || y2 <= y1) return 0.0;

    double inter = static_cast<double>(x2 - x1) * (y2 - y1);
    double area_a = static_cast<double>(a.width) * a.height;
    double area_b = static_cast<double>(b.width) * b.height;
    return inter / (area_a + area_b - inter);
}

SyncNetDetector::FaceRect SyncNetDetector::SelectDominantFace(
    const std::vector<std::vector<FaceRect>>& per_frame_faces
) const {
    // Build face tracks using IoU-based matching across frames.
    // Each track represents a unique physical face.
    constexpr double kIoUThreshold = 0.3;
    std::vector<FaceTrack> tracks;

    for (const auto& frame_faces : per_frame_faces) {
        // For each detected face in this frame, try to match to existing tracks
        std::vector<bool> matched_track(tracks.size(), false);

        for (const auto& face : frame_faces) {
            int best_track = -1;
            double best_iou = kIoUThreshold;

            for (size_t t = 0; t < tracks.size(); ++t) {
                if (matched_track[t]) continue;
                double iou = ComputeIoU(face, tracks[t].last_rect);
                if (iou > best_iou) {
                    best_iou = iou;
                    best_track = static_cast<int>(t);
                }
            }

            if (best_track >= 0) {
                // Update existing track
                tracks[best_track].last_rect = face;
                tracks[best_track].hit_count++;
                tracks[best_track].area_sum += static_cast<int64_t>(face.width) * face.height;
                matched_track[best_track] = true;
            } else {
                // Start a new track
                FaceTrack track;
                track.last_rect = face;
                track.hit_count = 1;
                track.area_sum = static_cast<int64_t>(face.width) * face.height;
                tracks.push_back(track);
            }
        }
    }

    if (tracks.empty()) {
        return {0, 0, 0, 0};
    }

    // Select the best track: prioritize frequency, break ties by average area
    auto best_it = std::max_element(tracks.begin(), tracks.end(),
        [](const FaceTrack& a, const FaceTrack& b) {
            if (a.hit_count != b.hit_count) return a.hit_count < b.hit_count;
            return (a.area_sum / a.hit_count) < (b.area_sum / b.hit_count);
        });

    Log::Debug("SyncNetDetector: selected dominant face track with %d hits, avg_area=%lld",
               best_it->hit_count,
               best_it->area_sum / best_it->hit_count);

    return best_it->last_rect;
}

std::vector<double> SyncNetDetector::ComputeLipMotion(
    const VideoSegment& video
) const {
    std::vector<double> motion;
    if (video.frames.size() < 2) return motion;

    size_t expected_frame_size = static_cast<size_t>(video.width) * video.height * 3;

    // Step 1: Detect faces in sampled frames to build tracks.
    // Sample every Nth frame to keep detection fast.
    constexpr size_t kSampleStep = 5;  // detect every 5th frame
    std::vector<std::vector<FaceRect>> sampled_faces;
    // Map from frame index -> detected faces (only for sampled frames)
    std::vector<std::pair<size_t, std::vector<FaceRect>>> frame_detections;

    for (size_t i = 0; i < video.frames.size(); i += kSampleStep) {
        std::vector<FaceRect> faces;
        if (video.frames[i]->size() == expected_frame_size) {
            faces = const_cast<SyncNetDetector*>(this)->DetectFaces(
                *video.frames[i], video.width, video.height);
        }
        sampled_faces.push_back(faces);
        frame_detections.push_back({i, faces});
    }

    // Step 2: Select the dominant face across the segment
    FaceRect dominant = SelectDominantFace(sampled_faces);
    if (dominant.width == 0 || dominant.height == 0) return motion;

    Log::Debug("SyncNetDetector: dominant face at (%d,%d,%dx%d)",
               dominant.x, dominant.y, dominant.width, dominant.height);

    // Step 3: Build per-frame face positions.
    // For sampled frames, find the best matching face.
    // For non-sampled frames, use the nearest sampled frame's face.
    // This avoids running detection on every frame (O(n/5) instead of O(n)).

    // Pre-compute the best face per sampled index
    auto find_best_face = [&](const std::vector<FaceRect>& faces) -> FaceRect {
        FaceRect best = dominant;
        double best_iou = 0.0;
        for (const auto& f : faces) {
            double iou = ComputeIoU(f, dominant);
            if (iou > best_iou) {
                best_iou = iou;
                best = f;
            }
        }
        // If no IoU match, pick the largest face
        if (best_iou < 0.2 && !faces.empty()) {
            auto largest = std::max_element(faces.begin(), faces.end(),
                [](const FaceRect& a, const FaceRect& b) {
                    return (a.width * a.height) < (b.width * b.height);
                });
            best = *largest;
        }
        return best;
    };

    // Build lookup: sampled frame index -> best face
    std::vector<std::pair<size_t, FaceRect>> keyframes;
    for (auto& [idx, faces] : frame_detections) {
        keyframes.push_back({idx, find_best_face(faces)});
    }

    // Resize target for lip region to ensure consistent size for diff
    int lip_target_w = std::max(32, dominant.width * 2 / 3);
    int lip_target_h = std::max(16, static_cast<int>(dominant.height * config_.lip_region_ratio));

    cv::Mat prev_lip_gray;

    for (size_t i = 0; i < video.frames.size(); ++i) {
        if (video.frames[i]->size() != expected_frame_size) {
            motion.push_back(0.0);
            continue;
        }

        // Find the nearest keyframe face for this frame index
        FaceRect face_for_frame = dominant;
        size_t best_dist = SIZE_MAX;
        for (auto& [kf_idx, kf_face] : keyframes) {
            size_t dist = (i >= kf_idx) ? (i - kf_idx) : (kf_idx - i);
            if (dist < best_dist) {
                best_dist = dist;
                face_for_frame = kf_face;
            }
        }

        cv::Mat rgb(video.height, video.width, CV_8UC3,
                    const_cast<uint8_t*>(video.frames[i]->data()));

        // Extract lip region from the tracked face
        LipRegion lip = ExtractLipRegion(face_for_frame);

        // Clamp to frame bounds
        int x1 = std::max(0, lip.x);
        int y1 = std::max(0, lip.y);
        int x2 = std::min(video.width, lip.x + lip.width);
        int y2 = std::min(video.height, lip.y + lip.height);

        if (x2 <= x1 || y2 <= y1) {
            motion.push_back(0.0);
            continue;
        }

        cv::Mat lip_roi = rgb(cv::Rect(x1, y1, x2 - x1, y2 - y1));
        cv::Mat lip_gray;
        cv::cvtColor(lip_roi, lip_gray, cv::COLOR_RGB2GRAY);
        // Resize to consistent dimensions for reliable diff
        cv::resize(lip_gray, lip_gray, cv::Size(lip_target_w, lip_target_h));

        if (!prev_lip_gray.empty()) {
            cv::Mat diff;
            cv::absdiff(lip_gray, prev_lip_gray, diff);
            double mean_diff = cv::mean(diff)[0];
            motion.push_back(mean_diff);
        } else {
            motion.push_back(0.0);
        }

        prev_lip_gray = lip_gray.clone();
    }

    // Step 4: Filter out scene-change spikes.
    // Scene changes cause very large lip-region diffs that corrupt the signal.
    // Suppress values above median + 3*MAD (robust outlier detection).
    if (motion.size() > 10) {
        std::vector<double> sorted_motion(motion);
        std::sort(sorted_motion.begin(), sorted_motion.end());
        double median = sorted_motion[sorted_motion.size() / 2];

        std::vector<double> abs_devs(motion.size());
        for (size_t i = 0; i < motion.size(); ++i) {
            abs_devs[i] = std::abs(motion[i] - median);
        }
        std::sort(abs_devs.begin(), abs_devs.end());
        double mad = abs_devs[abs_devs.size() / 2];

        double threshold = median + 4.0 * std::max(mad, 1.0);
        int suppressed = 0;
        for (auto& v : motion) {
            if (v > threshold) {
                v = 0.0;  // suppress scene-change frames
                suppressed++;
            }
        }
        if (suppressed > 0) {
            Log::Debug("SyncNetDetector: suppressed %d scene-change frames "
                       "(threshold=%.1f, median=%.1f, MAD=%.1f)",
                       suppressed, threshold, median, mad);
        }
    }

    return motion;
}

std::vector<double> SyncNetDetector::ComputeSpeechEnergy(
    const AudioSegment& audio,
    double frame_rate
) const {
    std::vector<double> energy;
    if (audio.samples.empty() || frame_rate <= 0.0) return energy;

    int channels = std::max(1, audio.channels);
    int total_mono = static_cast<int>(audio.samples.size()) / channels;
    int samples_per_frame = static_cast<int>(audio.sample_rate / frame_rate);

    if (samples_per_frame <= 0) return energy;

    for (int i = 0; i + samples_per_frame <= total_mono; i += samples_per_frame) {
        double e = 0.0;
        for (int j = 0; j < samples_per_frame; ++j) {
            float val = audio.samples[(i + j) * channels];
            e += static_cast<double>(val) * val;
        }
        energy.push_back(std::sqrt(e / samples_per_frame));
    }

    return energy;
}

SyncNetDetector::CorrelationResult SyncNetDetector::CrossCorrelateSignals(
    const std::vector<double>& signal_a,
    const std::vector<double>& signal_b,
    double frame_rate,
    double search_range_ms
) const {
    CorrelationResult result{0.0, 0.0};

    size_t len = std::min(signal_a.size(), signal_b.size());
    if (len < 10) return result;  // need enough samples

    // Normalize signals (zero mean, unit variance)
    auto normalize = [](const std::vector<double>& sig, size_t n) -> std::vector<double> {
        double mean = 0.0;
        for (size_t i = 0; i < n; ++i) mean += sig[i];
        mean /= n;

        double var = 0.0;
        for (size_t i = 0; i < n; ++i) {
            double d = sig[i] - mean;
            var += d * d;
        }
        double stddev = std::sqrt(var / n);
        if (stddev < 1e-10) stddev = 1.0;

        std::vector<double> result(n);
        for (size_t i = 0; i < n; ++i) {
            result[i] = (sig[i] - mean) / stddev;
        }
        return result;
    };

    auto norm_a = normalize(signal_a, len);
    auto norm_b = normalize(signal_b, len);

    // Search range in frames
    int max_shift = static_cast<int>(search_range_ms * frame_rate / 1000.0);
    max_shift = std::min(max_shift, static_cast<int>(len / 2));

    // Compute full cross-correlation curve
    std::vector<std::pair<int, double>> corr_curve;
    double best_corr = -1.0;
    int best_shift = 0;

    for (int shift = -max_shift; shift <= max_shift; ++shift) {
        double corr = 0.0;
        int count = 0;

        for (size_t i = 0; i < len; ++i) {
            int j = static_cast<int>(i) + shift;
            if (j >= 0 && j < static_cast<int>(len)) {
                corr += norm_a[i] * norm_b[j];
                count++;
            }
        }

        if (count > 0) {
            corr /= count;
            corr_curve.push_back({shift, corr});
            if (corr > best_corr) {
                best_corr = corr;
                best_shift = shift;
            }
        }
    }

    result.offset_ms = best_shift * 1000.0 / frame_rate;

    // Compute confidence using peak distinctiveness:
    // A reliable detection has one clear peak; noisy signals have many similar-height peaks.
    // We measure how much the best peak stands out from the mean correlation.
    if (corr_curve.size() > 1) {
        double sum = 0.0;
        double sum_sq = 0.0;
        for (auto& [s, c] : corr_curve) {
            sum += c;
            sum_sq += c * c;
        }
        double mean_corr = sum / corr_curve.size();
        double var_corr = sum_sq / corr_curve.size() - mean_corr * mean_corr;
        double std_corr = std::sqrt(std::max(0.0, var_corr));

        // Peak z-score: how many standard deviations above the mean is the peak?
        double peak_z = (std_corr > 1e-10) ? (best_corr - mean_corr) / std_corr : 0.0;

        // Confidence combines raw correlation strength with peak distinctiveness.
        // peak_z > 3 indicates a strong, distinct peak; < 2 means ambiguous.
        // Map peak_z from [1.5, 5] -> [0, 1] and multiply by raw correlation.
        double distinctiveness = std::max(0.0, std::min(1.0, (peak_z - 1.5) / 3.5));
        result.confidence = std::max(0.0, best_corr) * distinctiveness;

        Log::Debug("SyncNetDetector: xcorr peak=%.3f at %.1fms, peak_z=%.2f, "
                   "distinctiveness=%.2f, confidence=%.3f",
                   best_corr, result.offset_ms, peak_z,
                   distinctiveness, result.confidence);
    } else {
        result.confidence = 0.0;
    }

    return result;
}

std::vector<SegmentOffset> SyncNetDetector::Detect(
    const AudioSegment& audio,
    const VideoSegment& video
) {
    SegmentOffset result;
    result.start_time = audio.start_time;
    result.end_time = audio.end_time;
    result.method = "syncnet";
    result.skipped = false;

    // Step 1 + 2: Compute lip motion with per-frame face detection + tracking
    auto lip_motion = ComputeLipMotion(video);

    // Step 3: Compute speech energy envelope (aligned to video frame rate)
    auto speech_energy = ComputeSpeechEnergy(audio, video.fps);

    if (lip_motion.size() < 10 || speech_energy.size() < 10) {
        result.offset_ms = 0.0;
        result.confidence = 0.0;
        result.skipped = true;
        result.skip_reason = lip_motion.empty()
            ? "No face detected in video frames"
            : "Insufficient lip motion or speech data";
        Log::Info("SyncNetDetector: %s for segment [%.2f, %.2f] "
                  "(lip_motion=%zu, speech=%zu)",
                  result.skip_reason.c_str(),
                  audio.start_time, audio.end_time,
                  lip_motion.size(), speech_energy.size());
        return {result};
    }

    // Step 4: Smooth both signals to reduce noise before cross-correlation.
    // Apply a simple moving average filter.
    auto smooth = [](const std::vector<double>& sig, int window) -> std::vector<double> {
        std::vector<double> out(sig.size(), 0.0);
        int half = window / 2;
        for (size_t i = 0; i < sig.size(); ++i) {
            double sum = 0.0;
            int count = 0;
            for (int j = -half; j <= half; ++j) {
                int idx = static_cast<int>(i) + j;
                if (idx >= 0 && idx < static_cast<int>(sig.size())) {
                    sum += sig[idx];
                    count++;
                }
            }
            out[i] = sum / count;
        }
        return out;
    };

    // Smooth with ~5 frame window to remove frame-level noise
    auto smooth_lip = smooth(lip_motion, 5);
    auto smooth_energy = smooth(speech_energy, 5);

    // Step 5: Cross-correlate smoothed lip motion with speech energy
    // Use 2500ms search range to cover ±2000ms test offsets
    auto corr_result = CrossCorrelateSignals(
        smooth_lip, smooth_energy, video.fps, 2500.0);

    result.offset_ms = corr_result.offset_ms;
    result.confidence = corr_result.confidence;

    // Apply SyncNet-specific confidence threshold
    if (result.confidence < config_.confidence_threshold * 0.5) {
        result.skipped = true;
        result.skip_reason = "SyncNet confidence too low: " + std::to_string(result.confidence);
    }

    Log::Info("SyncNetDetector: segment [%.2f, %.2f] offset=%.1fms confidence=%.2f",
              result.start_time, result.end_time,
              result.offset_ms, result.confidence);

    return {result};
}

}  // namespace avsync

#endif  // AVSYNC_ENABLE_SYNCNET
