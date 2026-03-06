#include "common/Config.h"
#include "common/Log.h"

#include <fstream>
#include <nlohmann/json.hpp>

namespace avsync {

using json = nlohmann::json;

DispatchMode ParseDispatchMode(const std::string& mode) {
    if (mode == "force") return DispatchMode::Force;
    if (mode == "cascade") return DispatchMode::Cascade;
    return DispatchMode::Auto;
}

Config LoadConfig(const std::string& path) {
    Config config;

    std::ifstream file(path);
    if (!file.is_open()) {
        Log::Warn("Failed to open config file: %s, using defaults", path.c_str());
        return config;
    }

    try {
        json j = json::parse(file);

        if (j.contains("dispatch_mode")) {
            config.dispatch_mode = ParseDispatchMode(j["dispatch_mode"].get<std::string>());
        }
        if (j.contains("forced_detector")) {
            config.forced_detector = j["forced_detector"].get<std::string>();
        }
        if (j.contains("cascade_order")) {
            config.cascade_order = j["cascade_order"].get<std::vector<std::string>>();
        }
        if (j.contains("confidence_threshold")) {
            config.confidence_threshold = j["confidence_threshold"].get<double>();
        }
        if (j.contains("offset_threshold_ms")) {
            config.offset_threshold_ms = j["offset_threshold_ms"].get<double>();
        }
        if (j.contains("segment_window_sec")) {
            config.segment_window_sec = j["segment_window_sec"].get<double>();
        }
        if (j.contains("segment_step_sec")) {
            config.segment_step_sec = j["segment_step_sec"].get<double>();
        }
        if (j.contains("log_level")) {
            config.log_level = j["log_level"].get<std::string>();
        }
        if (j.contains("use_global_offset")) {
            config.use_global_offset = j["use_global_offset"].get<bool>();
        }
        if (j.contains("min_global_confidence")) {
            config.min_global_confidence = j["min_global_confidence"].get<double>();
        }
        if (j.contains("manual_offset_ms")) {
            config.manual_offset_ms = j["manual_offset_ms"].get<double>();
        }

        // Onset align config
        if (j.contains("onset_align")) {
            auto& oa = j["onset_align"];
            if (oa.contains("spectral_flux_threshold")) {
                config.onset_align.spectral_flux_threshold = oa["spectral_flux_threshold"].get<double>();
            }
            if (oa.contains("frame_diff_threshold")) {
                config.onset_align.frame_diff_threshold = oa["frame_diff_threshold"].get<double>();
            }
            if (oa.contains("min_onset_count")) {
                config.onset_align.min_onset_count = oa["min_onset_count"].get<int>();
            }
            if (oa.contains("search_range_ms")) {
                config.onset_align.search_range_ms = oa["search_range_ms"].get<double>();
            }
            if (oa.contains("resolution_ms")) {
                config.onset_align.resolution_ms = oa["resolution_ms"].get<double>();
            }
            if (oa.contains("match_tolerance_ms")) {
                config.onset_align.match_tolerance_ms = oa["match_tolerance_ms"].get<double>();
            }
            if (oa.contains("min_match_ratio")) {
                config.onset_align.min_match_ratio = oa["min_match_ratio"].get<double>();
            }
        }

        // SyncNet config
        if (j.contains("syncnet")) {
            auto& sn = j["syncnet"];
            if (sn.contains("face_detect_model")) {
                config.syncnet.face_detect_model = sn["face_detect_model"].get<std::string>();
            }
            if (sn.contains("confidence_threshold")) {
                config.syncnet.confidence_threshold = sn["confidence_threshold"].get<double>();
            }
            if (sn.contains("face_min_size")) {
                config.syncnet.face_min_size = sn["face_min_size"].get<int>();
            }
            if (sn.contains("lip_region_ratio")) {
                config.syncnet.lip_region_ratio = sn["lip_region_ratio"].get<double>();
            }
        }

        Log::Info("Config loaded from: %s", path.c_str());
    } catch (const json::exception& e) {
        Log::Error("Failed to parse config file: %s, error: %s", path.c_str(), e.what());
    }

    return config;
}

}  // namespace avsync
