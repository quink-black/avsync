#include "gui/GuiApp.h"
#include "gui/I18n.h"

#include <SDL.h>  // Must include SDL.h in the file containing main() —
                   // on Windows, SDL redefines main -> SDL_main so that
                   // SDL2main can provide the real WinMain entry point.

#include <cstdio>
#include <cstring>
#include <string>

static void PrintUsage(const char* program) {
    std::printf("Usage: %s [options] [input_file]\n\n", program);
    std::printf("AV Sync Tool - GUI for audio-video sync adjustment.\n\n");
    std::printf("Options:\n");
    std::printf("  -h          Show this help message\n");
    std::printf("  -W <width>  Window width  (default: 1280)\n");
    std::printf("  -H <height> Window height (default: 800)\n");
    std::printf("  -L <lang>   UI language: en, zh (default: auto-detect)\n");
    std::printf("\nYou can also drag and drop files onto the window.\n");
}

int main(int argc, char* argv[]) {
    std::string input_file;
    int width = 1280, height = 800;
    bool lang_set = false;
    avsync::gui::Lang lang = avsync::gui::Lang::EN;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-h") == 0) {
            PrintUsage(argv[0]);
            return 0;
        } else if (std::strcmp(argv[i], "-W") == 0 && i + 1 < argc) {
            width = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-H") == 0 && i + 1 < argc) {
            height = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            ++i;
            if (avsync::gui::ParseLang(argv[i], lang)) {
                lang_set = true;
            } else {
                std::fprintf(stderr, "Unknown language: %s (supported: en, zh)\n", argv[i]);
                return 1;
            }
        } else if (argv[i][0] != '-') {
            input_file = argv[i];
        }
    }

    avsync::gui::GuiApp app;

    // Apply explicit language override (before Init to load correct fonts)
    if (lang_set) {
        app.SetLang(lang);
    }

    if (!app.Init(width, height)) {
        std::fprintf(stderr, "Failed to initialize GUI\n");
        return 1;
    }

    // Auto-open file if provided on command line
    if (!input_file.empty()) {
        // Delay open to after first frame
        // We'll use SDL event to trigger it
        SDL_Event event;
        event.type = SDL_DROPFILE;
        event.drop.file = SDL_strdup(input_file.c_str());
        SDL_PushEvent(&event);
    }

    app.Run();
    app.Shutdown();

    return 0;
}
