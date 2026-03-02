#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "detector/OnsetAlignDetector.h"
#include "common/Config.h"

#include <cmath>
#include <vector>

using namespace avsync;
using Catch::Matchers::WithinAbs;

// Helper: create a default OnsetAlignConfig for testing
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

// ============================================================
// Direct tests of CrossCorrelateEvents (core algorithm)
// ============================================================

TEST_CASE("CrossCorrelate - perfectly synchronized event sequences", "[detector][xcorr]") {
    auto config = DefaultTestConfig();
    OnsetAlignDetector detector(config);

    // Audio and video events at the same times
    std::vector<double> events = {1.0, 2.0, 3.0, 4.0, 5.0};

    auto result = detector.CrossCorrelateEvents(events, events);

    CHECK_THAT(result.offset_ms, WithinAbs(0.0, 10.0));
    CHECK(result.matched_pairs == 5);
    CHECK(result.confidence > 0.3);
}

TEST_CASE("CrossCorrelate - 200ms positive offset", "[detector][xcorr]") {
    auto config = DefaultTestConfig();
    OnsetAlignDetector detector(config);

    std::vector<double> audio_events = {1.0, 2.0, 3.0, 4.0, 5.0};
    std::vector<double> video_events;
    for (double t : audio_events) {
        video_events.push_back(t + 0.2);  // video 200ms later
    }

    auto result = detector.CrossCorrelateEvents(audio_events, video_events);

    // Offset should be ~200ms (audio needs to shift +200ms to align with video)
    CHECK_THAT(result.offset_ms, WithinAbs(200.0, 20.0));
    CHECK(result.matched_pairs >= 4);
    CHECK(result.confidence > 0.3);
}

TEST_CASE("CrossCorrelate - 150ms negative offset", "[detector][xcorr]") {
    auto config = DefaultTestConfig();
    OnsetAlignDetector detector(config);

    std::vector<double> audio_events = {1.0, 2.0, 3.0, 4.0, 5.0};
    std::vector<double> video_events;
    for (double t : audio_events) {
        video_events.push_back(t - 0.15);  // video 150ms earlier
    }

    auto result = detector.CrossCorrelateEvents(audio_events, video_events);

    // Offset should be ~-150ms
    CHECK_THAT(result.offset_ms, WithinAbs(-150.0, 20.0));
    CHECK(result.matched_pairs >= 4);
    CHECK(result.confidence > 0.3);
}

TEST_CASE("CrossCorrelate - large offset 500ms", "[detector][xcorr]") {
    auto config = DefaultTestConfig();
    OnsetAlignDetector detector(config);

    std::vector<double> audio_events = {1.0, 2.0, 3.0, 4.0, 5.0};
    std::vector<double> video_events;
    for (double t : audio_events) {
        video_events.push_back(t + 0.5);
    }

    auto result = detector.CrossCorrelateEvents(audio_events, video_events);

    CHECK_THAT(result.offset_ms, WithinAbs(500.0, 20.0));
    CHECK(result.matched_pairs >= 4);
}

TEST_CASE("CrossCorrelate - empty audio events", "[detector][xcorr]") {
    auto config = DefaultTestConfig();
    OnsetAlignDetector detector(config);

    std::vector<double> audio_events;
    std::vector<double> video_events = {1.0, 2.0, 3.0};

    auto result = detector.CrossCorrelateEvents(audio_events, video_events);

    CHECK(result.matched_pairs == 0);
    CHECK(result.confidence == 0.0);
}

TEST_CASE("CrossCorrelate - empty video events", "[detector][xcorr]") {
    auto config = DefaultTestConfig();
    OnsetAlignDetector detector(config);

    std::vector<double> audio_events = {1.0, 2.0, 3.0};
    std::vector<double> video_events;

    auto result = detector.CrossCorrelateEvents(audio_events, video_events);

    CHECK(result.matched_pairs == 0);
    CHECK(result.confidence == 0.0);
}

TEST_CASE("CrossCorrelate - single event pair", "[detector][xcorr]") {
    auto config = DefaultTestConfig();
    OnsetAlignDetector detector(config);

    std::vector<double> audio_events = {3.0};
    std::vector<double> video_events = {3.1};  // 100ms offset

    auto result = detector.CrossCorrelateEvents(audio_events, video_events);

    CHECK_THAT(result.offset_ms, WithinAbs(100.0, 20.0));
    // Single event should have low confidence (penalized)
    CHECK(result.confidence < 0.5);
}

TEST_CASE("CrossCorrelate - mismatched event counts", "[detector][xcorr]") {
    auto config = DefaultTestConfig();
    OnsetAlignDetector detector(config);

    // More audio events than video, 100ms offset
    // Use events where the video subset clearly matches part of audio
    std::vector<double> audio_events = {1.0, 2.0, 3.0, 4.0, 5.0};
    std::vector<double> video_events = {1.1, 2.1, 3.1};  // 3 events at +100ms

    auto result = detector.CrossCorrelateEvents(audio_events, video_events);

    // Should still detect ~100ms offset
    CHECK_THAT(result.offset_ms, WithinAbs(100.0, 30.0));
    CHECK(result.matched_pairs >= 2);
}

// ============================================================
// Integration tests via Detect() with synthetic data
// ============================================================

static AudioSegment MakeSyntheticAudio(
    double start_time, double duration, int sample_rate,
    const std::vector<double>& onset_times_sec
) {
    AudioSegment audio;
    audio.start_time = start_time;
    audio.end_time = start_time + duration;
    audio.sample_rate = sample_rate;
    audio.channels = 1;
    int total_samples = static_cast<int>(duration * sample_rate);
    audio.samples.resize(total_samples, 0.001f);

    for (double t : onset_times_sec) {
        int center = static_cast<int>((t - start_time) * sample_rate);
        for (int i = -1024; i < 1024; ++i) {
            int idx = center + i;
            if (idx >= 0 && idx < total_samples) {
                double phase = 2.0 * 3.14159265 * 440.0 * i / sample_rate;
                audio.samples[idx] = static_cast<float>(0.9 * std::sin(phase));
            }
        }
    }
    return audio;
}

TEST_CASE("Detect - empty video returns skipped or low confidence", "[detector][detect]") {
    auto config = DefaultTestConfig();
    OnsetAlignDetector detector(config);

    auto audio = MakeSyntheticAudio(0.0, 5.0, 16000, {1.0, 2.0, 3.0});

    VideoSegment video;
    video.start_time = 0.0;
    video.end_time = 5.0;
    video.fps = 30.0;
    video.width = 32;
    video.height = 32;
    video.frames.clear();  // No frames

    auto results = detector.Detect(audio, video);
    REQUIRE(results.size() == 1);
    // With no video frames, detection is unreliable
    CHECK((results[0].skipped || results[0].confidence < 0.3));
}

TEST_CASE("Detect - all silence audio with empty video", "[detector][detect]") {
    auto config = DefaultTestConfig();
    OnsetAlignDetector detector(config);

    AudioSegment audio;
    audio.start_time = 0.0;
    audio.end_time = 5.0;
    audio.sample_rate = 16000;
    audio.channels = 1;
    audio.samples.resize(80000, 0.0f);

    VideoSegment video;
    video.start_time = 0.0;
    video.end_time = 5.0;
    video.fps = 30.0;
    video.width = 16;
    video.height = 16;
    video.frames.clear();

    auto results = detector.Detect(audio, video);
    REQUIRE(results.size() == 1);
    CHECK(results[0].skipped);
}
