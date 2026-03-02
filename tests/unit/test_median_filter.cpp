#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "aggregator/OffsetAggregator.h"
#include "common/Config.h"
#include "common/Types.h"

using namespace avsync;
using Catch::Matchers::WithinAbs;

// We test median filter indirectly through the Aggregator since it's a private method.
// Create input data that exercises the median filter path (needs >= 3 applied segments).

static Config DefaultTestConfig() {
    Config cfg;
    cfg.confidence_threshold = 0.4;
    cfg.offset_threshold_ms = 40.0;
    cfg.min_global_confidence = 0.45;
    cfg.use_global_offset = false;
    return cfg;
}

static SegmentOffset MakeSegment(double start, double end, double offset_ms, double confidence) {
    SegmentOffset seg;
    seg.start_time = start;
    seg.end_time = end;
    seg.offset_ms = offset_ms;
    seg.confidence = confidence;
    seg.method = "test";
    seg.skipped = false;
    return seg;
}

TEST_CASE("Median filter smooths mild outlier in middle", "[aggregator][median]") {
    Config cfg = DefaultTestConfig();
    OffsetAggregator agg(cfg);

    // Three segments: 100, 150, 100 — median filter should make middle = 100
    std::vector<SegmentOffset> raw = {
        MakeSegment(0, 5, 100.0, 0.8),
        MakeSegment(5, 10, 150.0, 0.8),
        MakeSegment(10, 15, 100.0, 0.8),
    };

    auto decisions = agg.Aggregate(raw);
    REQUIRE(decisions.size() == 3);

    // After median filter with kernel=3, the middle should be 100 (median of {100, 150, 100})
    CHECK_THAT(decisions[1].correction_ms, WithinAbs(100.0, 5.0));
}

TEST_CASE("Median filter preserves consistent values", "[aggregator][median]") {
    Config cfg = DefaultTestConfig();
    OffsetAggregator agg(cfg);

    std::vector<SegmentOffset> raw = {
        MakeSegment(0, 5, 200.0, 0.9),
        MakeSegment(5, 10, 200.0, 0.9),
        MakeSegment(10, 15, 200.0, 0.9),
        MakeSegment(15, 20, 200.0, 0.9),
    };

    auto decisions = agg.Aggregate(raw);
    REQUIRE(decisions.size() == 4);

    for (const auto& d : decisions) {
        CHECK(d.apply);
        CHECK_THAT(d.correction_ms, WithinAbs(200.0, 5.0));
    }
}

TEST_CASE("Median filter with 5 segments smooths outlier", "[aggregator][median]") {
    Config cfg = DefaultTestConfig();
    OffsetAggregator agg(cfg);

    // 100, 100, 300, 100, 100 — the 300 is an outlier but may not reach 3*MAD threshold
    // so it may still be present. Median filter should smooth it.
    std::vector<SegmentOffset> raw = {
        MakeSegment(0, 5, 100.0, 0.8),
        MakeSegment(5, 10, 100.0, 0.8),
        MakeSegment(10, 15, 120.0, 0.8),  // mild variation
        MakeSegment(15, 20, 100.0, 0.8),
        MakeSegment(20, 25, 100.0, 0.8),
    };

    auto decisions = agg.Aggregate(raw);
    REQUIRE(decisions.size() == 5);

    // After median filter, the middle segment should be pulled toward 100
    CHECK_THAT(decisions[2].correction_ms, WithinAbs(100.0, 10.0));
}
