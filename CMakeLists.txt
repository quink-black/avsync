cmake_minimum_required(VERSION 3.16)
project(av-auto-sync VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Cross-platform output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Options
option(AVSYNC_ENABLE_SYNCNET "Enable SyncNet detector (requires ONNX Runtime + OpenCV)" OFF)
option(AVSYNC_ENABLE_TESTS "Build tests" ON)
option(AVSYNC_ENABLE_GUI "Build GUI application (requires SDL2)" ON)

# ---------- Dependencies ----------

# FFmpeg (required)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED
    libavformat
    libavcodec
    libavutil
    libswresample
    libswscale
    libavfilter
)

# nlohmann/json for config parsing
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# Sonic time-stretch library (for speed change without pitch shift in GUI)
if(AVSYNC_ENABLE_GUI)
    FetchContent_Declare(
        sonic
        GIT_REPOSITORY https://github.com/waywardgeek/sonic.git
        GIT_TAG master
    )
    FetchContent_MakeAvailable(sonic)

    # sonic is a single C file – build as a static library
    if(NOT TARGET sonic_lib)
        add_library(sonic_lib STATIC ${sonic_SOURCE_DIR}/sonic.c)
        set_target_properties(sonic_lib PROPERTIES LINKER_LANGUAGE C)
        target_include_directories(sonic_lib PUBLIC ${sonic_SOURCE_DIR})
    endif()
endif()

# SDL2 (optional, for GUI)
if(AVSYNC_ENABLE_GUI)
    # Try to find SDL2 via pkg-config first, then cmake
    pkg_check_modules(SDL2 sdl2)
    if(SDL2_FOUND)
        # Use pkg-config provided libraries
        set(SDL2_LIBRARIES ${SDL2_LIBRARIES})
    else()
        find_package(SDL2 QUIET)
        if(SDL2_FOUND)
            set(SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIRS})
            set(SDL2_LIBRARIES SDL2::SDL2)
        else()
            message(WARNING "SDL2 not found, GUI will not be built")
            set(AVSYNC_ENABLE_GUI OFF)
        endif()
    endif()
endif()

# Dear ImGui (for GUI)
if(AVSYNC_ENABLE_GUI)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.8
    )
    FetchContent_MakeAvailable(imgui)

    # Native file dialog (cross-platform open/save dialog)
    FetchContent_Declare(
        nativefiledialog
        GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
        GIT_TAG v1.2.1
    )
    FetchContent_MakeAvailable(nativefiledialog)
endif()

# OpenCV (optional, for SyncNet face/lip detection)
if(AVSYNC_ENABLE_SYNCNET)
    find_package(OpenCV REQUIRED COMPONENTS core imgproc objdetect)
    # ONNX Runtime (optional enhancement, not required for basic SyncNet)
    find_package(onnxruntime QUIET)
    if(onnxruntime_FOUND)
        # Verify the library actually exists (some CMake configs report found but library is missing)
        find_library(ONNXRUNTIME_LIB onnxruntime)
        if(ONNXRUNTIME_LIB)
            message(STATUS "ONNX Runtime found at ${ONNXRUNTIME_LIB}, SyncNet ONNX model support enabled")
            set(ONNXRUNTIME_AVAILABLE ON)
        else()
            message(STATUS "ONNX Runtime package found but library not available, using OpenCV-based detection only")
            set(ONNXRUNTIME_AVAILABLE OFF)
        endif()
    else()
        message(STATUS "ONNX Runtime not found, SyncNet will use OpenCV-based detection only")
        set(ONNXRUNTIME_AVAILABLE OFF)
    endif()
endif()

# ---------- Source files ----------

set(COMMON_SOURCES
    src/common/Types.h
    src/common/Config.h
    src/common/Config.cpp
    src/common/Log.h
)

set(DECODER_SOURCES
    src/decoder/MediaDecoder.h
    src/decoder/MediaDecoder.cpp
)

set(DETECTOR_SOURCES
    src/detector/ISyncDetector.h
    src/detector/SyncDetectorDispatcher.h
    src/detector/SyncDetectorDispatcher.cpp
    src/detector/OnsetAlignDetector.h
    src/detector/OnsetAlignDetector.cpp
)

if(AVSYNC_ENABLE_SYNCNET)
    list(APPEND DETECTOR_SOURCES
        src/detector/SyncNetDetector.h
        src/detector/SyncNetDetector.cpp
    )
endif()

set(AGGREGATOR_SOURCES
    src/aggregator/OffsetAggregator.h
    src/aggregator/OffsetAggregator.cpp
)

set(CORRECTOR_SOURCES
    src/corrector/TimestampCorrector.h
    src/corrector/TimestampCorrector.cpp
)

set(PIPELINE_SOURCES
    src/pipeline/SyncPipeline.h
    src/pipeline/SyncPipeline.cpp
)

# ---------- Library target ----------

add_library(avsync_core STATIC
    ${COMMON_SOURCES}
    ${DECODER_SOURCES}
    ${DETECTOR_SOURCES}
    ${AGGREGATOR_SOURCES}
    ${CORRECTOR_SOURCES}
    ${PIPELINE_SOURCES}
)

target_include_directories(avsync_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(avsync_core PUBLIC
    ${FFMPEG_LIBRARIES}
    nlohmann_json::nlohmann_json
)

target_link_directories(avsync_core PUBLIC
    ${FFMPEG_LIBRARY_DIRS}
)

if(AVSYNC_ENABLE_SYNCNET)
    target_compile_definitions(avsync_core PUBLIC AVSYNC_ENABLE_SYNCNET=1)
    target_link_libraries(avsync_core PUBLIC
        ${OpenCV_LIBS}
    )
    target_include_directories(avsync_core PUBLIC
        ${OpenCV_INCLUDE_DIRS}
    )
    if(ONNXRUNTIME_AVAILABLE)
        target_compile_definitions(avsync_core PUBLIC AVSYNC_HAS_ONNX=1)
        target_link_libraries(avsync_core PUBLIC ${ONNXRUNTIME_LIB})
    endif()
endif()

# ---------- CLI executable ----------

add_executable(avsync src/main.cpp)
target_link_libraries(avsync PRIVATE avsync_core)

# ---------- GUI executable ----------

if(AVSYNC_ENABLE_GUI)
    # Build Dear ImGui as a static library with SDL2 backend
    set(IMGUI_DIR ${imgui_SOURCE_DIR})

    add_library(imgui_sdl2 STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
    )
    target_include_directories(imgui_sdl2 PUBLIC
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${SDL2_INCLUDE_DIRS}
    )
    target_link_libraries(imgui_sdl2 PUBLIC ${SDL2_LIBRARIES})
    target_link_directories(imgui_sdl2 PUBLIC ${SDL2_LIBRARY_DIRS})

    add_executable(avsync_gui WIN32
        src/gui/gui_main.cpp
        src/gui/GuiApp.cpp
        src/gui/PlayerCore.cpp
    )

    target_include_directories(avsync_gui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${SDL2_INCLUDE_DIRS}
    )

    target_link_libraries(avsync_gui PRIVATE
        avsync_core
        imgui_sdl2
        sonic_lib
        nfd
        ${SDL2_LIBRARIES}
    )

    # Windows: link SDL2main (provides WinMain entry point that calls SDL_main/main)
    if(WIN32)
        # When using vcpkg/find_package, SDL2::SDL2main is available as a target
        if(TARGET SDL2::SDL2main)
            target_link_libraries(avsync_gui PRIVATE SDL2::SDL2main)
        else()
            # When using pkg-config, link SDL2main library directly
            target_link_libraries(avsync_gui PRIVATE SDL2main)
        endif()
    endif()

    # macOS: link CoreFoundation for locale detection (I18n.h)
    if(APPLE)
        target_link_libraries(avsync_gui PRIVATE "-framework CoreFoundation")
    endif()

    target_link_directories(avsync_gui PRIVATE
        ${SDL2_LIBRARY_DIRS}
    )

endif()

# ---------- Tests ----------

if(AVSYNC_ENABLE_TESTS)
    enable_testing()

    # Catch2 v3
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.2
    )
    FetchContent_MakeAvailable(Catch2)

    # Unit tests executable
    add_executable(avsync_unit_tests
        tests/unit/test_cross_correlate.cpp
        tests/unit/test_offset_aggregator.cpp
        tests/unit/test_median_filter.cpp
        tests/unit/test_config.cpp
        tests/unit/test_spectral_flux.cpp
    )

    target_link_libraries(avsync_unit_tests PRIVATE
        avsync_core
        Catch2::Catch2WithMain
    )

    target_include_directories(avsync_unit_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    include(CTest)
    include(Catch)
    catch_discover_tests(avsync_unit_tests)
endif()
