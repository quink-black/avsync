#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "aggregator/OffsetAggregator.h"
#include "common/Config.h"
#include "common/Types.h"

using namespace avsync;
using Catch::Matchers::WithinAbs;

static Config DefaultTestConfig() {
    Config cfg;
    cfg.confidence_threshold = 0.4;
    cfg.offset_threshold_ms = 40.0;
    cfg.min_global_confidence = 0.45;
    cfg.use_global_offset = false;  // test per-segment behavior first
    return cfg;
}

static SegmentOffset MakeSegment(double start, double end, double offset_ms,
                                  double confidence, bool skipped = false,
                                  const std::string& reason = "") {
    SegmentOffset seg;
    seg.start_time = start;
    seg.end_time = end;
    seg.offset_ms = offset_ms;
    seg.confidence = confidence;
    seg.method = "test";
    seg.skipped = skipped;
    seg.skip_reason = reason;
    return seg;
}

TEST_CASE("Aggregator - all segments agree on offset", "[aggregator]") {
    Config cfg = DefaultTestConfig();
    OffsetAggregator agg(cfg);

    std::vector<SegmentOffset> raw = {
        MakeSegment(0, 5, 100.0, 0.8),
        MakeSegment(5, 10, 105.0, 0.75),
        MakeSegment(10, 15, 98.0, 0.85),
        MakeSegment(15, 20, 102.0, 0.7),
    };

    auto decisions = agg.Aggregate(raw);
    REQUIRE(decisions.size() == 4);

    for (const auto& d : decisions) {
        CHECK(d.apply);
        // All offsets should be around 100ms (possibly median-filtered)
        CHECK_THAT(d.correction_ms, WithinAbs(100.0, 20.0));
    }
}

TEST_CASE("Aggregator - outlier segment replaced by consensus", "[aggregator]") {
    Config cfg = DefaultTestConfig();
    OffsetAggregator agg(cfg);

    std::vector<SegmentOffset> raw = {
        MakeSegment(0, 5, 100.0, 0.8),
        MakeSegment(5, 10, 100.0, 0.8),
        MakeSegment(10, 15, 500.0, 0.8),  // outlier
        MakeSegment(15, 20, 100.0, 0.8),
    };

    auto decisions = agg.Aggregate(raw);
    REQUIRE(decisions.size() == 4);

    // The outlier segment (index 2) should be replaced by consensus (~100ms)
    CHECK_THAT(decisions[2].correction_ms, WithinAbs(100.0, 20.0));
}

TEST_CASE("Aggregator - low confidence segments", "[aggregator]") {
    Config cfg = DefaultTestConfig();
    OffsetAggregator agg(cfg);

    std::vector<SegmentOffset> raw = {
        MakeSegment(0, 5, 100.0, 0.8),
        MakeSegment(5, 10, 105.0, 0.1),  // low confidence
        MakeSegment(10, 15, 100.0, 0.8),
    };

    auto decisions = agg.Aggregate(raw);
    REQUIRE(decisions.size() == 3);

    // Low confidence segment should either use consensus or be skipped
    // Since it agrees with consensus (offset ~105 is close to median ~100),
    // it should use consensus median
    CHECK(decisions[1].apply);
}

TEST_CASE("Aggregator - offset within threshold is skipped", "[aggregator]") {
    Config cfg = DefaultTestConfig();
    OffsetAggregator agg(cfg);

    std::vector<SegmentOffset> raw = {
        MakeSegment(0, 5, 20.0, 0.8),   // within ±40ms threshold
        MakeSegment(5, 10, 15.0, 0.75),
        MakeSegment(10, 15, 25.0, 0.85),
    };

    auto decisions = agg.Aggregate(raw);
    REQUIRE(decisions.size() == 3);

    // All offsets are within ±40ms threshold, so none should be applied
    for (const auto& d : decisions) {
        CHECK_FALSE(d.apply);
    }
}

TEST_CASE("Aggregator - skipped segments filled by neighbors", "[aggregator]") {
    Config cfg = DefaultTestConfig();
    OffsetAggregator agg(cfg);

    std::vector<SegmentOffset> raw = {
        MakeSegment(0, 5, 100.0, 0.8),
        MakeSegment(5, 10, 0.0, 0.0, true, "silence"),  // skipped
        MakeSegment(10, 15, 100.0, 0.8),
    };

    auto decisions = agg.Aggregate(raw);
    REQUIRE(decisions.size() == 3);

    // Skipped segment should be filled by interpolation from neighbors
    CHECK(decisions[1].apply);
    CHECK_THAT(decisions[1].correction_ms, WithinAbs(100.0, 20.0));
}

TEST_CASE("Aggregator - empty input returns empty", "[aggregator]") {
    Config cfg = DefaultTestConfig();
    OffsetAggregator agg(cfg);

    std::vector<SegmentOffset> raw;
    auto decisions = agg.Aggregate(raw);
    CHECK(decisions.empty());
}

TEST_CASE("Aggregator - single segment", "[aggregator]") {
    Config cfg = DefaultTestConfig();
    OffsetAggregator agg(cfg);

    std::vector<SegmentOffset> raw = {
        MakeSegment(0, 10, 200.0, 0.9),
    };

    auto decisions = agg.Aggregate(raw);
    REQUIRE(decisions.size() == 1);
    CHECK(decisions[0].apply);
    CHECK_THAT(decisions[0].correction_ms, WithinAbs(200.0, 10.0));
}

TEST_CASE("Aggregator - safety gate rejects all-low-confidence corrections", "[aggregator]") {
    Config cfg = DefaultTestConfig();
    cfg.min_global_confidence = 0.45;
    OffsetAggregator agg(cfg);

    // All segments have low confidence (below 0.45 average)
    std::vector<SegmentOffset> raw = {
        MakeSegment(0, 5, -500.0, 0.3),
        MakeSegment(5, 10, -480.0, 0.25),
        MakeSegment(10, 15, -520.0, 0.35),
        MakeSegment(15, 20, -490.0, 0.2),
    };

    auto decisions = agg.Aggregate(raw);
    REQUIRE(decisions.size() == 4);

    // Safety gate should reject ALL corrections
    for (const auto& d : decisions) {
        CHECK_FALSE(d.apply);
        CHECK(d.correction_ms == 0.0);
    }
}

TEST_CASE("Aggregator - safety gate allows high-confidence corrections", "[aggregator]") {
    Config cfg = DefaultTestConfig();
    cfg.min_global_confidence = 0.45;
    OffsetAggregator agg(cfg);

    // All segments have high confidence (above 0.45 average)
    std::vector<SegmentOffset> raw = {
        MakeSegment(0, 5, 200.0, 0.85),
        MakeSegment(5, 10, 195.0, 0.80),
        MakeSegment(10, 15, 205.0, 0.75),
        MakeSegment(15, 20, 200.0, 0.90),
    };

    auto decisions = agg.Aggregate(raw);
    REQUIRE(decisions.size() == 4);

    // All should be applied since avg confidence is high
    for (const auto& d : decisions) {
        CHECK(d.apply);
        CHECK_THAT(d.correction_ms, WithinAbs(200.0, 20.0));
    }
}

TEST_CASE("Aggregator - borderline confidence just below gate", "[aggregator]") {
    Config cfg = DefaultTestConfig();
    cfg.min_global_confidence = 0.45;
    OffsetAggregator agg(cfg);

    // Average confidence = (0.44 + 0.44 + 0.44) / 3 = 0.44 < 0.45
    std::vector<SegmentOffset> raw = {
        MakeSegment(0, 5, 300.0, 0.44),
        MakeSegment(5, 10, 305.0, 0.44),
        MakeSegment(10, 15, 295.0, 0.44),
    };

    auto decisions = agg.Aggregate(raw);
    REQUIRE(decisions.size() == 3);

    // Just below threshold: all should be rejected
    for (const auto& d : decisions) {
        CHECK_FALSE(d.apply);
    }
}
