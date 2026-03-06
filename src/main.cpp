#include "common/Config.h"
#include "common/Log.h"
#include "pipeline/SyncPipeline.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace {

// Get the directory containing the running executable.
// Returns empty string on failure.
std::string GetExecutableDir() {
#ifdef __APPLE__
    char buf[1024];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        return std::filesystem::path(buf).parent_path().string();
    }
#elif defined(__linux__)
    char buf[1024];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        return std::filesystem::path(buf).parent_path().string();
    }
#elif defined(_WIN32)
    // On Windows, use GetModuleFileName if needed
#endif
    return "";
}

// Resolve a path: if it's relative and doesn't exist at cwd, try resolving
// against the executable's directory.
std::string ResolveModelPath(const std::string& path) {
    namespace fs = std::filesystem;
    fs::path p(path);

    // Absolute path: use as-is
    if (p.is_absolute()) return path;

    // Relative path: check cwd first
    if (fs::exists(p)) return path;

    // Try relative to executable directory
    std::string exe_dir = GetExecutableDir();
    if (!exe_dir.empty()) {
        fs::path resolved = fs::path(exe_dir) / p;
        if (fs::exists(resolved)) {
            return resolved.string();
        }
    }

    // Return original (will fail at load time with a clear error)
    return path;
}

void PrintUsage(const char* program) {
    std::printf("Usage: %s [options] -i <input> -o <output>\n\n", program);
    std::printf("AV Auto-Sync: Detect and correct audio-video synchronization offsets.\n\n");
    std::printf("Options:\n");
    std::printf("  -i <path>      Input video file (required)\n");
    std::printf("  -o <path>      Output video file (required)\n");
    std::printf("  -c <path>      Configuration file (JSON, optional)\n");
    std::printf("  -m <mode>      Dispatch mode: auto, force, cascade (default: auto)\n");
    std::printf("  -d <name>      Force detector name (used with -m force)\n");
    std::printf("  -t <ms>        Offset threshold in ms (default: 40)\n");
    std::printf("  -w <sec>       Segment window size in seconds (default: 5.0)\n");
    std::printf("  -s <sec>       Segment step size in seconds (default: 2.5)\n");
    std::printf("  -O <ms>        Manual offset in ms (skip auto-detection, directly apply this offset)\n");
    std::printf("                 Same convention as detection: positive = audio ahead of video\n");
    std::printf("                 e.g. -O 200 means audio leads by 200ms, will shift video earlier to fix\n");
    std::printf("  -G             Disable global constant offset (allow per-segment offsets)\n");
    std::printf("  -v             Verbose output (debug level)\n");
    std::printf("  -q             Quiet output (error level only)\n");
    std::printf("  -h             Show this help message\n");
    std::printf("\nExamples:\n");
    std::printf("  %s -i input.mp4 -o output.mp4\n", program);
    std::printf("  %s -i input.mp4 -o output.mp4 -c config.json\n", program);
    std::printf("  %s -i input.mp4 -o output.mp4 -O 100\n", program);
    std::printf("  %s -i input.mp4 -o output.mp4 -m force -d onset_align -v\n", program);
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string input_path;
    std::string output_path;
    std::string config_path;

    avsync::Config config;

    // Parse command-line arguments
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            input_path = argv[++i];
        } else if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (std::strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            config.dispatch_mode = avsync::ParseDispatchMode(argv[++i]);
        } else if (std::strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            config.forced_detector = argv[++i];
        } else if (std::strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            config.offset_threshold_ms = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            config.segment_window_sec = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            config.segment_step_sec = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "-O") == 0 && i + 1 < argc) {
            config.manual_offset_ms = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "-G") == 0) {
            config.use_global_offset = false;
        } else if (std::strcmp(argv[i], "-v") == 0) {
            config.log_level = "debug";
        } else if (std::strcmp(argv[i], "-q") == 0) {
            config.log_level = "error";
        } else if (std::strcmp(argv[i], "-h") == 0) {
            PrintUsage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            PrintUsage(argv[0]);
            return 1;
        }
    }

    if (input_path.empty() || output_path.empty()) {
        std::fprintf(stderr, "Error: input (-i) and output (-o) paths are required.\n\n");
        PrintUsage(argv[0]);
        return 1;
    }

    // Load config file if provided (command-line args override file values)
    if (!config_path.empty()) {
        avsync::Config file_config = avsync::LoadConfig(config_path);
        if (config.log_level == "info") config.log_level = file_config.log_level;
        config.onset_align = file_config.onset_align;
        config.syncnet = file_config.syncnet;
        if (config.cascade_order.size() <= 2) {
            config.cascade_order = file_config.cascade_order;
        }
        if (config.confidence_threshold == 0.4) {
            config.confidence_threshold = file_config.confidence_threshold;
        }
        // Don't override manual_offset_ms from file if already set on CLI
        if (std::isnan(config.manual_offset_ms) && !std::isnan(file_config.manual_offset_ms)) {
            config.manual_offset_ms = file_config.manual_offset_ms;
        }
    }

    // Resolve model paths: try executable directory if relative path not found at cwd
    config.syncnet.face_detect_model = ResolveModelPath(config.syncnet.face_detect_model);

    // Run pipeline
    avsync::SyncPipeline pipeline;
    pipeline.Configure(config);

    bool success = pipeline.Process(input_path, output_path);

    return success ? 0 : 1;
}
