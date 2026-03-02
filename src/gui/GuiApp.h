#pragma once

#include "PlayerCore.h"
#include "I18n.h"
#include "common/Config.h"

#include <string>

struct SDL_Renderer;
struct SDL_Window;

namespace avsync {
namespace gui {

// The main GUI application window built with SDL2 + Dear ImGui.
// Provides:
//   - Video preview with AV sync offset adjustment
//   - Auto-detection of AV offset via SyncPipeline
//   - Manual fine-tuning with slider and ±10/±100ms buttons
//   - Variable-speed playback (0.1x~4.0x) for lip-sync verification
//   - File open dialog and drag-and-drop
//   - Save corrected video output
class GuiApp {
public:
    GuiApp();
    ~GuiApp();

    // Set the UI language explicitly (must be called before Init).
    // If not called, language is auto-detected from system locale.
    void SetLang(Lang lang);

    // Initialize SDL, ImGui, and window. Returns false on failure.
    bool Init(int width = 1280, int height = 800);

    // Main event loop. Returns when user closes the window.
    void Run();

    // Clean up resources.
    void Shutdown();

private:
    // Render one frame of the GUI
    void RenderFrame();

    // Draw the control panel (ImGui) — uses logical pixel coordinates
    void DrawControlPanel();

    // Draw the video area — uses logical pixel coordinates
    void DrawVideoArea();

    // Handle SDL events
    bool HandleEvents();

    // Handle file drop
    void OnFileDrop(const char* path);

    // Open a file from path
    void OpenFile(const std::string& path);

    // Run auto-detection in background
    void RunAutoDetect();

    // Save corrected video
    void SaveCorrectedVideo();

    // Open a native file browser dialog for opening a video file
    void ShowOpenFileDialog();

    // Open a native file browser dialog for choosing output save path
    void ShowSaveFileDialog();

    // Format time (seconds) to MM:SS.ms string
    static std::string FormatTime(double seconds);

    // Window
    SDL_Window*   window_   = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    int win_width_  = 1280;
    int win_height_ = 800;
    int ctrl_panel_height_ = 320;  // Height of control panel at bottom (logical pixels)
    float dpi_scale_ = 1.0f;       // Framebuffer pixels / logical pixels

    // Player
    PlayerCore player_;

    // Video texture
    SDL_Texture* video_texture_ = nullptr;
    int tex_width_  = 0;
    int tex_height_ = 0;

    // State
    bool initialized_ = false;
    bool running_ = false;
    bool show_advanced_ = false;

    // Internationalization
    Lang lang_ = Lang::EN;
    const Strings* str_ = nullptr;
    bool need_font_rebuild_ = false;

    // Offset controls
    double manual_offset_ms_ = 0.0;       // Applied offset (in player)
    double offset_input_ms_ = 0.0;        // Editing buffer for InputDouble
    double detected_offset_ms_ = 0.0;
    bool   has_detection_ = false;
    bool   detecting_ = false;
    std::string detect_status_;

    // Offset feedback: flash "Applied!" briefly after Enter
    bool   offset_just_applied_ = false;
    Uint32 offset_applied_tick_ = 0;
    static constexpr Uint32 kAppliedFlashMs = 1200;  // show for 1.2s

    // Playback speed
    float play_speed_ = 1.0f;
    static constexpr float kSpeedPresets[] = {
        0.1f, 0.2f, 0.25f, 0.5f, 0.75f,
        1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 4.0f
    };
    static constexpr int kNumSpeedPresets = 11;

    // Advanced config
    Config config_;

    // Save state
    bool saving_ = false;
    std::string save_status_;
    std::string save_output_path_;   // User-chosen output path
    char output_path_buf_[1024] = {}; // Editable text buffer for output path
};

}  // namespace gui
}  // namespace avsync
