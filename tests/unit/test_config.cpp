#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "common/Config.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

using namespace avsync;
using Catch::Matchers::WithinAbs;

TEST_CASE("Config - default values", "[config]") {
    Config cfg;

    CHECK(std::isnan(cfg.manual_offset_ms));
    CHECK(cfg.use_global_offset == true);
    CHECK(cfg.log_level == "info");
    CHECK(cfg.offset_threshold_ms > 0.0);
    CHECK(cfg.confidence_threshold > 0.0);
}

TEST_CASE("Config - load from JSON file", "[config]") {
    // Create a temporary JSON config in system temp directory
    std::string tmp_path = (std::filesystem::temp_directory_path() / "avsync_test_config.json").string();
    {
        std::ofstream ofs(tmp_path);
        ofs << R"({
            "confidence_threshold": 0.5,
            "offset_threshold_ms": 50,
            "use_global_offset": false,
            "manual_offset_ms": 123.4,
            "log_level": "debug",
            "onset_align": {
                "spectral_flux_threshold": 3.0,
                "search_range_ms": 500.0
            }
        })";
    }

    Config cfg = LoadConfig(tmp_path);

    CHECK_THAT(cfg.confidence_threshold, WithinAbs(0.5, 0.001));
    CHECK_THAT(cfg.offset_threshold_ms, WithinAbs(50.0, 0.001));
    CHECK(cfg.use_global_offset == false);
    CHECK_THAT(cfg.manual_offset_ms, WithinAbs(123.4, 0.001));
    CHECK(cfg.log_level == "debug");
    CHECK_THAT(cfg.onset_align.spectral_flux_threshold, WithinAbs(3.0, 0.001));
    CHECK_THAT(cfg.onset_align.search_range_ms, WithinAbs(500.0, 0.001));

    // Cleanup
    std::remove(tmp_path.c_str());
}

TEST_CASE("Config - load from non-existent file returns defaults", "[config]") {
    std::string nonexistent = (std::filesystem::temp_directory_path() / "nonexistent_avsync_config_xyz.json").string();
    Config cfg = LoadConfig(nonexistent);

    // Should return default config (LoadConfig logs warning but doesn't crash)
    CHECK(std::isnan(cfg.manual_offset_ms));
    CHECK(cfg.use_global_offset == true);
}

TEST_CASE("Config - manual_offset_ms NaN means auto mode", "[config]") {
    Config cfg;
    CHECK(std::isnan(cfg.manual_offset_ms));

    cfg.manual_offset_ms = 100.0;
    CHECK_FALSE(std::isnan(cfg.manual_offset_ms));
    CHECK_THAT(cfg.manual_offset_ms, WithinAbs(100.0, 0.001));
}
