# GameCore, another game engine

## Build instructions

It's strongly recommended to set the `CPM_CACHE` environment variable to avoid re-downloading dependencies when switching presets or regenerating the build directory.

### Prerequisites

- CMake 3.25 or later
- A C++20 compiler
- Git
- [Vulkan SDK](https://www.lunarg.com/vulkan-sdk/) (including [volk](https://github.com/zeux/volk))

### Linux

Ensure the Vulkan headers and Volk are installed on your system.
On Arch Linux, install the [`vulkan-headers`](https://archlinux.org/packages/extra/any/vulkan-headers/) and [`volk`](https://archlinux.org/packages/extra/x86_64/volk/) packages.

``` bash
git clone https://github.com/bailwillharr/gamecore.git
cd gamecore
cmake --preset=$BUILD_PRESET .
cd out/build/$BUILD_PRESET
cmake --build . --parallel
```
Where `$BUILD_PRESET` is one of `x64-debug-linux`, `x64-release-linux`, `x64-dist-linux`, or `x64-dist-linux-profiling`.

### Windows

Ensure you have Visual Studio 2022 installed with the 'Desktop development with C++' workload with the 'C++ CMake tools for Windows' component.

Ensure the Vulkan SDK is installed and 'volk' was selected during installation.

Select 'File -> Clone Repository...' in Visual Studio and use the URL `https://github.com/bailwillharr/gamecore.git`.
Once opened, select the appropriate build preset and build the project.

It's recommended to open the repository directly in Visual Studio as a folder instead of generating the solution file using the CMake GUI or `cmake.exe`.
Visual Studio can build the CMake project without needing a `.sln` solution file.