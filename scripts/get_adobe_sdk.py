#!/usr/bin/env python3
"""
Puts the Adobe After Effects and Premiere Pro SDKs where the build expects them:
    external/foundation/external/adobe/AfterEffectsSDK   (contains Examples/Headers/AE_GeneralPlug.h)
    external/foundation/external/adobe/premiere          (contains Headers/PrSDKExport.h)

Usage (any mix of zip files, unpacked folders, or https links to a zip):
    python3 scripts/get_adobe_sdk.py ~/Downloads/AfterEffectsSDK.zip ~/Downloads/PremiereSDK.zip
    python3 scripts/get_adobe_sdk.py "https://www.dropbox.com/...adobe-sdks.zip?dl=1"
With no arguments it uses the SDK zips/folders in the local adobe-sdk/ folder (git-ignored: never
commit them), or else the ADOBE_SDK_ZIP_URL environment variable.
Get the SDKs (free Adobe account) from https://developer.adobe.com/console/servicesandapis
"""
import os, shutil, sys, tempfile, urllib.request, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEST = ROOT / "external/foundation/external/adobe"

def find(base: Path, marker: str, levels_up: int):
    for p in base.rglob(Path(marker).name):
        if p.as_posix().endswith(marker) and "__MACOSX" not in p.parts:
            return p.parents[levels_up]
    return None

def main(args):
    if not args:
        local = ROOT / "adobe-sdk"
        items = [str(p) for p in sorted(local.iterdir()) if not p.name.startswith(".") and p.name != "README.md"] if local.is_dir() else []
        if items:
            # prefer the SDK downloads for this platform when both Mac and Windows ones are present
            key = "win" if sys.platform.startswith("win") else "mac"
            mine = [i for i in items if key in Path(i).name.lower()]
            args = mine if len(mine) >= 2 else items
        elif os.environ.get("ADOBE_SDK_ZIP_URL"):
            args = [os.environ["ADOBE_SDK_ZIP_URL"]]
        else:
            sys.exit(__doc__)
    tmp = Path(tempfile.mkdtemp(prefix="adobe-sdk-"))
    roots = []
    for i, a in enumerate(args):
        if a.startswith(("http://", "https://")):
            z = tmp / f"dl{i}.zip"
            print("Downloading SDK archive ...")
            urllib.request.urlretrieve(a, z)
            a = str(z)
        p = Path(a).expanduser()
        if p.is_file() and zipfile.is_zipfile(p):
            out = tmp / f"x{i}"
            zipfile.ZipFile(p).extractall(out)
            # zips inside zips (Adobe ships per-SDK zips)
            for inner in list(out.rglob("*.zip")):
                zipfile.ZipFile(inner).extractall(inner.with_suffix(""))
            roots.append(out)
        elif p.is_dir():
            roots.append(p)
        else:
            sys.exit(f"Not a zip file or folder: {a}")
    ae = pr = None
    for r in roots:
        ae = ae or find(r, "Examples/Headers/AE_GeneralPlug.h", 2)
        pr = pr or find(r, "Headers/PrSDKExport.h", 1)
    if not ae: sys.exit("After Effects SDK not found (looked for Examples/Headers/AE_GeneralPlug.h)")
    if not pr: sys.exit("Premiere Pro SDK not found (looked for Headers/PrSDKExport.h)")
    for src, name in ((ae, "AfterEffectsSDK"), (pr, "premiere")):
        d = DEST / name
        if d.exists(): shutil.rmtree(d)
        shutil.copytree(src, d)
        print(f"OK: {name}  <-  {src}")
    r = DEST / "AfterEffectsSDK/Examples/Resources/AE_General.r"
    if r.exists() and "CodeMacARM64" not in r.read_text(errors="ignore"):
        print("WARNING: This After Effects SDK predates Apple Silicon support - download the current one.")
    shutil.rmtree(tmp, ignore_errors=True)

if __name__ == "__main__":
    main(sys.argv[1:])
