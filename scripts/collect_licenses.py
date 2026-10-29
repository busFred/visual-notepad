#!/usr/bin/env python3
"""Collects the license files for a package into one folder.

Copies the project's LICENSE and THIRD-PARTY-NOTICES.md, the license files of
KSyntaxHighlighting, MicroTeX and tinyxml2 from their source trees, and for
every bundled conda library the license files of the conda package that
provides it.

  collect_licenses.py --out DIR --env CONDA_PREFIX --libs LISTFILE
      [--ksh-src DIR] [--microtex-src DIR] [--tinyxml2-src DIR] [--pkgs-dir DIR]

LISTFILE holds one absolute path per line: the files copied out of the env.
"""
import argparse
import glob
import json
import os
import shutil
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def norm(path):
    return path.replace("\\", "/").lower()


def copy_if_exists(src, dst):
    if os.path.isfile(src):
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        return True
    return False


def copy_tree_files(srcdir, dstdir):
    n = 0
    for p in sorted(glob.glob(os.path.join(srcdir, "*"))):
        if os.path.isfile(p):
            copy_if_exists(p, os.path.join(dstdir, os.path.basename(p)))
            n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--env", required=True)
    ap.add_argument("--libs", required=True)
    ap.add_argument("--ksh-src")
    ap.add_argument("--microtex-src")
    ap.add_argument("--tinyxml2-src")
    ap.add_argument("--pkgs-dir")
    a = ap.parse_args()

    out = a.out
    os.makedirs(out, exist_ok=True)
    for name in ("LICENSE", "THIRD-PARTY-NOTICES.md"):
        if not copy_if_exists(os.path.join(root, name), os.path.join(out, name)):
            sys.exit("missing " + name)

    if a.ksh_src:
        if not copy_tree_files(os.path.join(a.ksh_src, "LICENSES"), os.path.join(out, "KSyntaxHighlighting")):
            sys.exit("no KSyntaxHighlighting LICENSES in " + a.ksh_src)
    if a.microtex_src:
        if not copy_if_exists(os.path.join(a.microtex_src, "LICENSE"), os.path.join(out, "MicroTeX", "LICENSE")):
            sys.exit("no MicroTeX LICENSE in " + a.microtex_src)
    if a.tinyxml2_src:
        if not copy_if_exists(os.path.join(a.tinyxml2_src, "LICENSE.txt"), os.path.join(out, "tinyxml2", "LICENSE.txt")):
            sys.exit("no tinyxml2 LICENSE.txt in " + a.tinyxml2_src)

    env = os.path.abspath(a.env)
    with open(a.libs, encoding="utf-8") as f:
        wanted = [os.path.abspath(l.strip()) for l in f if l.strip()]

    # file (relative to env, lower case) -> conda-meta record
    owner = {}
    for meta in glob.glob(os.path.join(env, "conda-meta", "*.json")):
        with open(meta, encoding="utf-8") as f:
            rec = json.load(f)
        rec["_stem"] = os.path.basename(meta)[:-5]
        for rel in rec.get("files", []):
            owner[norm(rel)] = rec

    pkgs_dirs = []
    if a.pkgs_dir:
        pkgs_dirs.append(a.pkgs_dir)
    pkgs_dirs.append(os.path.join(os.path.dirname(os.path.dirname(env)), "pkgs"))

    envn = norm(env).rstrip("/") + "/"
    packages = {}
    unmapped = []
    for p in wanted:
        pn = norm(p)
        if not pn.startswith(envn):
            continue  # not from the env (e.g. our own KSyntaxHighlighting)
        rec = owner.get(pn[len(envn):])
        if rec is None:
            unmapped.append(p)
        else:
            packages[rec["_stem"]] = rec

    count = 0
    for stem, rec in sorted(packages.items()):
        cands = []
        if rec.get("extracted_package_dir"):
            cands.append(rec["extracted_package_dir"])
        cands += [os.path.join(d, stem) for d in pkgs_dirs]
        if not any(os.path.isdir(c) for c in cands):
            sys.exit(f"package directory of {stem} not found (looked in {cands}); "
                     "the conda package cache is needed for the license files")
        lic = next((os.path.join(c, "info", "licenses") for c in cands
                    if os.path.isdir(os.path.join(c, "info", "licenses"))), None)
        dst = os.path.join(out, "conda", rec["name"])
        if lic is None:
            # The package ships no license text; record the declared license.
            print("note: no license files in", stem, file=sys.stderr)
            os.makedirs(dst, exist_ok=True)
            with open(os.path.join(dst, "LICENSE-ID.txt"), "w", encoding="utf-8") as f:
                f.write(f"{rec['name']} {rec.get('version', '')}: {rec.get('license', 'unknown')}\n"
                        "The conda package contains no license text.\n")
            continue
        for dirpath, _, files in os.walk(lic):
            for fn in files:
                src = os.path.join(dirpath, fn)
                copy_if_exists(src, os.path.join(dst, os.path.relpath(src, lic)))
                count += 1
    for p in unmapped:
        print("warning: no conda package owns", p, file=sys.stderr)
    print(f"licenses: {len(packages)} conda packages, {count} files -> {out}")


if __name__ == "__main__":
    main()
