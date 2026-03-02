#include "GuiApp.h"

#include "common/Log.h"
#include "pipeline/SyncPipeline.h"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

#include <nfd.h>

namespace avsync {
namespace gui {

constexpr float GuiApp::kSpeedPresets[];

GuiApp::GuiApp() {
    lang_ = DetectSystemLang();
    str_ = &GetStrings(lang_);
}

void GuiApp::SetLang(Lang lang) {
    lang_ = lang;
    str_ = &GetStrings(lang_);
}

GuiApp::~GuiApp() { Shutdown(); }

bool GuiApp::Init(int width, int height) {
    win_width_ = width;
    win_height_ = height;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
        return false;
    }

    window_ = SDL_CreateWindow(
        str_->window_title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_width_, win_height_,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window_) {
        std::fprintf(stderr, "SDL_CreateWindow Error: %s\n", SDL_GetError());
        return false;
    }

    renderer_ = SDL_CreateRenderer(window_, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) {
        std::fprintf(stderr, "SDL_CreateRenderer Error: %s\n", SDL_GetError());
        return false;
    }

    // Initialize Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // --- HiDPI / Retina support ---
    // ImGui_ImplSDL2 sets io.DisplaySize = logical window size (e.g. 1280x800)
    // and io.DisplayFramebufferScale = fb/window ratio (e.g. 2.0 on Retina).
    // So ImGui coordinates are always in LOGICAL pixels.
    // We only need dpi_scale_ for SDL_RenderCopy (which uses fb pixels).
    int fb_w = 0, fb_h = 0;
    SDL_GetRendererOutputSize(renderer_, &fb_w, &fb_h);
    dpi_scale_ = (win_width_ > 0) ? static_cast<float>(fb_w) / win_width_ : 1.0f;
    if (dpi_scale_ < 1.0f) dpi_scale_ = 1.0f;

    // Build a smooth, readable font.
    // Try to load a system TTF font for smooth rendering.
    // Fall back to default bitmap font with high oversampling if TTF not found.
    float font_size = 16.0f;
    ImFont* loaded_font = nullptr;

    // Font loading strategy:
    // 1. Load a base Latin font (system font or fallback bitmap)
    // 2. If Chinese locale, merge a CJK font on top for Chinese glyphs

    // System font paths for Latin text
    static const char* latin_font_paths[] = {
        "/System/Library/Fonts/SFNS.ttf",                    // macOS San Francisco
        "/System/Library/Fonts/SFNSText.ttf",                // macOS San Francisco Text
        "/System/Library/Fonts/Helvetica.ttc",               // macOS Helvetica
        "/System/Library/Fonts/Supplemental/Arial.ttf",      // macOS Arial
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",   // Linux DejaVu
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", // Linux Liberation
        "C:\\Windows\\Fonts\\segoeui.ttf",                    // Windows Segoe UI
        "C:\\Windows\\Fonts\\arial.ttf",                      // Windows Arial
        nullptr
    };

    // CJK font paths (for Chinese glyph support)
    static const char* cjk_font_paths[] = {
        "/System/Library/Fonts/STHeiti Light.ttc",           // macOS STHeiti
        "/System/Library/Fonts/PingFang.ttc",                // macOS PingFang
        "/Library/Fonts/Arial Unicode.ttf",                  // macOS Arial Unicode
        "/System/Library/Fonts/Hiragino Sans GB.ttc",        // macOS Hiragino
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", // Linux Noto CJK
        "/usr/share/fonts/truetype/droid/DroidSansFallback.ttf",  // Linux Droid
        "C:\\Windows\\Fonts\\msyh.ttc",                      // Windows Microsoft YaHei
        "C:\\Windows\\Fonts\\simsun.ttc",                     // Windows SimSun
        nullptr
    };

    ImFontConfig font_cfg;
    font_cfg.OversampleH = 3;
    font_cfg.OversampleV = 2;
    font_cfg.PixelSnapH = false;

    // Step 1: Load base Latin font
    for (int i = 0; latin_font_paths[i] != nullptr; ++i) {
        if (std::filesystem::exists(latin_font_paths[i])) {
            loaded_font = io.Fonts->AddFontFromFileTTF(latin_font_paths[i], font_size, &font_cfg);
            if (loaded_font) {
                std::printf("[GUI] Loaded Latin font: %s (%.0fpx)\n", latin_font_paths[i], font_size);
                break;
            }
        }
    }

    if (!loaded_font) {
        // Fallback: default bitmap font with high oversampling
        font_cfg.SizePixels = font_size;
        loaded_font = io.Fonts->AddFontDefault(&font_cfg);
        std::printf("[GUI] Using default bitmap font (%.0fpx)\n", font_size);
    }

    // Step 2: Merge CJK glyphs.
    // For Chinese locale: load full CJK glyph range.
    // For other locales: load minimal CJK glyphs needed for the language
    // switch combo (the characters "中文" U+4E2D U+6587).
    {
        ImFontConfig cjk_cfg;
        cjk_cfg.OversampleH = 2;
        cjk_cfg.OversampleV = 1;
        cjk_cfg.MergeMode = true;  // Merge into the previous font
        cjk_cfg.PixelSnapH = true;

        // Minimal glyph range for the language combo label "中文"
        static const ImWchar minimal_cjk_ranges[] = {
            0x4E2D, 0x4E2D,  // 中
            0x6587, 0x6587,  // 文
            0, 0
        };

        const ImWchar* glyph_ranges = (lang_ == Lang::ZH)
            ? io.Fonts->GetGlyphRangesChineseFull()
            : minimal_cjk_ranges;

        for (int i = 0; cjk_font_paths[i] != nullptr; ++i) {
            if (std::filesystem::exists(cjk_font_paths[i])) {
                ImFont* cjk = io.Fonts->AddFontFromFileTTF(
                    cjk_font_paths[i], font_size, &cjk_cfg,
                    glyph_ranges);
                if (cjk) {
                    std::printf("[GUI] Merged CJK font: %s (%s)\n",
                        cjk_font_paths[i],
                        (lang_ == Lang::ZH) ? "full" : "minimal");
                    break;
                }
            }
        }
    }

    io.FontDefault = loaded_font;

    // Enable anti-aliased font rendering
    io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

    // Style — dark theme with high-contrast, readable colors
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.WindowPadding = ImVec2(10, 8);
    style.ItemSpacing = ImVec2(8, 5);
    style.FramePadding = ImVec2(6, 4);

    // Override colors for clear visibility
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]         = ImVec4(0.13f, 0.13f, 0.15f, 1.00f);
    colors[ImGuiCol_ChildBg]          = ImVec4(0.13f, 0.13f, 0.15f, 1.00f);
    colors[ImGuiCol_Text]             = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_TextDisabled]     = ImVec4(0.55f, 0.55f, 0.55f, 1.00f);
    colors[ImGuiCol_Button]           = ImVec4(0.26f, 0.46f, 0.75f, 1.00f);
    colors[ImGuiCol_ButtonHovered]    = ImVec4(0.36f, 0.56f, 0.88f, 1.00f);
    colors[ImGuiCol_ButtonActive]     = ImVec4(0.18f, 0.36f, 0.65f, 1.00f);
    colors[ImGuiCol_FrameBg]          = ImVec4(0.20f, 0.21f, 0.24f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]   = ImVec4(0.28f, 0.30f, 0.34f, 1.00f);
    colors[ImGuiCol_FrameBgActive]    = ImVec4(0.32f, 0.34f, 0.38f, 1.00f);
    colors[ImGuiCol_SliderGrab]       = ImVec4(0.50f, 0.70f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.65f, 0.85f, 1.00f, 1.00f);
    colors[ImGuiCol_CheckMark]        = ImVec4(0.50f, 0.80f, 1.00f, 1.00f);
    colors[ImGuiCol_Separator]        = ImVec4(0.40f, 0.42f, 0.48f, 0.60f);
    colors[ImGuiCol_Header]           = ImVec4(0.26f, 0.46f, 0.75f, 0.55f);
    colors[ImGuiCol_HeaderHovered]    = ImVec4(0.36f, 0.56f, 0.88f, 0.80f);
    colors[ImGuiCol_HeaderActive]     = ImVec4(0.30f, 0.50f, 0.82f, 1.00f);
    colors[ImGuiCol_PopupBg]          = ImVec4(0.16f, 0.16f, 0.19f, 0.98f);
    colors[ImGuiCol_TitleBg]          = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgActive]    = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]      = ImVec4(0.10f, 0.10f, 0.12f, 0.80f);
    colors[ImGuiCol_ScrollbarGrab]    = ImVec4(0.40f, 0.42f, 0.48f, 1.00f);

    // Setup Platform/Renderer backends
    ImGui_ImplSDL2_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer2_Init(renderer_);

    // Set SDL render scale to match DPI.
    // ImGui_ImplSDLRenderer2_RenderDrawData() passes vertex coordinates to
    // SDL_RenderGeometryRaw() in logical pixels, but on HiDPI the viewport is
    // in framebuffer pixels. Without SDL_RenderSetScale, clip rects (scaled by
    // FramebufferScale) don't match the unscaled vertex coords, so everything
    // outside the top-left quadrant gets clipped away.
    // Setting SDL_RenderSetScale makes SDL scale all render operations
    // (including SDL_RenderGeometryRaw vertices), and ImGui's backend detects
    // this and stops scaling clip rects, keeping both in logical pixel space.
    SDL_RenderSetScale(renderer_, dpi_scale_, dpi_scale_);

    initialized_ = true;

    // Enable file drag-and-drop
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    // Initialize native file dialog library
    NFD_Init();

    running_ = true;
    return true;
}

void GuiApp::Run() {
    while (running_) {
        if (!HandleEvents()) break;
        RenderFrame();
    }
}

void GuiApp::Shutdown() {
    // Guard against double-shutdown (destructor + explicit call)
    if (!initialized_) return;
    initialized_ = false;

    player_.Close();

    if (video_texture_) { SDL_DestroyTexture(video_texture_); video_texture_ = nullptr; }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
    if (window_)   { SDL_DestroyWindow(window_);     window_ = nullptr; }

    NFD_Quit();
    SDL_Quit();
}

bool GuiApp::HandleEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);
        switch (event.type) {
            case SDL_QUIT:
                running_ = false;
                return false;
            case SDL_DROPFILE:
                OnFileDrop(event.drop.file);
                SDL_free(event.drop.file);
                break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    win_width_ = event.window.data1;
                    win_height_ = event.window.data2;
                }
                break;
            case SDL_KEYDOWN:
                if (!ImGui::GetIO().WantCaptureKeyboard) {
                    switch (event.key.keysym.sym) {
                        case SDLK_SPACE:
                            if (player_.IsPlaying()) player_.TogglePause();
                            break;
                        case SDLK_LEFT:
                            if (player_.IsPlaying())
                                player_.Seek(std::max(0.0, player_.GetCurrentTime() - 5.0));
                            break;
                        case SDLK_RIGHT:
                            if (player_.IsPlaying())
                                player_.Seek(player_.GetCurrentTime() + 5.0);
                            break;
                    }
                }
                break;
        }
    }
    return true;
}

void GuiApp::OnFileDrop(const char* path) {
    OpenFile(std::string(path));
}

void GuiApp::OpenFile(const std::string& path) {
    // Reset state
    has_detection_ = false;
    detected_offset_ms_ = 0.0;
    manual_offset_ms_ = 0.0;
    offset_input_ms_ = 0.0;
    offset_just_applied_ = false;
    detect_status_.clear();
    save_status_.clear();

    if (video_texture_) {
        SDL_DestroyTexture(video_texture_);
        video_texture_ = nullptr;
        tex_width_ = tex_height_ = 0;
    }

    if (!player_.Open(path)) {
        detect_status_ = "Failed to open file!";
        return;
    }

    // Auto-generate default output path: <dir>/<stem>_synced.<ext>
    auto inpath = std::filesystem::path(path);
    auto stem = inpath.stem().string();
    auto ext = inpath.extension().string();
    auto dir = inpath.parent_path();
    save_output_path_ = (dir / (stem + "_synced" + ext)).string();
    std::strncpy(output_path_buf_, save_output_path_.c_str(),
                 sizeof(output_path_buf_) - 1);
    output_path_buf_[sizeof(output_path_buf_) - 1] = '\0';
}

void GuiApp::RenderFrame() {
    // Update logical window size each frame (may change from resize events)
    SDL_GetWindowSize(window_, &win_width_, &win_height_);

    // Rebuild fonts if language was switched (e.g. need CJK glyphs)
    if (need_font_rebuild_) {
        need_font_rebuild_ = false;
        ImGuiIO& io = ImGui::GetIO();

        // Invalidate current renderer font texture
        ImGui_ImplSDLRenderer2_DestroyFontsTexture();

        io.Fonts->Clear();

        float font_size = 16.0f;
        ImFont* loaded_font = nullptr;

        static const char* latin_font_paths[] = {
            "/System/Library/Fonts/SFNS.ttf",
            "/System/Library/Fonts/SFNSText.ttf",
            "/System/Library/Fonts/Helvetica.ttc",
            "/System/Library/Fonts/Supplemental/Arial.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "C:\\Windows\\Fonts\\segoeui.ttf",
            "C:\\Windows\\Fonts\\arial.ttf",
            nullptr
        };
        static const char* cjk_font_paths[] = {
            "/System/Library/Fonts/STHeiti Light.ttc",
            "/System/Library/Fonts/PingFang.ttc",
            "/Library/Fonts/Arial Unicode.ttf",
            "/System/Library/Fonts/Hiragino Sans GB.ttc",
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/droid/DroidSansFallback.ttf",
            "C:\\Windows\\Fonts\\msyh.ttc",
            "C:\\Windows\\Fonts\\simsun.ttc",
            nullptr
        };

        ImFontConfig font_cfg;
        font_cfg.OversampleH = 3;
        font_cfg.OversampleV = 2;
        font_cfg.PixelSnapH = false;

        for (int i = 0; latin_font_paths[i] != nullptr; ++i) {
            if (std::filesystem::exists(latin_font_paths[i])) {
                loaded_font = io.Fonts->AddFontFromFileTTF(latin_font_paths[i], font_size, &font_cfg);
                if (loaded_font) break;
            }
        }
        if (!loaded_font) {
            font_cfg.SizePixels = font_size;
            loaded_font = io.Fonts->AddFontDefault(&font_cfg);
        }

        if (lang_ == Lang::ZH) {
            ImFontConfig cjk_cfg;
            cjk_cfg.OversampleH = 2;
            cjk_cfg.OversampleV = 1;
            cjk_cfg.MergeMode = true;
            cjk_cfg.PixelSnapH = true;
            for (int i = 0; cjk_font_paths[i] != nullptr; ++i) {
                if (std::filesystem::exists(cjk_font_paths[i])) {
                    ImFont* cjk = io.Fonts->AddFontFromFileTTF(
                        cjk_font_paths[i], font_size, &cjk_cfg,
                        io.Fonts->GetGlyphRangesChineseFull());
                    if (cjk) break;
                }
            }
        } else {
            // English mode: load minimal CJK for language combo label "中文"
            static const ImWchar minimal_cjk_ranges[] = {
                0x4E2D, 0x4E2D,  // 中
                0x6587, 0x6587,  // 文
                0, 0
            };
            ImFontConfig cjk_cfg;
            cjk_cfg.OversampleH = 2;
            cjk_cfg.OversampleV = 1;
            cjk_cfg.MergeMode = true;
            cjk_cfg.PixelSnapH = true;
            for (int i = 0; cjk_font_paths[i] != nullptr; ++i) {
                if (std::filesystem::exists(cjk_font_paths[i])) {
                    ImFont* cjk = io.Fonts->AddFontFromFileTTF(
                        cjk_font_paths[i], font_size, &cjk_cfg,
                        minimal_cjk_ranges);
                    if (cjk) break;
                }
            }
        }

        io.FontDefault = loaded_font;
        io.Fonts->Build();

        // Recreate renderer font texture
        ImGui_ImplSDLRenderer2_CreateFontsTexture();

        std::printf("[GUI] Font rebuilt for language: %s\n", LangToString(lang_));
    }

    // Start ImGui frame
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // Video refresh (updates texture)
    double remaining = 0.01;
    if (player_.IsPlaying()) {
        remaining = player_.VideoRefresh(renderer_, &video_texture_,
                                          &tex_width_, &tex_height_);
    }

    // Draw the UI controls using logical (window) pixel coordinates.
    // ImGui's coordinate space = io.DisplaySize = SDL_GetWindowSize (logical pixels).
    DrawControlPanel();
    DrawVideoArea();

    // Finalize ImGui frame
    ImGui::Render();

    // === SDL Render Pass ===
    // 1. Clear screen
    SDL_SetRenderDrawColor(renderer_, 24, 24, 28, 255);
    SDL_RenderClear(renderer_);

    // 2. Draw video texture in the upper area (behind ImGui)
    //    SDL_RenderSetScale is active, so SDL_RenderCopy coordinates are in
    //    logical pixels (SDL will scale to framebuffer internally).
    if (video_texture_ && tex_width_ > 0 && tex_height_ > 0) {
        int video_area_h = win_height_ - ctrl_panel_height_;
        int video_area_w = win_width_;
        if (video_area_h < 100) video_area_h = 100;

        float scale_w = static_cast<float>(video_area_w) / tex_width_;
        float scale_h = static_cast<float>(video_area_h) / tex_height_;
        float scale = std::min(scale_w, scale_h);

        int disp_w = static_cast<int>(tex_width_ * scale);
        int disp_h = static_cast<int>(tex_height_ * scale);
        int x = (video_area_w - disp_w) / 2;
        int y = (video_area_h - disp_h) / 2;

        SDL_Rect dst = { x, y, disp_w, disp_h };
        SDL_RenderCopy(renderer_, video_texture_, nullptr, &dst);
    }

    // 3. Draw ImGui overlay on top
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer_);
    SDL_RenderPresent(renderer_);

    // Sleep to maintain refresh rate
    if (remaining > 0.001) {
        unsigned int delay_ms = static_cast<unsigned int>(remaining * 1000);
        SDL_Delay(std::min(delay_ms, 10u));
    }
}

void GuiApp::DrawVideoArea() {
    // Draw "Drop file here" overlay when no file is loaded
    if (!player_.IsPlaying()) {
        float video_area_h = static_cast<float>(win_height_ - ctrl_panel_height_);
        if (video_area_h < 50) video_area_h = 50;

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(win_width_), video_area_h));
        ImGui::Begin("##VideoArea", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs);

        auto avail = ImGui::GetContentRegionAvail();
        const char* text = str_->drag_drop_hint;
        auto text_size = ImGui::CalcTextSize(text);
        ImGui::SetCursorPos(ImVec2(
            (avail.x - text_size.x) * 0.5f,
            (avail.y - text_size.y) * 0.5f));
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", text);

        ImGui::End();
    }
}

void GuiApp::DrawControlPanel() {
    // The control panel is anchored at the bottom of the window.
    // ImGui coordinates = logical pixels (io.DisplaySize = window size).
    float panel_y = static_cast<float>(win_height_ - ctrl_panel_height_);

    ImGui::SetNextWindowPos(ImVec2(0, panel_y));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(win_width_),
                                     static_cast<float>(ctrl_panel_height_)));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.13f, 0.13f, 0.15f, 0.98f));
    ImGui::Begin("##Controls", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    // ---- Row 1: File controls & info ----
    if (ImGui::Button(str_->open_file)) {
        ShowOpenFileDialog();
    }

    if (player_.IsPlaying()) {
        ImGui::SameLine();
        std::string filename = std::filesystem::path(player_.GetFilePath()).filename().string();
        ImGui::Text("%s  |  %dx%d  %.1ffps  %.1fs",
            filename.c_str(),
            player_.GetVideoWidth(), player_.GetVideoHeight(),
            player_.GetVideoFps(), player_.GetDuration());
    }

    ImGui::Separator();

    // ---- Row 2: Playback progress ----
    if (player_.IsPlaying()) {
        double current = player_.GetCurrentTime();
        double duration = player_.GetDuration();

        float progress = duration > 0 ? static_cast<float>(current / duration) : 0.0f;
        ImGui::PushItemWidth(-100);
        if (ImGui::SliderFloat("##Progress", &progress, 0.0f, 1.0f,
                                FormatTime(current).c_str())) {
            player_.Seek(progress * duration);
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::Text("%s", FormatTime(duration).c_str());
    }

    // ---- Row 3: Playback controls ----
    if (player_.IsPlaying()) {
        if (ImGui::Button(player_.IsPaused() ? str_->play : str_->pause)) {
            player_.TogglePause();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1.0f), "%s", str_->seek_label);
        ImGui::SameLine();
        if (ImGui::Button("<<5s")) { player_.Seek(std::max(0.0, player_.GetCurrentTime() - 5.0)); }
        ImGui::SameLine();
        if (ImGui::Button(">>5s")) { player_.Seek(player_.GetCurrentTime() + 5.0); }
        ImGui::SameLine();
        if (ImGui::Button("<1s")) { player_.Seek(std::max(0.0, player_.GetCurrentTime() - 1.0)); }
        ImGui::SameLine();
        if (ImGui::Button(">1s")) { player_.Seek(player_.GetCurrentTime() + 1.0); }

        ImGui::SameLine();
        ImGui::Text("%s", str_->speed_label);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        char speed_label[16];
        std::snprintf(speed_label, sizeof(speed_label), "%.2fx", play_speed_);
        if (ImGui::BeginCombo("##SpeedCombo", speed_label)) {
            for (int i = 0; i < kNumSpeedPresets; ++i) {
                char label[16];
                std::snprintf(label, sizeof(label), "%.2fx", kSpeedPresets[i]);
                bool selected = (std::abs(play_speed_ - kSpeedPresets[i]) < 0.001f);
                if (ImGui::Selectable(label, selected)) {
                    play_speed_ = kSpeedPresets[i];
                    player_.SetSpeed(play_speed_);
                }
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();

    // ---- Row 4: Offset adjustment ----
    ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "%s", str_->offset_title);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1.0f), "%s", str_->offset_hint);

    // Auto detect button
    if (player_.IsPlaying()) {
        if (detecting_) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                str_->detecting, detect_status_.c_str());
        } else {
            if (ImGui::Button(str_->auto_detect)) {
                RunAutoDetect();
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                "%s", str_->auto_detect_experimental);
            if (has_detection_) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                    str_->detected_label, detected_offset_ms_);
                ImGui::SameLine();
                char apply_label[64];
                std::snprintf(apply_label, sizeof(apply_label), "%s##detected", str_->apply);
                if (ImGui::Button(apply_label)) {
                    manual_offset_ms_ = detected_offset_ms_;
                    offset_input_ms_ = detected_offset_ms_;
                    player_.SetOffsetMs(manual_offset_ms_);
                    offset_just_applied_ = true;
                    offset_applied_tick_ = SDL_GetTicks();
                }
            }
        }
    }

    // Manual offset input — always visible
    // Show whether the input differs from the currently applied value.
    bool input_dirty = (std::abs(offset_input_ms_ - manual_offset_ms_) > 0.05);
    ImGui::Text("%s", str_->offset_ms_label);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    // Tint the input background when value is pending (not yet applied)
    if (input_dirty) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.45f, 0.35f, 0.10f, 1.0f));
    }
    if (ImGui::InputDouble("##offset_input", &offset_input_ms_, 0, 0, "%.1f",
                           ImGuiInputTextFlags_EnterReturnsTrue)) {
        // User pressed Enter — apply the value
        manual_offset_ms_ = offset_input_ms_;
        player_.SetOffsetMs(manual_offset_ms_);
        offset_just_applied_ = true;
        offset_applied_tick_ = SDL_GetTicks();
    }
    if (input_dirty) {
        ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    // Show pending/applied feedback
    if (input_dirty) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
            "%s", str_->offset_press_enter);
        ImGui::SameLine();
    } else if (offset_just_applied_) {
        Uint32 elapsed = SDL_GetTicks() - offset_applied_tick_;
        if (elapsed < kAppliedFlashMs) {
            float alpha = 1.0f - static_cast<float>(elapsed) / kAppliedFlashMs;
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, alpha),
                "%s", str_->offset_applied);
            ImGui::SameLine();
        } else {
            offset_just_applied_ = false;
        }
    }
    char reset_label[64];
    std::snprintf(reset_label, sizeof(reset_label), "%s##offset_reset", str_->reset);
    if (ImGui::Button(reset_label)) {
        manual_offset_ms_ = 0.0;
        offset_input_ms_ = 0.0;
        player_.SetOffsetMs(0.0);
        offset_just_applied_ = true;
        offset_applied_tick_ = SDL_GetTicks();
    }
    ImGui::SameLine();

    // Fine adjustment buttons with unique IDs
    auto OffsetBtn = [&](const char* label, const char* id, double delta) {
        char btn_label[32];
        std::snprintf(btn_label, sizeof(btn_label), "%s##%s", label, id);
        if (ImGui::Button(btn_label)) {
            manual_offset_ms_ += delta;
            offset_input_ms_ = manual_offset_ms_;
            player_.SetOffsetMs(manual_offset_ms_);
            offset_just_applied_ = true;
            offset_applied_tick_ = SDL_GetTicks();
        }
    };
    OffsetBtn("-100", "om100", -100); ImGui::SameLine();
    OffsetBtn("-10",  "om10",  -10);  ImGui::SameLine();
    OffsetBtn("+10",  "op10",  +10);  ImGui::SameLine();
    OffsetBtn("+100", "op100", +100);

    // Offset slider — full width
    float offset_f = static_cast<float>(manual_offset_ms_);
    ImGui::PushItemWidth(-1);
    if (ImGui::SliderFloat("##offset_slider", &offset_f, -2000.0f, 2000.0f, "%.0f ms")) {
        manual_offset_ms_ = offset_f;
        offset_input_ms_ = offset_f;
        player_.SetOffsetMs(manual_offset_ms_);
        offset_just_applied_ = true;
        offset_applied_tick_ = SDL_GetTicks();
    }
    ImGui::PopItemWidth();

    // ---- Row 5: Language & Advanced (always on a NEW line) ----
    // These must NOT use SameLine relative to offset slider or save section.
    // We place them on a dedicated line, right-aligned using SetCursorPosX.
    {
        const ImGuiStyle& sty = ImGui::GetStyle();
        float adv_w    = ImGui::CalcTextSize(str_->advanced).x + sty.FramePadding.x * 2 + 20.0f; // checkbox
        float lang_lbl = ImGui::CalcTextSize(str_->language_label).x + ImGui::CalcTextSize(":").x;
        float combo_w  = 55.0f;
        float group_w  = lang_lbl + sty.ItemSpacing.x + combo_w + sty.ItemSpacing.x * 2 + adv_w;
        float start_x  = ImGui::GetWindowWidth() - group_w - sty.WindowPadding.x;
        if (start_x < sty.WindowPadding.x) start_x = sty.WindowPadding.x;

        ImGui::SetCursorPosX(start_x);

        // Language switch combo
        ImGui::Text("%s:", str_->language_label);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(combo_w);
        const char* lang_labels[] = { "EN", u8"\u4e2d\u6587" };  // EN, 中文
        int lang_idx = (lang_ == Lang::ZH) ? 1 : 0;
        if (ImGui::Combo("##LangCombo", &lang_idx, lang_labels, 2)) {
            Lang new_lang = (lang_idx == 1) ? Lang::ZH : Lang::EN;
            if (new_lang != lang_) {
                SetLang(new_lang);
                need_font_rebuild_ = true;
            }
        }
        ImGui::SameLine();
        ImGui::Checkbox(str_->advanced, &show_advanced_);
    }

    // ---- Row 6: Save output (only when offset != 0 and file is open) ----
    if (player_.IsPlaying() && std::abs(manual_offset_ms_) > 0.5) {
        ImGui::Separator();
        // Row 6a: Output path — [Label] [InputText] [Browse]
        const ImGuiStyle& sty = ImGui::GetStyle();
        float browse_w = ImGui::CalcTextSize(str_->browse).x + sty.FramePadding.x * 2;
        float save_w   = ImGui::CalcTextSize(str_->save_button).x + sty.FramePadding.x * 2;
        float label_w  = ImGui::CalcTextSize(str_->output_path_label).x + sty.ItemSpacing.x;
        float total_w  = ImGui::GetContentRegionAvail().x;
        // Right side: spacing + Browse + spacing + Save
        float right_w  = sty.ItemSpacing.x + browse_w + sty.ItemSpacing.x + save_w;
        float input_w  = total_w - label_w - right_w;
        if (input_w < 80.0f) input_w = 80.0f;

        ImGui::Text("%s", str_->output_path_label);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(input_w);
        ImGui::InputText("##output_path", output_path_buf_, sizeof(output_path_buf_));
        ImGui::SameLine();
        if (ImGui::Button(str_->browse)) {
            ShowSaveFileDialog();
        }
        ImGui::SameLine();

        // Save button (or saving status)
        if (saving_) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                str_->saving, save_status_.c_str());
        } else {
            if (ImGui::Button(str_->save_button)) {
                save_output_path_ = std::string(output_path_buf_);
                SaveCorrectedVideo();
            }
        }
        // Save status on a separate line
        if (!saving_ && !save_status_.empty()) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                "%s", save_status_.c_str());
        }
    }

    if (show_advanced_) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "%s", str_->detection_params);

        ImGui::SetNextItemWidth(100);
        float win_sec = static_cast<float>(config_.segment_window_sec);
        if (ImGui::InputFloat(str_->window_sec, &win_sec, 1.0f, 5.0f, "%.1f"))
            config_.segment_window_sec = std::max(1.0, static_cast<double>(win_sec));

        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        float step_sec = static_cast<float>(config_.segment_step_sec);
        if (ImGui::InputFloat(str_->step_sec, &step_sec, 0.5f, 2.5f, "%.1f"))
            config_.segment_step_sec = std::max(0.5, static_cast<double>(step_sec));

        ImGui::SetNextItemWidth(100);
        float thresh = static_cast<float>(config_.offset_threshold_ms);
        if (ImGui::InputFloat(str_->threshold_ms, &thresh, 5.0f, 20.0f, "%.0f"))
            config_.offset_threshold_ms = std::max(10.0, static_cast<double>(thresh));

        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        float conf = static_cast<float>(config_.min_global_confidence);
        if (ImGui::InputFloat(str_->min_confidence, &conf, 0.05f, 0.1f, "%.2f"))
            config_.min_global_confidence = std::clamp(static_cast<double>(conf), 0.1, 0.9);

        ImGui::SetNextItemWidth(100);
        float range = static_cast<float>(config_.onset_align.search_range_ms);
        if (ImGui::InputFloat(str_->search_range_ms, &range, 100.0f, 500.0f, "%.0f"))
            config_.onset_align.search_range_ms = std::max(100.0, static_cast<double>(range));
    }

    // Status line at the bottom
    if (!detect_status_.empty() && !detecting_) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "%s", detect_status_.c_str());
    }

    ImGui::End();
    ImGui::PopStyleColor();
}

void GuiApp::RunAutoDetect() {
    if (detecting_ || !player_.IsPlaying()) return;

    detecting_ = true;
    detect_status_ = "Running detection...";

    std::string path = player_.GetFilePath();
    Config cfg = config_;

    // Run in background thread
    std::thread([this, path, cfg]() {
        SyncPipeline pipeline;
        Config c = cfg;
        c.log_level = "info";
        pipeline.Configure(c);

        // Use a temp output to detect without saving
        std::string tmp = "/tmp/_avsync_detect_tmp.mp4";
        bool ok = pipeline.Process(path, tmp);

        auto& results = pipeline.GetDetectionResults();
        auto& decisions = pipeline.GetCorrectionDecisions();

        if (!decisions.empty() && decisions[0].apply) {
            detected_offset_ms_ = decisions[0].correction_ms;
            has_detection_ = true;
            detect_status_ = str_->detect_complete;
        } else if (!results.empty()) {
            // Find median of non-skipped results
            std::vector<double> offsets;
            for (auto& r : results) {
                if (!r.skipped) offsets.push_back(r.offset_ms);
            }
            if (!offsets.empty()) {
                std::sort(offsets.begin(), offsets.end());
                detected_offset_ms_ = offsets[offsets.size() / 2];
                has_detection_ = true;
                detect_status_ = str_->detect_complete_raw;
            } else {
                detect_status_ = str_->detect_failed_no_segments;
            }
        } else {
            detect_status_ = str_->detect_failed;
        }

        // Clean up temp file
        std::remove(tmp.c_str());
        detecting_ = false;
    }).detach();
}

void GuiApp::SaveCorrectedVideo() {
    if (saving_ || !player_.IsPlaying()) return;

    // Use user-specified output path (from text buffer), or generate default
    if (save_output_path_.empty()) {
        auto inpath = std::filesystem::path(player_.GetFilePath());
        auto stem = inpath.stem().string();
        auto ext = inpath.extension().string();
        auto dir = inpath.parent_path();
        save_output_path_ = (dir / (stem + "_synced" + ext)).string();
    }

    saving_ = true;
    save_status_ = "Saving...";

    double offset = manual_offset_ms_;
    std::string input = player_.GetFilePath();
    std::string output = save_output_path_;

    std::thread([this, input, output, offset]() {
        SyncPipeline pipeline;
        Config cfg = config_;
        cfg.manual_offset_ms = offset;
        cfg.log_level = "info";
        pipeline.Configure(cfg);

        bool ok = pipeline.Process(input, output);

        if (ok) {
            save_status_ = std::string(str_->save_ok_prefix) + output;
        } else {
            save_status_ = str_->save_failed;
        }
        saving_ = false;
    }).detach();
}

void GuiApp::ShowOpenFileDialog() {
    nfdchar_t* out_path = nullptr;
    nfdfilteritem_t filters[2] = {
        { str_->video_filter_name, "mp4,mkv,avi,mov,flv,wmv,webm,ts,m4v,mpg,mpeg" },
        { str_->all_filter_name,   "*" }
    };

    nfdresult_t result = NFD_OpenDialog(&out_path, filters, 2, nullptr);
    if (result == NFD_OKAY && out_path) {
        OpenFile(std::string(out_path));
        NFD_FreePath(out_path);
    }
    // NFD_CANCEL: user cancelled, do nothing
}

void GuiApp::ShowSaveFileDialog() {
    nfdchar_t* out_path = nullptr;
    nfdfilteritem_t filters[2] = {
        { str_->video_filter_name, "mp4,mkv,avi,mov" },
        { str_->all_filter_name,   "*" }
    };

    // Pre-fill with current default output filename
    std::string default_name;
    std::string default_dir;
    if (!save_output_path_.empty()) {
        auto p = std::filesystem::path(save_output_path_);
        default_name = p.filename().string();
        default_dir = p.parent_path().string();
    }

    nfdresult_t result = NFD_SaveDialog(&out_path, filters, 2,
                                         default_dir.empty() ? nullptr : default_dir.c_str(),
                                         default_name.empty() ? nullptr : default_name.c_str());
    if (result == NFD_OKAY && out_path) {
        save_output_path_ = std::string(out_path);
        std::strncpy(output_path_buf_, save_output_path_.c_str(),
                     sizeof(output_path_buf_) - 1);
        output_path_buf_[sizeof(output_path_buf_) - 1] = '\0';
        NFD_FreePath(out_path);
    }
}

std::string GuiApp::FormatTime(double seconds) {
    if (std::isnan(seconds) || seconds < 0) seconds = 0;
    int mins = static_cast<int>(seconds) / 60;
    int secs = static_cast<int>(seconds) % 60;
    int ms   = static_cast<int>((seconds - static_cast<int>(seconds)) * 10);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d.%d", mins, secs, ms);
    return buf;
}

}  // namespace gui
}  // namespace avsync
