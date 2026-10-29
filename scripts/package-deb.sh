#!/usr/bin/env bash
# Builds a self-contained .deb from a Release build made with the project
# toolchain (scripts/dev-env.sh) or an activated conda environment (CI). Qt
# and the other libraries from the conda environment are bundled under
# /opt/visnotepad; glibc and the OpenGL driver libraries come from the system.
#
#   scripts/package-deb.sh            # builds build-rel, writes dist/*.deb
#
# Environment variables (all optional; defaults match the local toolchain):
#   VN_ENV_PREFIX     conda env with Qt (default .toolchain/env, else $CONDA_PREFIX)
#   VN_KF6_PREFIX     prefix holding KSyntaxHighlighting (default .toolchain/kf6)
#   VN_KSH_SRC        KSyntaxHighlighting source tree, for its license files
#   VN_BUILD_DIR      CMake build directory (default build-rel)
#   VN_BUILD_TESTING  ON/OFF for that build directory (default OFF)
#   VN_NO_DEV_ENV=1   do not source scripts/dev-env.sh; use the current environment
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
if [ -z "${VN_NO_DEV_ENV:-}" ] && [ -z "${VN_ENV_PREFIX:-}" ] && [ -d "$root/.toolchain/env" ]; then
    # shellcheck disable=SC1091
    source scripts/dev-env.sh
fi
env_prefix="${VN_ENV_PREFIX:-${CONDA_PREFIX:-$root/.toolchain/env}}"
kf6_prefix="${VN_KF6_PREFIX:-$root/.toolchain/kf6}"
ksh_src="${VN_KSH_SRC:-$root/.toolchain/src/syntax-highlighting}"
export CONDA_PREFIX="$env_prefix"

version="$(sed -n 's/^project(visnotepad VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
arch="$(dpkg --print-architecture)"
build="${VN_BUILD_DIR:-$root/build-rel}"
stage="$root/build-deb/visnotepad_${version}_${arch}"
opt="$stage/opt/visnotepad"

cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING="${VN_BUILD_TESTING:-OFF}" >/dev/null
cmake --build "$build" --target visnotepad microtex_res

rm -rf "$stage"
mkdir -p "$opt/bin" "$opt/lib" "$opt/plugins" "$opt/share/visnotepad" \
    "$stage/usr/bin" "$stage/usr/share/applications" "$stage/usr/share/icons/hicolor/scalable/apps" \
    "$stage/DEBIAN"

install -m 755 "$build/visnotepad" "$opt/bin/visnotepad"
cp -r "$build/microtex" "$opt/share/visnotepad/microtex"

# Qt plugins the app needs: X11 and Wayland, the XDG portal (native file
# dialogs), input methods, images, icons.
qtplugins="$env_prefix/lib/qt6/plugins"
for dir in platforms platformthemes platforminputcontexts imageformats iconengines xcbglintegrations \
    wayland-shell-integration wayland-decoration-client wayland-graphics-integration-client; do
    [ -d "$qtplugins/$dir" ] || continue
    mkdir -p "$opt/plugins/$dir"
    cp "$qtplugins/$dir"/*.so "$opt/plugins/$dir/"
done
# Platforms other than X11 and Wayland would pull in extra libraries.
find "$opt/plugins/platforms" -name '*.so' ! -name 'libqxcb.so' ! -name 'libqwayland*.so' -delete
cat >"$opt/bin/qt.conf" <<'EOF'
[Paths]
Prefix = ..
Plugins = plugins
EOF

# Copy every shared library that resolves into the toolchain, recursively.
# glibc and the GL driver stack stay with the system: GL must match the GPU
# driver installed there.
system_only='^(ld-linux.*|libc|libm|libdl|libpthread|librt|libresolv|libutil|libGL|libEGL|libGLX|libGLdispatch|libOpenGL|libgbm|libdrm)\.so'
queue=("$opt/bin/visnotepad")
while IFS= read -r -d '' plugin; do queue+=("$plugin"); done < <(find "$opt/plugins" -name '*.so' -print0)
declare -A seen
copied="$root/build-deb/copied-from-env.txt"
: >"$copied"
while [ "${#queue[@]}" -gt 0 ]; do
    elf="${queue[0]}"
    queue=("${queue[@]:1}")
    while read -r name path; do
        [ -n "$path" ] || continue
        [[ "$name" =~ $system_only ]] && continue
        case "$path" in "$env_prefix"/*|"$kf6_prefix"/*) ;; *) continue ;; esac
        [ -n "${seen[$name]:-}" ] && continue
        seen[$name]=1
        echo "$path" >>"$copied"
        cp -L "$path" "$opt/lib/$name"
        chmod 644 "$opt/lib/$name"
        queue+=("$opt/lib/$name")
    done < <(LD_LIBRARY_PATH="$env_prefix/lib:$kf6_prefix/lib" ldd "$elf" 2>/dev/null | awk '/=>/ {print $1, $3}')
done

# License texts of the app and of every bundled library.
find "$opt/plugins" -name '*.so' | sed "s#^$opt/plugins#$qtplugins#" >>"$copied"
licargs=(--out "$opt/licenses" --env "$env_prefix" --libs "$copied"
    --microtex-src "$build/_deps/microtex_src-src" --tinyxml2-src "$build/_deps/tinyxml2_src-src")
[ -d "$ksh_src/LICENSES" ] && licargs+=(--ksh-src "$ksh_src")
python3 "$root/scripts/collect_licenses.py" "${licargs[@]}"

# Relative library paths so nothing depends on the build machine.
patchelf --set-rpath '$ORIGIN/../lib' "$opt/bin/visnotepad"
for lib in "$opt"/lib/*.so*; do patchelf --set-rpath '$ORIGIN' "$lib"; done
find "$opt/plugins" -name '*.so' -exec patchelf --set-rpath '$ORIGIN/../../lib' {} \;

# The bundled fontconfig, xkbcommon and libX11 look for their data under the
# conda prefix; point them at the system's.
cat >"$stage/usr/bin/visnotepad" <<'EOF'
#!/bin/sh
[ -z "${FONTCONFIG_FILE:-}" ] && [ -f /etc/fonts/fonts.conf ] && export FONTCONFIG_FILE=/etc/fonts/fonts.conf
[ -z "${XKB_CONFIG_ROOT:-}" ] && [ -d /usr/share/X11/xkb ] && export XKB_CONFIG_ROOT=/usr/share/X11/xkb
[ -z "${XLOCALEDIR:-}" ] && [ -d /usr/share/X11/locale ] && export XLOCALEDIR=/usr/share/X11/locale
exec /opt/visnotepad/bin/visnotepad "$@"
EOF
chmod 755 "$stage/usr/bin/visnotepad"

install -m 644 resources/app/visnotepad.svg "$stage/usr/share/icons/hicolor/scalable/apps/visnotepad.svg"
cat >"$stage/usr/share/applications/visnotepad.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Visual Notepad
GenericName=Text Editor
Comment=Edit text and Markdown files
Exec=visnotepad %F
Icon=visnotepad
Terminal=false
StartupWMClass=visnotepad
Categories=Utility;TextEditor;
MimeType=text/plain;text/markdown;text/x-markdown;
Keywords=Text;Editor;Markdown;Notepad;
EOF

installed_kb="$(du -sk "$stage" --exclude=DEBIAN | cut -f1)"
cat >"$stage/DEBIAN/control" <<EOF
Package: visnotepad
Version: $version
Architecture: $arch
Maintainer: Hung-Tien Huang <32878707+busFred@users.noreply.github.com>
Installed-Size: $installed_kb
Depends: libc6 (>= 2.28), libgl1, libegl1, fontconfig-config, xkb-data
Section: editors
Priority: optional
Description: Lightweight Notepad replacement with a visual Markdown editor
 Visual Notepad edits plain text with syntax highlighting and multiple
 cursors, and Markdown with a hybrid visual editor, live math and
 Mermaid diagrams. Qt and its dependencies are bundled under
 /opt/visnotepad.
EOF

mkdir -p "$root/dist"
fakeroot dpkg-deb --build --root-owner-group -Zxz "$stage" "$root/dist/visnotepad_${version}_${arch}.deb" >/dev/null
echo "$root/dist/visnotepad_${version}_${arch}.deb"
