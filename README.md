# Visual Notepad

A native, lightweight Notepad replacement with Markdown editing and preview,
for Windows and Linux. Built with Qt 6 and a small Rust core. See
[docs/design.md](docs/design.md).

## Building on Linux

The toolchain lives in the ignored `.toolchain/` directory, so nothing is
installed system-wide.

1. Create the conda environment (any conda; base is not touched):

   ```sh
   conda create -p .toolchain/env --override-channels -c conda-forge \
       "qt6-main>=6.10" rust cmake ninja cxx-compiler c-compiler \
       libgl-devel libegl-devel pkg-config git perl
   ```

2. Build KDE's Extra CMake Modules and KSyntaxHighlighting into
   `.toolchain/kf6`:

   ```sh
   source scripts/dev-env.sh
   mkdir -p .toolchain/src && cd .toolchain/src
   git clone --depth 1 -b v6.31.0 https://invent.kde.org/frameworks/extra-cmake-modules.git
   git clone --depth 1 -b v6.31.0 https://invent.kde.org/frameworks/syntax-highlighting.git
   cmake -S extra-cmake-modules -B build-ecm -G Ninja -DCMAKE_INSTALL_PREFIX="$PWD/../kf6" -DBUILD_TESTING=OFF
   cmake --build build-ecm && cmake --install build-ecm
   cmake -S syntax-highlighting -B build-ksh -G Ninja -DCMAKE_BUILD_TYPE=Release \
       -DCMAKE_INSTALL_PREFIX="$PWD/../kf6" -DBUILD_TESTING=OFF
   cmake --build build-ksh && cmake --install build-ksh
   cd ../..
   ```

3. Build, test and run:

   ```sh
   source scripts/dev-env.sh
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
   cmake --build build
   ctest --test-dir build --output-on-failure
   build/visnotepad README.md
   ```

Set `VN_SCREENSHOT=/path/shot.png` when running the tests to save a
rendering of the main window.
