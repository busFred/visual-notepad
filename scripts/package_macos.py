#!/usr/bin/env python3
"""Makes the macOS disk image: Visual Notepad.app with Qt and its libraries.

  package_macos.py --build-dir DIR --env CONDA_PREFIX --kf6 KF6_PREFIX
      [--ksh-src DIR] [--out DIR]

The build (cmake --build DIR) must have made visnotepad.app and microtex_res.
Run on macOS (otool, install_name_tool, codesign, iconutil, hdiutil); needs
rsvg-convert (conda-forge librsvg) for the icon.

Every library the app or its Qt plugins load from the conda env or the KDE
prefix is copied into Contents/Frameworks and named @rpath/<name>, so that
nothing refers to the build machine; system libraries are left alone, and
anything else is an error. Qt's plugins go to Contents/PlugIns. The bundle is
signed ad hoc (Apple Silicon runs nothing unsigned, and changing load
commands breaks a signature): it is not notarized, so on first start macOS
asks the user to allow it (System Settings, Privacy & Security, Open Anyway).
"""
import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Qt plugins to bundle, by folder; None for every plugin of the folder.
PLUGINS = {
    # The offscreen one is what the math worker runs on (src/markdown/MathRenderer.h).
    "platforms": ["libqcocoa.dylib", "libqoffscreen.dylib"],
    "styles": None,
    "imageformats": None,
    "iconengines": None,
    "tls": None,
}
SYSTEM = ("/System/", "/usr/lib/")


def run(*args, capture=False):
    if capture:
        return subprocess.run(args, check=True, capture_output=True, text=True).stdout
    subprocess.run(args, check=True)


def is_macho(path):
    if os.path.islink(path) or not os.path.isfile(path):
        return False
    with open(path, "rb") as f:
        magic = f.read(4)
    return magic in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe")


def load_commands(path):
    """The dylibs a Mach-O file loads (without its own id) and its rpaths."""
    out = run("otool", "-l", path, capture=True)
    deps, rpaths, own = [], [], None
    cmd = None
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("cmd "):
            cmd = line.split()[1]
        elif line.startswith("name ") and cmd in ("LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB"):
            deps.append(line[5:].rsplit(" (offset", 1)[0])
        elif line.startswith("name ") and cmd == "LC_ID_DYLIB":
            own = line[5:].rsplit(" (offset", 1)[0]
        elif line.startswith("path ") and cmd == "LC_RPATH":
            rpaths.append(line[5:].rsplit(" (offset", 1)[0])
    return deps, rpaths, own


class Bundler:
    def __init__(self, app, env, kf6):
        self.app = app
        self.frameworks = os.path.join(app, "Contents", "Frameworks")
        self.lib_dirs = [os.path.join(env, "lib"), os.path.join(kf6, "lib")]
        self.prefixes = [os.path.realpath(env) + "/", os.path.realpath(kf6) + "/"]
        self.copied = {}  # Name in Frameworks -> source path.
        self.sources = {}  # Bundled file -> the file it was copied from.

    def resolve(self, dep, source):
        """The file `dep`, as loaded by `source` (the original file), or None
        for a system library."""
        if dep.startswith(SYSTEM):
            return None
        name = os.path.basename(dep)
        if dep.startswith("@rpath/"):
            for d in [os.path.dirname(source)] + self.lib_dirs:
                if os.path.exists(os.path.join(d, name)):
                    return os.path.join(d, name)
        elif dep.startswith(("@loader_path/", "@executable_path/")):
            path = os.path.normpath(os.path.join(os.path.dirname(source), dep.split("/", 1)[1]))
            if os.path.exists(path):
                return path
        elif os.path.isabs(dep) and os.path.exists(dep):
            if any(os.path.realpath(dep).startswith(p) for p in self.prefixes):
                return dep
        sys.exit(f"{source}: cannot bundle {dep} (not in the conda env or the KDE prefix)")

    def bundle(self, path, source, rpath):
        """Points `path` (a copy of `source` in the bundle) at the bundled
        libraries, copying them in as needed; `rpath` is how it finds
        Contents/Frameworks."""
        deps, rpaths, own = load_commands(path)
        for dep in deps:
            found = self.resolve(dep, source)
            if found is None:
                continue
            name = os.path.basename(dep)
            if name not in self.copied:
                target = os.path.join(self.frameworks, name)
                self.copied[name] = os.path.realpath(found)
                shutil.copyfile(os.path.realpath(found), target)
                os.chmod(target, 0o755)
                self.sources[target] = os.path.realpath(found)
                run("install_name_tool", "-id", "@rpath/" + name, target)
                self.bundle(target, os.path.realpath(found), "@loader_path/")
            if dep != "@rpath/" + name:
                run("install_name_tool", "-change", dep, "@rpath/" + name, path)
        # Only the bundle's own rpath: none to the build machine.
        for r in rpaths:
            if r != rpath:
                run("install_name_tool", "-delete_rpath", r, path)
        if rpath not in rpaths:
            run("install_name_tool", "-add_rpath", rpath, path)


def make_icon(resources):
    svg = os.path.join(root, "resources", "app", "visnotepad.svg")
    with tempfile.TemporaryDirectory() as tmp:
        iconset = os.path.join(tmp, "visnotepad.iconset")
        os.mkdir(iconset)
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                px = size * scale
                suffix = "" if scale == 1 else "@2x"
                run("rsvg-convert", "-w", str(px), "-h", str(px), "-o",
                    os.path.join(iconset, f"icon_{size}x{size}{suffix}.png"), svg)
        run("iconutil", "-c", "icns", "-o", os.path.join(resources, "visnotepad.icns"), iconset)


def sign(app):
    # Inside out: libraries and plugins, then the bundle (its executable and
    # the seal over its resources).
    for dirpath, _, files in os.walk(app):
        for f in files:
            p = os.path.join(dirpath, f)
            if p.endswith(".dylib") and is_macho(p):
                run("codesign", "--force", "--sign", "-", "--timestamp=none", p)
    run("codesign", "--force", "--sign", "-", "--timestamp=none", app)
    run("codesign", "--verify", "--strict", "--verbose=2", app)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--env", required=True)
    ap.add_argument("--kf6", required=True)
    ap.add_argument("--ksh-src")
    ap.add_argument("--out", default=os.path.join(root, "dist"))
    args = ap.parse_args()

    with open(os.path.join(root, "CMakeLists.txt")) as f:
        version = re.search(r"^project\(visnotepad VERSION ([0-9.]+)", f.read(), re.M).group(1)
    arch = platform.machine()  # arm64 or x86_64
    build = os.path.abspath(args.build_dir)
    stage = os.path.join(root, "build-macos")
    app = os.path.join(stage, "Visual Notepad.app")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    shutil.copytree(os.path.join(build, "visnotepad.app"), app, symlinks=True)
    contents = os.path.join(app, "Contents")
    resources = os.path.join(contents, "Resources")
    plugins = os.path.join(contents, "PlugIns")
    for d in (resources, plugins, os.path.join(contents, "Frameworks")):
        os.makedirs(d, exist_ok=True)

    bundler = Bundler(app, args.env, args.kf6)
    exe = os.path.join(contents, "MacOS", "visnotepad")
    bundler.bundle(exe, os.path.join(build, "visnotepad.app", "Contents", "MacOS", "visnotepad"),
                   "@executable_path/../Frameworks")

    qt_plugins = os.path.join(args.env, "lib", "qt6", "plugins")
    plugin_sources = []
    for folder, names in PLUGINS.items():
        src = os.path.join(qt_plugins, folder)
        if not os.path.isdir(src):
            continue
        os.makedirs(os.path.join(plugins, folder), exist_ok=True)
        for name in sorted(os.listdir(src)):
            if not name.endswith(".dylib") or (names is not None and name not in names):
                continue
            target = os.path.join(plugins, folder, name)
            shutil.copyfile(os.path.join(src, name), target)
            os.chmod(target, 0o755)
            plugin_sources.append(os.path.join(src, name))
            bundler.bundle(target, os.path.join(src, name), "@loader_path/../../Frameworks")

    with open(os.path.join(resources, "qt.conf"), "w") as f:
        f.write("[Paths]\nPlugins = PlugIns\n")
    shutil.copytree(os.path.join(build, "microtex"), os.path.join(resources, "microtex"))
    make_icon(resources)

    # License texts of the app and of every bundled library.
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as listing:
        for path in list(bundler.copied.values()) + plugin_sources:
            listing.write(path + "\n")
    lic = [sys.executable, os.path.join(root, "scripts", "collect_licenses.py"),
           "--out", os.path.join(resources, "licenses"), "--env", os.path.realpath(args.env), "--libs", listing.name,
           "--microtex-src", os.path.join(build, "_deps", "microtex_src-src"),
           "--tinyxml2-src", os.path.join(build, "_deps", "tinyxml2_src-src")]
    if args.ksh_src and os.path.isdir(os.path.join(args.ksh_src, "LICENSES")):
        lic += ["--ksh-src", args.ksh_src]
    run(*lic)
    os.unlink(listing.name)

    sign(app)

    os.makedirs(args.out, exist_ok=True)
    os.symlink("/Applications", os.path.join(stage, "Applications"))
    dmg = os.path.join(args.out, f"visnotepad-{version}-macos-{arch}.dmg")
    if os.path.exists(dmg):
        os.unlink(dmg)
    # hdiutil fails now and then with "Resource busy" on GitHub's macOS
    # machines (something else has the new image open for a moment): try
    # again a few times before giving up.
    for attempt in range(1, 6):
        try:
            run("hdiutil", "create", "-volname", "Visual Notepad", "-srcfolder", stage, "-ov", "-format", "UDZO", dmg)
            break
        except subprocess.CalledProcessError:
            if attempt == 5:
                raise
            print(f"hdiutil failed (attempt {attempt}); trying again", file=sys.stderr)
            time.sleep(10 * attempt)
    print(dmg)


if __name__ == "__main__":
    main()
