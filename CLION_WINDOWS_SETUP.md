# CLion Windows 开发环境配置指南

## 前置要求

### 1. 安装必要软件
- **MSYS2**: https://www.msys2.org/
- **Visual Studio Build Tools** 或 **Visual Studio Community**
- **vcpkg**: Microsoft C++ 包管理器
- **CMake**: 3.16+

### 2. 环境变量设置
设置以下环境变量（在Windows系统环境变量中设置）：

```bash
# MSYS2 路径
MSYS2_ROOT=C:\msys64

# vcpkg 路径  
VCPKG_ROOT=C:\vcpkg

# 添加到 PATH
PATH=%MSYS2_ROOT%\usr\bin;%VCPKG_ROOT%;%PATH%
```

## CLion 配置步骤

### 1. 工具链配置

1. 打开 CLion → File → Settings → Build, Execution, Deployment → Toolchains
2. 添加新的工具链，配置如下：

```
Name: Windows MSYS2 + MSVC
Environment: MSYS2
CMake: %MSYS2_ROOT%/usr/bin/cmake.exe
C Compiler: %MSYS2_ROOT%/usr/bin/gcc.exe
C++ Compiler: %MSYS2_ROOT%/usr/bin/g++.exe
Debugger: %MSYS2_ROOT%/usr/bin/gdb.exe
Make: %MSYS2_ROOT%/usr/bin/make.exe
```

### 2. CMake 配置

在 CLion 的 CMake 配置中设置以下参数：

```cmake
-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
-DCMAKE_BUILD_TYPE=Release
-DAVSYNC_ENABLE_SYNCNET=ON
-DAVSYNC_ENABLE_GUI=ON
-DAVSYNC_ENABLE_TESTS=ON
```

**在 CLion 中设置方法：**
1. 打开 CLion → File → Settings → Build, Execution, Deployment → CMake
2. 在 "CMake options" 字段中添加上述参数
3. 确保 "Build directory" 设置为项目根目录下的 `build` 或类似目录

### 3. vcpkg 依赖安装

在 MSYS2 终端中执行以下命令安装依赖：

```bash
# 进入项目目录
cd /c/path/to/av-auto-sync

# 使用 vcpkg 安装依赖
vcpkg install \
    ffmpeg \
    opencv[imgproc,objdetect] \
    sdl2 \
    nlohmann-json \
    onnxruntime
```

### 4. 解决常见问题

#### 问题1: SDL2main 链接错误
**症状**: `unresolved external symbol WinMain`
**解决方案**: 确保 CMakeLists.txt 中正确链接 SDL2main

#### 问题2: FFmpeg 库找不到
**症状**: `cannot find -lavcodec` 等链接错误
**解决方案**: 检查 vcpkg 是否正确安装 FFmpeg，确保 `vcpkg.cmake` 工具链文件被正确引用

#### 问题3: OpenCV 头文件找不到
**症状**: `fatal error: opencv2/core.hpp: No such file or directory`
**解决方案**: 确保 OpenCV 组件正确安装，CMake 配置中包含 OpenCV 路径

## 手动构建（备用方案）

如果 CLion 配置有问题，可以使用提供的批处理脚本：

```bash
# 在 MSYS2 终端中执行
./build_windows.bat
```

## 调试配置

### 1. 调试器配置
在 CLion 中配置调试器：
1. Run → Edit Configurations
2. 添加 CMake Application 配置
3. 选择要调试的可执行文件（avsync 或 avsync_gui）
4. 设置程序参数和工作目录

### 2. 调试技巧
- 使用 CLion 的内置调试器设置断点
- 查看变量值和调用栈
- 使用条件断点进行复杂调试

## 性能优化

### 1. 编译优化
- 使用 Release 模式进行最终构建
- 启用并行编译：`-j8`（根据 CPU 核心数调整）

### 2. 内存优化
- 关闭不必要的调试符号
- 使用预编译头文件（如果支持）

## 故障排除

### 常见错误及解决方案

1. **CMake 配置失败**
   - 检查 vcpkg 工具链文件路径是否正确
   - 确认所有依赖包已安装

2. **链接错误**
   - 检查库文件路径
   - 确认库文件版本匹配

3. **运行时错误**
   - 检查 DLL 文件是否在 PATH 中
   - 确认依赖库的运行时版本

### 获取帮助
- 查看项目 README.md
- 检查 GitHub Issues
- 查看构建日志中的详细错误信息

## 注意事项

1. **路径分隔符**: Windows 使用反斜杠 `\`，但在 MSYS2 环境中使用正斜杠 `/`
2. **环境变量**: 确保在 CLion 启动前设置好所有必要的环境变量
3. **权限**: 以管理员身份运行 MSYS2 和 CLion 可能解决某些权限问题
4. **版本兼容**: 确保所有工具和库的版本兼容

通过以上配置，你应该能够在 Windows 上使用 CLion 成功构建和调试 av-auto-sync 项目。
