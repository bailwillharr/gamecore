# GameCore -- A better game engine than my other one

## Build instructions

### Prerequisites

- CMake 3.25 or later
- A C++20 compiler
- Git
- [Vulkan SDK](https://www.lunarg.com/vulkan-sdk/) (including [volk](https://github.com/zeux/volk))

### Linux

Ensure the Vulkan headers and Volk are installed on your system.
On Arch Linux, install the [`vulkan-headers`](https://archlinux.org/packages/extra/any/vulkan-headers/) and [`volk`](https://archlinux.org/packages/extra/x86_64/volk/) packages.

``` bash
git clone --recurse-submodules https://github.com/bailwillharr/gamecore.git
cd gamecore
cmake --preset=$BUILD_PRESET .
cd out/build/$BUILD_PRESET
cmake --build . --parallel
```
Where `$BUILD_PRESET` is one of `x64-debug-linux`, `x64-release-linux`, or `x64-dist-linux`.

### Windows

Ensure you have Visual Studio 2022 installed with the 'Desktop development with C++' workload with the 'C++ CMake tools for Windows' component.

Select 'File -> Clone Repository...' in Visual Studio and use the URL `https://github.com/bailwillharr/gamecore.git`.
Once opened, select the appropriate build preset and build the project.