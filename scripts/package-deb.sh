#!/usr/bin/env bash
# Builds a self-contained .deb from a Release build made with the project
# toolchain (scripts/dev-env.sh). Qt and the other libraries from the conda
# environment are bundled under /opt/visnotepad; glibc and the OpenGL
# driver libraries come from the system.
#
#   scripts/package-deb.sh            # builds build-rel, writes dist/*.deb
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
# shellcheck disable=SC1091
source scripts/dev-env.sh

version="$(sed -n 's/^project(visnotepad VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
arch="$(dpkg --print-architecture)"
build="$root/build-rel"
stage="$root/build-deb/visnotepad_${version}_${arch}"
opt="$stage/opt/visnotepad"
toolchain="$root/.toolchain"

cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF >/dev/null
cmake --build "$build" --target visnotepad microtex_res

rm -rf "$stage"
mkdir -p "$opt/bin" "$opt/lib" "$opt/plugins" "$opt/share/visnotepad" \
    "$stage/usr/bin" "$stage/usr/share/applications" "$stage/usr/share/icons/hicolor/scalable/apps" \
    "$stage/DEBIAN"

install -m 755 "$build/visnotepad" "$opt/bin/visnotepad"
cp -r "$build/microtex" "$opt/share/visnotepad/microtex"

# Qt plugins the app needs: X11 and Wayland, input methods, images, icons.
qtplugins="$CONDA_PREFIX/lib/qt6/plugins"
for dir in platforms platforminputcontexts imageformats iconengines xcbglintegrations \
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
while [ "${#queue[@]}" -gt 0 ]; do
    elf="${queue[0]}"
    queue=("${queue[@]:1}")
    while read -r name path; do
        [ -n "$path" ] || continue
        [[ "$name" =~ $system_only ]] && continue
        case "$path" in "$toolchain"/*) ;; *) continue ;; esac
        [ -n "${seen[$name]:-}" ] && continue
        seen[$name]=1
        cp -L "$path" "$opt/lib/$name"
        chmod 644 "$opt/lib/$name"
        queue+=("$opt/lib/$name")
    done < <(LD_LIBRARY_PATH="$CONDA_PREFIX/lib:$toolchain/kf6/lib" ldd "$elf" 2>/dev/null | awk '/=>/ {print $1, $3}')
done

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
