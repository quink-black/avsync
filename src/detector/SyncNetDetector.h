#pragma once

#include "common/Config.h"
#include "common/Types.h"
#include "detector/ISyncDetector.h"

#include <string>
#include <vector>

#ifdef AVSYNC_ENABLE_SYNCNET

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>

namespace avsync {

// SyncNet-based detector: uses face detection + lip region motion analysis
// to detect audio-visual synchronization offsets.
//
// Implementation approach:
// 1. Detect faces in video frames using OpenCV's YuNet DNN model
// 2. Extract lip/lower-face region
// 3. Compute lip motion signal (frame-to-frame differences in lip region)
// 4. Extract speech energy envelope from audio
// 5. Cross-correlate lip motion and speech energy to find AV offset
class SyncNetDetector : public ISyncDetector {
public:
    explicit SyncNetDetector(const SyncNetConfig& config);
    ~SyncNetDetector() override = default;

    std::vector<SegmentOffset> Detect(
        const AudioSegment& audio,
        const VideoSegment& video
    ) override;

    bool CanHandle(const ContentFeatures& features) const override;
    std::string Name() const override { return "syncnet"; }

    // Exposed for testing

    // Detect faces in a single RGB frame, returns bounding boxes
    struct FaceRect {
        int x, y, width, height;
    };
    std::vector<FaceRect> DetectFaces(const std::vector<uint8_t>& frame, int img_width, int img_height);

    // Extract lip region from a face bounding box
    struct LipRegion {
        int x, y, width, height;
    };
    LipRegion ExtractLipRegion(const FaceRect& face) const;

    // Compute lip motion signal: per-frame lip region difference
    std::vector<double> ComputeLipMotion(
        const VideoSegment& video,
        const std::vector<FaceRect>& faces
    ) const;

    // Compute speech energy envelope from audio
    std::vector<double> ComputeSpeechEnergy(
        const AudioSegment& audio,
        double frame_rate
    ) const;

    // Cross-correlate two signals to find optimal offset
    struct CorrelationResult {
        double offset_ms;
        double confidence;
    };
    CorrelationResult CrossCorrelateSignals(
        const std::vector<double>& signal_a,
        const std::vector<double>& signal_b,
        double frame_rate,
        double search_range_ms
    ) const;

private:
    SyncNetConfig config_;
    cv::Ptr<cv::FaceDetectorYN> face_detector_;
    bool face_detector_loaded_ = false;
};

}  // namespace avsync

#endif  // AVSYNC_ENABLE_SYNCNET
