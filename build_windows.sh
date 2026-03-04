#!/bin/bash

# ==============================================
# Windows Build Script for av-auto-sync (Bash version)
# Requires: MSYS2 + MSVC + vcpkg
# ==============================================

echo "[INFO] Starting Windows build for av-auto-sync..."

# Check if we're in MSYS2 environment
if [ -z "$MSYSTEM" ]; then
    echo "[ERROR] Please run this script from MSYS2 environment!"
    echo "[INFO] Start MSYS2 shell and run: ./build_windows.sh"
    read -p "Press any key to exit..."
    exit 1
fi

echo "[INFO] Detected MSYS2 environment: $MSYSTEM"

# Set build configuration
BUILD_TYPE="Release"
BUILD_DIR="build_windows"
VCPKG_ROOT="${VCPKG_ROOT:-}"

# Check if vcpkg is available
if [ -z "$VCPKG_ROOT" ]; then
    echo "[WARNING] VCPKG_ROOT not set, trying default locations..."
    if [ -f "/c/vcpkg/vcpkg.exe" ]; then
        VCPKG_ROOT="/c/vcpkg"
    elif [ -f "$HOME/vcpkg/vcpkg.exe" ]; then
        VCPKG_ROOT="$HOME/vcpkg"
    else
        echo "[ERROR] vcpkg not found! Please install vcpkg and set VCPKG_ROOT"
        echo "[INFO] Install vcpkg: git clone https://github.com/Microsoft/vcpkg.git"
        echo "[INFO] Then run: ./vcpkg/bootstrap-vcpkg.bat"
        read -p "Press any key to exit..."
        exit 1
    fi
fi

echo "[INFO] Using vcpkg at: $VCPKG_ROOT"

# Check required dependencies
echo "[INFO] Checking dependencies..."
if [ ! -f "$VCPKG_ROOT/vcpkg.exe" ]; then
    echo "[ERROR] vcpkg.exe not found at $VCPKG_ROOT/vcpkg.exe"
    read -p "Press any key to exit..."
    exit 1
fi

# Install required dependencies using manifest mode
echo "[INFO] Installing dependencies using vcpkg manifest (vcpkg.json)..."
"$VCPKG_ROOT/vcpkg.exe" install --triplet x64-windows

if [ $? -ne 0 ]; then
    echo "[ERROR] Failed to install dependencies via vcpkg"
    read -p "Press any key to exit..."
    exit 1
fi

echo "[INFO] Dependencies installed successfully"

# Clean previous build
if [ -d "$BUILD_DIR" ]; then
    echo "[INFO] Removing previous build directory..."
    rm -rf "$BUILD_DIR"
fi

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure with CMake
echo "[INFO] Configuring project with CMake..."
cmake .. \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DAVSYNC_ENABLE_SYNCNET=OFF \
    -DAVSYNC_ENABLE_GUI=ON \
    -DAVSYNC_ENABLE_TESTS=ON

if [ $? -ne 0 ]; then
    echo "[ERROR] CMake configuration failed"
    cd ..
    read -p "Press any key to exit..."
    exit 1
fi

# Build the project
echo "[INFO] Building project..."
cmake --build . --config "$BUILD_TYPE" --parallel

if [ $? -ne 0 ]; then
    echo "[ERROR] Build failed"
    cd ..
    read -p "Press any key to exit..."
    exit 1
fi

echo "[INFO] Build completed successfully!"

# Show output files
echo
echo "[INFO] Built executables:"
if [ -f "bin/avsync.exe" ]; then
    echo "  - avsync.exe (CLI version)"
fi
if [ -f "bin/avsync_gui.exe" ]; then
    echo "  - avsync_gui.exe (GUI version)"
fi
if [ -f "bin/avsync_unit_tests.exe" ]; then
    echo "  - avsync_unit_tests.exe (Unit tests)"
fi

echo
echo "[SUCCESS] Windows build completed!"
echo "[INFO] Executables are in: $BUILD_DIR/bin"

cd ..
read -p "Press any key to exit..."
