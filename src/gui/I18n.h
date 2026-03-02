#pragma once

#include <cstdlib>
#include <cstring>
#include <string>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace avsync {
namespace gui {

enum class Lang { EN, ZH };

// Detect system language at startup
inline Lang DetectSystemLang() {
#ifdef __APPLE__
    // macOS: use CoreFoundation to get preferred language
    CFArrayRef langs = CFLocaleCopyPreferredLanguages();
    if (langs && CFArrayGetCount(langs) > 0) {
        CFStringRef first = (CFStringRef)CFArrayGetValueAtIndex(langs, 0);
        char buf[16] = {};
        CFStringGetCString(first, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(langs);
        if (strncmp(buf, "zh", 2) == 0) return Lang::ZH;
        return Lang::EN;
    }
    if (langs) CFRelease(langs);
#endif
    // Fallback: check LANG / LC_ALL environment variable
    const char* lang = std::getenv("LANG");
    if (!lang) lang = std::getenv("LC_ALL");
    if (!lang) lang = std::getenv("LC_MESSAGES");
    if (lang) {
        if (strncmp(lang, "zh", 2) == 0) return Lang::ZH;
    }
    return Lang::EN;
}

// Parse language string ("en", "zh", etc.) to Lang enum.
// Returns true if parsed successfully, false if unrecognized.
inline bool ParseLang(const char* s, Lang& out) {
    if (!s) return false;
    if (strncmp(s, "zh", 2) == 0) { out = Lang::ZH; return true; }
    if (strncmp(s, "en", 2) == 0) { out = Lang::EN; return true; }
    return false;
}

inline const char* LangToString(Lang l) {
    return (l == Lang::ZH) ? "zh" : "en";
}

// All translatable UI strings
struct Strings {
    // Window title
    const char* window_title;

    // File controls
    const char* open_file;
    const char* drag_drop_hint;
    const char* drag_drop_status;

    // Playback
    const char* play;
    const char* pause;
    const char* seek_label;
    const char* speed_label;

    // Offset adjustment
    const char* offset_title;
    const char* offset_hint;
    const char* auto_detect;
    const char* detecting;
    const char* detect_complete;
    const char* detect_complete_raw;
    const char* detect_failed_no_segments;
    const char* detect_failed;
    const char* detected_label;
    const char* apply;
    const char* offset_ms_label;
    const char* reset;

    // Save
    const char* save_button;
    const char* saving;
    const char* save_ok_prefix;
    const char* save_failed;
    const char* output_path_label;
    const char* browse;
    const char* video_filter_name;
    const char* all_filter_name;

    // Language switch
    const char* language_label;

    // Advanced
    const char* advanced;
    const char* detection_params;
    const char* window_sec;
    const char* step_sec;
    const char* threshold_ms;
    const char* min_confidence;
    const char* search_range_ms;

    // Auto-detect experimental warning
    const char* auto_detect_experimental;

    // Offset feedback
    const char* offset_applied;
    const char* offset_press_enter;
};

inline const Strings& GetStrings(Lang lang) {
    static const Strings en = {
        // window_title
        "AV Sync Tool",
        // open_file
        "Open File",
        // drag_drop_hint
        "Drag & drop a video file here",
        // drag_drop_status
        "Drag and drop a video file onto the window",
        // play / pause
        "  Play  ", " Pause  ",
        // seek_label
        "Seek:",
        // speed_label
        "  Speed:",
        // offset_title
        "AV Offset Adjustment",
        // offset_hint  (describe by correction effect)
        "(positive = shift video earlier to fix audio-ahead)",
        // auto_detect
        "Auto Detect",
        // detecting
        "Detecting... %s",
        // detect_complete
        "Detection complete",
        // detect_complete_raw
        "Detection complete (safety gate rejected, showing raw median)",
        // detect_failed_no_segments
        "Detection failed: no valid segments",
        // detect_failed
        "Detection failed",
        // detected_label
        "Detected: %.1f ms",
        // apply
        "Apply",
        // offset_ms_label
        "Offset (ms):",
        // reset
        "Reset",
        // save_button
        "Save Corrected Video",
        // saving
        "Saving... %s",
        // save_ok_prefix
        "Saved to: ",
        // save_failed
        "Save failed!",
        // output_path_label
        "Output:",
        // browse
        "Browse...",
        // video_filter_name
        "Video Files",
        // all_filter_name
        "All Files",
        // language_label
        "Lang",
        // advanced
        "Advanced",
        // detection_params
        "Detection Parameters",
        // window_sec
        "Window (s)",
        // step_sec
        "Step (s)",
        // threshold_ms
        "Threshold (ms)",
        // min_confidence
        "Min Confidence",
        // search_range_ms
        "Search Range (ms)",
        // auto_detect_experimental
        "[Experimental] Results may be inaccurate. Please verify by preview.",
        // offset_applied
        "Applied!",
        // offset_press_enter
        "(Press Enter to apply)",
    };

    static const Strings zh = {
        // window_title
        u8"音视频同步工具",
        // open_file
        u8"打开文件",
        // drag_drop_hint
        u8"将视频文件拖放到此处",
        // drag_drop_status
        u8"请将视频文件拖放到窗口中",
        // play / pause
        u8"  播放  ", u8" 暂停  ",
        // seek_label
        u8"跳转:",
        // speed_label
        u8"  速度:",
        // offset_title
        u8"音视频偏移调整",
        // offset_hint  (describe by correction effect)
        u8"(正值 = 视频提前播放，修正音频超前的问题)",
        // auto_detect
        u8"自动检测",
        // detecting
        u8"检测中... %s",
        // detect_complete
        u8"检测完成",
        // detect_complete_raw
        u8"检测完成（安全门限拒绝，显示原始中值）",
        // detect_failed_no_segments
        u8"检测失败：无有效片段",
        // detect_failed
        u8"检测失败",
        // detected_label
        u8"检测结果: %.1f 毫秒",
        // apply
        u8"应用",
        // offset_ms_label
        u8"偏移量 (毫秒):",
        // reset
        u8"重置",
        // save_button
        u8"保存修正后的视频",
        // saving
        u8"保存中... %s",
        // save_ok_prefix
        u8"已保存至: ",
        // save_failed
        u8"保存失败！",
        // output_path_label
        u8"输出:",
        // browse
        u8"浏览...",
        // video_filter_name
        u8"视频文件",
        // all_filter_name
        u8"所有文件",
        // language_label
        u8"语言",
        // advanced
        u8"高级设置",
        // detection_params
        u8"检测参数",
        // window_sec
        u8"窗口 (秒)",
        // step_sec
        u8"步长 (秒)",
        // threshold_ms
        u8"阈值 (毫秒)",
        // min_confidence
        u8"最小置信度",
        // search_range_ms
        u8"搜索范围 (毫秒)",
        // auto_detect_experimental
        u8"[实验功能] 结果可能不准确，请通过预览验证",
        // offset_applied
        u8"已应用！",
        // offset_press_enter
        u8"(按回车键应用)",
    };

    return (lang == Lang::ZH) ? zh : en;
}

}  // namespace gui
}  // namespace avsync
