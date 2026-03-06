
# AV Auto-Sync

Detect and correct audio-video synchronization offsets in media files — automatically or manually.

AV Auto-Sync provides both a **CLI pipeline** for batch processing and an **interactive GUI** with real-time preview, built on top of FFmpeg.

---

## Features

- **Automatic offset detection** via audio/video onset alignment (spectral-flux + frame-diff cross-correlation)
- **Optional SyncNet detector** (lip-sync neural network, requires OpenCV)
- **GUI player** with SDL2 + Dear ImGui
  - Real-time A/V preview with adjustable offset (millisecond precision)
  - Variable playback speed (0.1× – 2.0×)
  - Seek, pause, frame-step controls
  - Save corrected video with applied offset
  - Bilingual UI (English / 中文), auto-detected or selectable
- **CLI tool** for headless / scripted workflows
- Cross-platform: **macOS**, **Linux**, **Windows**

---

## Quick Start

### Prerequisites

| Dependency | Required | Notes |
|---|---|---|
| CMake ≥ 3.16 | ✅ | Build system |
| C++17 compiler | ✅ | GCC 9+, Clang 10+, MSVC 2019+ |
| FFmpeg 5+ dev libs | ✅ | `libavformat libavcodec libavutil libswresample libswscale libavfilter` |
| pkg-config | ✅ | For finding FFmpeg |
| SDL2 dev | GUI only | Required for `avsync_gui` |
| OpenCV | Optional | For SyncNet detector |

### Install Dependencies

<details>
<summary><strong>macOS (Homebrew)</strong></summary>

```bash
brew install cmake ffmpeg sdl2 pkg-config
```
</details>

<details>
<summary><strong>Ubuntu / Debian</strong></summary>

```bash
sudo apt update
sudo apt install cmake pkg-config \
    libavformat-dev libavcodec-dev libavutil-dev \
    libswresample-dev libswscale-dev libavfilter-dev \
    libsdl2-dev
```
</details>

<details>
<summary><strong>Windows (vcpkg)</strong></summary>

```powershell
vcpkg install ffmpeg[core,avformat,avcodec,swresample,swscale,avfilter]:x64-windows sdl2:x64-windows
```

When configuring CMake, pass `-DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake`.
</details>

### Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Binaries are placed in `build/bin/`.

### Build Options

| Option | Default | Description |
|---|---|---|
| `AVSYNC_ENABLE_GUI` | `ON` | Build the GUI application (requires SDL2) |
| `AVSYNC_ENABLE_TESTS` | `ON` | Build unit tests |
| `AVSYNC_ENABLE_SYNCNET` | `OFF` | Enable SyncNet detector (requires OpenCV) |

Example — CLI only, no GUI:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DAVSYNC_ENABLE_GUI=OFF
```

---

## Usage

### CLI

```bash
# Auto-detect offset and fix
./build/bin/avsync -i input.mp4 -o output.mp4

# Apply a known manual offset (in milliseconds, positive = audio leads)
./build/bin/avsync -i input.mp4 -o output.mp4 -O 200

# Use custom config and verbose logging
./build/bin/avsync -i input.mp4 -o output.mp4 -c config/default.json -v
```

**All CLI options:**

```
  -i <path>      Input video file (required)
  -o <path>      Output video file (required)
  -c <path>      Configuration file (JSON, optional)
  -m <mode>      Dispatch mode: auto, force, cascade (default: auto)
  -d <name>      Force detector name (used with -m force)
  -t <ms>        Offset threshold in ms (default: 40)
  -w <sec>       Segment window size in seconds (default: 5.0)
  -s <sec>       Segment step size in seconds (default: 2.5)
  -O <ms>        Manual offset in ms (skip auto-detection)
  -G             Disable global constant offset
  -v             Verbose output (debug level)
  -q             Quiet output (error level only)
```

### GUI

```bash
# Launch with default settings
./build/bin/avsync_gui

# Open a file directly
./build/bin/avsync_gui video.mp4

# Set UI language and window size
./build/bin/avsync_gui -L zh -W 1600 -H 900
```

**GUI options:**

```
  -W <width>     Window width  (default: 1280)
  -H <height>    Window height (default: 800)
  -L <lang>      UI language: en, zh (default: auto-detect)
```

You can also drag-and-drop files onto the GUI window.

---

## Configuration

A JSON config file can be passed to the CLI with `-c`. See [`config/default.json`](config/default.json) for all available options:

```json
{
    "dispatch_mode": "auto",
    "onset_align": {
        "spectral_flux_threshold": 2.0,
        "frame_diff_threshold": 30.0,
        "search_range_ms": 1200.0,
        "resolution_ms": 5.0
    },
    "log_level": "info"
}
```

---

## Project Structure

```
av-auto-sync/
├── src/
│   ├── main.cpp                 # CLI entry point
│   ├── common/                  # Config, Types, Logging
│   ├── decoder/                 # FFmpeg media decoder
│   ├── detector/                # Sync detectors (OnsetAlign, SyncNet)
│   ├── aggregator/              # Offset aggregation & filtering
│   ├── corrector/               # Timestamp correction & remuxing
│   ├── pipeline/                # End-to-end pipeline orchestration
│   └── gui/                     # Dear ImGui + SDL2 GUI application
├── config/                      # Default configuration files
├── models/                      # Neural network models (SyncNet)
├── tests/
│   ├── unit/                    # Catch2 unit tests
│   └── scripts/                 # Integration test scripts
└── CMakeLists.txt
```

---

## Running Tests

```bash
cmake -B build -DAVSYNC_ENABLE_TESTS=ON
cmake --build build
cd build && ctest --output-on-failure
```

---

## License

See the project license file for details.
