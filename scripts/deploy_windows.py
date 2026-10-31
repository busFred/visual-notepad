#!/usr/bin/env python3
"""Assembles the self-contained Windows application folder (CI).

Run from a shell where the MSVC tools (dumpbin) are on PATH.

  deploy_windows.py --build-dir B --env CONDA_PREFIX --kf6 PREFIX --ksh-src DIR --out DIR
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Provided by Windows itself (or the Universal CRT on Windows 10+).
system_re = re.compile(r"^(api-ms-win-.*|ext-ms-.*)\.dll$", re.I)
# The VC++ runtime is not on every machine: bundle it.
vcrt_re = re.compile(r"^(vcruntime|msvcp|concrt|vcomp|vccorlib)\d.*\.dll$", re.I)


def dependents(binary):
    out = subprocess.run(["dumpbin", "/dependents", binary], capture_output=True, text=True, check=True).stdout
    return re.findall(r"^\s+(\S+\.dll)\s*$", out, re.I | re.M)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--env", required=True)
    ap.add_argument("--kf6", required=True)
    ap.add_argument("--ksh-src", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    env, out = os.path.abspath(a.env), os.path.abspath(a.out)
    build = os.path.abspath(a.build_dir)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    copied = []  # files taken from the conda env

    exe = os.path.join(out, "visnotepad.exe")
    shutil.copy(os.path.join(build, "visnotepad.exe"), exe)

    search = [os.path.join(env, "Library", "bin"), os.path.join(env, "bin"), os.path.join(env, "Scripts"),
              env, os.path.join(a.kf6, "bin")]
    tool = None
    for pat in ("windeployqt6.exe", "windeployqt.exe"):
        for d in search + [os.path.join(env, "Library", "lib", "qt6", "bin")]:
            if os.path.isfile(os.path.join(d, pat)):
                tool = os.path.join(d, pat)
                break
        if tool:
            break
    if not tool:
        sys.exit("windeployqt not found in " + env)

    # KSyntaxHighlighting must be beside the exe before windeployqt scans it.
    for dll in glob.glob(os.path.join(a.kf6, "bin", "KF6SyntaxHighlighting*.dll")):
        shutil.copy(dll, out)

    runenv = dict(os.environ)
    runenv["PATH"] = os.pathsep.join(search + [runenv["PATH"]])
    qtpaths = next(iter(sorted(glob.glob(os.path.join(env, "Library", "**", "qtpaths*.exe"), recursive=True))), None)
    print("windeployqt:", tool, "qtpaths:", qtpaths)
    extra = ["--qtpaths", qtpaths] if qtpaths else []
    subprocess.run([tool, *extra, "--release", "--no-translations", "--no-system-d3d-compiler", "--no-compiler-runtime",
                    "--dir", out, exe], check=True, env=runenv)

    # https pictures in Markdown need a TLS backend plugin (Schannel uses the
    # Windows certificate store); make sure it is there whatever windeployqt decided.
    tls = os.path.join(env, "Library", "lib", "qt6", "plugins", "tls")
    for name in ("qschannelbackend.dll", "qcertonlybackend.dll"):
        if os.path.isfile(os.path.join(tls, name)) and not os.path.isfile(os.path.join(out, "tls", name)):
            os.makedirs(os.path.join(out, "tls"), exist_ok=True)
            shutil.copy(os.path.join(tls, name), os.path.join(out, "tls"))

    # Everything windeployqt copied came from the env; record by name.
    for dirpath, _, files in os.walk(out):
        for fn in files:
            if fn.lower().endswith((".dll",)) and not fn.lower().startswith("kf6"):
                for d in search:
                    cand = os.path.join(d, fn)
                    if os.path.isfile(cand):
                        copied.append(cand)
                        break
    # Qt plugins live under Library/lib/qt6/plugins in the env.
    plugroot = os.path.join(env, "Library", "lib", "qt6", "plugins")
    for dirpath, _, files in os.walk(out):
        rel = os.path.relpath(dirpath, out)
        if rel != "." and os.path.isdir(os.path.join(plugroot, rel)):
            for fn in files:
                copied.append(os.path.join(plugroot, rel, fn))

    # The offscreen platform serves smoke tests and automation.
    off = os.path.join(plugroot, "platforms", "qoffscreen.dll")
    if os.path.isfile(off):
        shutil.copy(off, os.path.join(out, "platforms"))
        copied.append(off)

    # Walk the import tables until nothing is missing.
    redist = os.environ.get("VCToolsRedistDir", "")
    crt_dirs = glob.glob(os.path.join(redist, "x64", "Microsoft.VC*.CRT"))
    sys32 = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32")
    seen = set()
    missing = set()
    while True:
        present = {f.lower() for f in os.listdir(out)}
        todo = []
        for dirpath, _, files in os.walk(out):
            for fn in files:
                if fn.lower().endswith((".dll", ".exe")):
                    p = os.path.join(dirpath, fn)
                    if p not in seen:
                        seen.add(p)
                        todo.append(p)
        if not todo:
            break
        for p in todo:
            for dep in dependents(p):
                dl = dep.lower()
                if dl in present or system_re.match(dep):
                    continue
                src = None
                if vcrt_re.match(dep):
                    for d in search + crt_dirs:
                        if os.path.isfile(os.path.join(d, dep)):
                            src = os.path.join(d, dep)
                            break
                else:
                    for d in search:
                        if os.path.isfile(os.path.join(d, dep)):
                            src = os.path.join(d, dep)
                            break
                if src is None:
                    if os.path.isfile(os.path.join(sys32, dep)):
                        continue  # a Windows system DLL
                    missing.add(dep)
                    continue
                shutil.copy(src, out)
                present.add(dl)
                if src.lower().startswith(env.lower()):
                    copied.append(src)
                print("bundled", dep, "from", src)
    if missing:
        sys.exit("unresolved DLLs: " + ", ".join(sorted(missing)))

    shutil.copytree(os.path.join(build, "microtex", "res"), os.path.join(out, "microtex", "res"))

    listfile = os.path.join(build, "copied-from-env.txt")
    with open(listfile, "w", encoding="utf-8") as f:
        f.write("\n".join(sorted(set(copied))) + "\n")
    deps = os.path.join(build, "_deps")
    subprocess.run([sys.executable, os.path.join(root, "scripts", "collect_licenses.py"),
                    "--out", os.path.join(out, "licenses"), "--env", env, "--libs", listfile,
                    "--ksh-src", a.ksh_src,
                    "--microtex-src", os.path.join(deps, "microtex_src-src"),
                    "--tinyxml2-src", os.path.join(deps, "tinyxml2_src-src")], check=True)


if __name__ == "__main__":
    main()
