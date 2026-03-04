# Windows 快速开始指南

## 1. 环境准备

### 安装必要软件
```bash
# 1. 安装 MSYS2
# 下载: https://www.msys2.org/
# 安装到: C:\msys64

# 2. 安装 vcpkg
cd C:\
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat

# 3. 设置环境变量
setx VCPKG_ROOT "C:\vcpkg"
setx MSYS2_ROOT "C:\msys64"
# 将 %MSYS2_ROOT%\usr\bin 和 %VCPKG_ROOT% 添加到 PATH
```

## 2. 构建项目

### 方法一：使用Bash脚本（推荐）
```bash
# 在 MSYS2 终端中执行
./build_windows.sh
```

### 方法二：使用批处理脚本
```bash
# 在 MSYS2 终端中执行
./build_windows.bat
```

### 方法三：手动构建
```bash
# 在 MSYS2 终端中执行
cd /c/path/to/av-auto-sync

# 安装依赖
vcpkg install --triplet x64-windows

# 配置构建
mkdir build_windows && cd build_windows
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake \
    -DAVSYNC_ENABLE_SYNCNET=ON \
    -DAVSYNC_ENABLE_GUI=ON \
    -DAVSYNC_ENABLE_TESTS=ON

# 构建
cmake --build . --config Release --parallel
```

## 3. CLion 配置（可选）

### 工具链设置
1. File → Settings → Build, Execution, Deployment → Toolchains
2. 添加工具链：Windows MSYS2 + MSVC
3. 设置编译器路径为 MSYS2 下的 gcc/g++

### CMake 配置
在 CMake options 中添加：
```
-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
-DCMAKE_BUILD_TYPE=Release
-DAVSYNC_ENABLE_SYNCNET=ON
-DAVSYNC_ENABLE_GUI=ON
```

## 4. 运行程序

### CLI 版本
```bash
./build_windows/bin/avsync.exe --help
```

### GUI 版本
```bash
./build_windows/bin/avsync_gui.exe
```

## 5. 故障排除

### 常见问题

**Q: 构建失败，提示找不到库文件**
A: 检查 vcpkg 是否正确安装依赖，重新运行 `vcpkg install --triplet x64-windows`

**Q: CLion 找不到头文件**
A: 确保 CMake 配置中包含 vcpkg 工具链文件路径

**Q: 运行时缺少 DLL**
A: 将 vcpkg 的 installed\x64-windows\bin 目录添加到 PATH

### 调试技巧
- 使用 CLion 内置调试器设置断点
- 查看构建日志中的详细错误信息
- 检查 vcpkg 安装状态：`vcpkg list`

## 6. 文件说明

- `build_windows.bat` - Windows 自动构建脚本
- `vcpkg.json` - 项目依赖清单文件
- `CLION_WINDOWS_SETUP.md` - 详细 CLion 配置指南

## 7. 联系方式

如有问题，请查看：
- 项目 README.md
- GitHub Issues
- 构建日志文件

---
**提示**: 首次构建可能需要较长时间，因为 vcpkg 需要下载和编译所有依赖库。
