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
            "",                    // config (not needed for ONNX)
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

std::vector<double> SyncNetDetector::ComputeLipMotion(
    const VideoSegment& video,
    const std::vector<FaceRect>& faces
) const {
    std::vector<double> motion;
    if (faces.empty() || video.frames.size() < 2) return motion;

    // Use the first (largest) detected face for all frames
    const FaceRect& face = faces[0];
    LipRegion lip = ExtractLipRegion(face);

    size_t expected_frame_size = static_cast<size_t>(video.width) * video.height * 3;

    cv::Mat prev_lip_gray;

    for (size_t i = 0; i < video.frames.size(); ++i) {
        if (video.frames[i]->size() != expected_frame_size) {
            motion.push_back(0.0);
            continue;
        }

        cv::Mat rgb(video.height, video.width, CV_8UC3,
                    const_cast<uint8_t*>(video.frames[i]->data()));

        // Clamp lip region to frame bounds
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

        if (!prev_lip_gray.empty() && prev_lip_gray.size() == lip_gray.size()) {
            cv::Mat diff;
            cv::absdiff(lip_gray, prev_lip_gray, diff);
            double mean_diff = cv::mean(diff)[0];
            motion.push_back(mean_diff);
        } else {
            motion.push_back(0.0);
        }

        prev_lip_gray = lip_gray.clone();
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
            if (corr > best_corr) {
                best_corr = corr;
                best_shift = shift;
            }
        }
    }

    result.offset_ms = best_shift * 1000.0 / frame_rate;
    result.confidence = std::max(0.0, best_corr);

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

    // Step 1: Detect face in a sample of frames
    std::vector<FaceRect> faces;
    if (!video.frames.empty()) {
        size_t expected_size = static_cast<size_t>(video.width) * video.height * 3;

        // Sample a few frames to find stable face detection
        std::vector<size_t> sample_indices;
        size_t step = std::max<size_t>(1, video.frames.size() / 10);
        for (size_t i = 0; i < video.frames.size(); i += step) {
            sample_indices.push_back(i);
        }

        // Find the best face (one that appears in most frames)
        std::vector<FaceRect> all_faces;
        for (size_t idx : sample_indices) {
            if (video.frames[idx]->size() == expected_size) {
                auto frame_faces = DetectFaces(*video.frames[idx], video.width, video.height);
                if (!frame_faces.empty()) {
                    all_faces.push_back(frame_faces[0]);  // take largest face
                }
            }
        }

        if (!all_faces.empty()) {
            // Use the median face position for stability
            std::sort(all_faces.begin(), all_faces.end(),
                      [](const FaceRect& a, const FaceRect& b) {
                          return (a.width * a.height) > (b.width * b.height);
                      });
            faces.push_back(all_faces[all_faces.size() / 2]);
        }
    }

    if (faces.empty()) {
        result.offset_ms = 0.0;
        result.confidence = 0.0;
        result.skipped = true;
        result.skip_reason = "No face detected in video frames";
        Log::Info("SyncNetDetector: no face in segment [%.2f, %.2f]",
                  audio.start_time, audio.end_time);
        return {result};
    }

    Log::Debug("SyncNetDetector: face detected at (%d,%d,%d,%d)",
               faces[0].x, faces[0].y, faces[0].width, faces[0].height);

    // Step 2: Compute lip motion signal
    auto lip_motion = ComputeLipMotion(video, faces);

    // Step 3: Compute speech energy envelope (aligned to video frame rate)
    auto speech_energy = ComputeSpeechEnergy(audio, video.fps);

    if (lip_motion.size() < 10 || speech_energy.size() < 10) {
        result.offset_ms = 0.0;
        result.confidence = 0.0;
        result.skipped = true;
        result.skip_reason = "Insufficient lip motion or speech data";
        Log::Info("SyncNetDetector: insufficient data for segment [%.2f, %.2f] "
                  "(lip_motion=%zu, speech=%zu)",
                  audio.start_time, audio.end_time,
                  lip_motion.size(), speech_energy.size());
        return {result};
    }

    // Step 4: Cross-correlate lip motion with speech energy
    auto corr_result = CrossCorrelateSignals(
        lip_motion, speech_energy, video.fps, 600.0);

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
