#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "detector/OnsetAlignDetector.h"
#include "common/Config.h"
#include "common/Types.h"

#include <cmath>
#include <vector>

using namespace avsync;

static OnsetAlignConfig DefaultTestConfig() {
    OnsetAlignConfig cfg;
    cfg.spectral_flux_threshold = 2.0;
    cfg.frame_diff_threshold = 30.0;
    cfg.min_onset_count = 3;
    cfg.search_range_ms = 600.0;
    cfg.resolution_ms = 5.0;
    cfg.match_tolerance_ms = 50.0;
    cfg.min_match_ratio = 0.15;
    return cfg;
}

// Helper: create sine burst audio at specific times
static AudioSegment MakeBurstAudio(
    double start, double duration, int sr,
    const std::vector<double>& onset_times
) {
    AudioSegment audio;
    audio.start_time = start;
    audio.end_time = start + duration;
    audio.sample_rate = sr;
    audio.channels = 1;
    int total = static_cast<int>(duration * sr);
    audio.samples.resize(total, 0.001f);
    for (double t : onset_times) {
        int center = static_cast<int>((t - start) * sr);
        for (int i = -1024; i < 1024; ++i) {
            int idx = center + i;
            if (idx >= 0 && idx < total) {
                double phase = 2.0 * 3.14159265 * 440.0 * i / sr;
                audio.samples[idx] = static_cast<float>(0.9 * std::sin(phase));
            }
        }
    }
    return audio;
}

TEST_CASE("Audio onset detection - silence produces no onsets", "[detector][onset]") {
    auto config = DefaultTestConfig();
    OnsetAlignDetector detector(config);

    AudioSegment audio;
    audio.start_time = 0.0;
    audio.end_time = 5.0;
    audio.sample_rate = 16000;
    audio.channels = 1;
    audio.samples.resize(80000, 0.0f);  // all silence

    VideoSegment video;
    video.start_time = 0.0;
    video.end_time = 5.0;
    video.fps = 30.0;
    video.width = 16;
    video.height = 16;

    auto results = detector.Detect(audio, video);
    REQUIRE(results.size() == 1);
    // Silent audio should result in skipped or very low confidence
    CHECK((results[0].skipped || results[0].confidence < 0.3));
}

TEST_CASE("Audio onset detection - periodic clicks produce onsets", "[detector][onset]") {
    auto config = DefaultTestConfig();
    OnsetAlignDetector detector(config);

    auto audio = MakeBurstAudio(0.0, 5.0, 16000, {1.0, 2.0, 3.0, 4.0});

    // Also create matching video events
    VideoSegment video;
    video.start_time = 0.0;
    video.end_time = 5.0;
    video.fps = 30.0;
    video.width = 16;
    video.height = 16;
    size_t frame_size = 16 * 16 * 3;
    int num_frames = 150;
    for (int i = 0; i < num_frames; ++i) {
        video.frames.push_back(MakeFrame(std::vector<uint8_t>(frame_size, 128)));
    }
    // Visual events at same times
    for (double t : {1.0, 2.0, 3.0, 4.0}) {
        int f = static_cast<int>(t * 30.0);
        if (f >= 0 && f < num_frames) {
            std::fill(video.frames[f]->begin(), video.frames[f]->end(), 255);
        }
    }

    auto results = detector.Detect(audio, video);
    REQUIRE(results.size() == 1);
    // Should not be skipped since we have clear events
    CHECK_FALSE(results[0].skipped);
}

TEST_CASE("Video frame diff - static video produces no events", "[detector][video]") {
    auto config = DefaultTestConfig();
    OnsetAlignDetector detector(config);

    AudioSegment audio;
    audio.start_time = 0.0;
    audio.end_time = 3.0;
    audio.sample_rate = 16000;
    audio.channels = 1;
    audio.samples.resize(48000, 0.0f);

    VideoSegment video;
    video.start_time = 0.0;
    video.end_time = 3.0;
    video.fps = 30.0;
    video.width = 16;
    video.height = 16;
    size_t frame_size = 16 * 16 * 3;
    // All frames identical (gray)
    for (int i = 0; i < 90; ++i) {
        video.frames.push_back(MakeFrame(std::vector<uint8_t>(frame_size, 128)));
    }

    auto results = detector.Detect(audio, video);
    REQUIRE(results.size() == 1);
    // No events to correlate
    CHECK((results[0].skipped || results[0].confidence < 0.3));
}

TEST_CASE("Multi-channel audio is properly downmixed", "[detector][audio]") {
    auto config = DefaultTestConfig();
    OnsetAlignDetector detector(config);

    // Stereo audio with events — use sine bursts
    AudioSegment audio;
    audio.start_time = 0.0;
    audio.end_time = 5.0;
    audio.sample_rate = 16000;
    audio.channels = 2;
    int total_mono = 80000;
    audio.samples.resize(total_mono * 2, 0.001f);

    // Insert stereo sine bursts at known times
    for (double t : {1.0, 2.0, 3.0, 4.0}) {
        int center = static_cast<int>(t * 16000);
        for (int i = -1024; i < 1024; ++i) {
            int idx = center + i;
            if (idx >= 0 && idx < total_mono) {
                double phase = 2.0 * 3.14159265 * 440.0 * i / 16000;
                float val = static_cast<float>(0.9 * std::sin(phase));
                audio.samples[idx * 2] = val;       // left
                audio.samples[idx * 2 + 1] = val;   // right
            }
        }
    }

    VideoSegment video;
    video.start_time = 0.0;
    video.end_time = 5.0;
    video.fps = 30.0;
    video.width = 16;
    video.height = 16;
    size_t frame_size = 16 * 16 * 3;
    for (int i = 0; i < 150; ++i) {
        video.frames.push_back(MakeFrame(std::vector<uint8_t>(frame_size, 128)));
    }
    for (double t : {1.0, 2.0, 3.0, 4.0}) {
        int f = static_cast<int>(t * 30.0);
        if (f >= 0 && f < 150) {
            std::fill(video.frames[f]->begin(), video.frames[f]->end(), 255);
        }
    }

    auto results = detector.Detect(audio, video);
    REQUIRE(results.size() == 1);
    // Should handle multi-channel without crashing
    CHECK_FALSE(results[0].skipped);
}
