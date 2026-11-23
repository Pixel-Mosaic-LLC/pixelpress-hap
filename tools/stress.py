#!/usr/bin/env python3
"""
Encoder stress test run by the build scripts before packaging (on the real target CPU).
Generates photos (scikit-image samples, if installed) and synthetic worst cases at 1080p and 4K,
and runs every frame through every one of the plugin's texture encoders (Hap R, Hap, Hap Alpha,
Hap Q, alpha planes) at every quality setting with the hapr_selftest harness
(encode -> Hap round trip -> texture decode). Any encoder assertion, crash or
round-trip mismatch fails the build.

usage: stress.py path/to/hapr_selftest [workdir]
"""
import os, subprocess, sys, tempfile
import numpy as np

exe = sys.argv[1]
work = sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp(prefix="pphap-stress-")
os.makedirs(work, exist_ok=True)
rng = np.random.default_rng(7)

def frames(w, h):
    y, x = np.mgrid[0:h, 0:w]
    opaque = np.full((h, w), 255, np.uint8)
    yield "noise", rng.integers(0, 256, (h, w, 3)), opaque
    yield "noise_alpha", rng.integers(0, 256, (h, w, 3)), rng.integers(0, 256, (h, w)).astype(np.uint8)
    yield "gradient", np.dstack([x * 255 // w, y * 255 // h, (x + y) * 255 // (w + h)]), opaque
    yield "checker", ((x // 3 + y // 3) % 2 * 255)[..., None].repeat(3, 2), opaque
    yield "black", np.zeros((h, w, 3)), opaque
    yield "white", np.full((h, w, 3), 255), opaque
    yield "soft_alpha", np.dstack([x * 255 // w, 255 - y * 255 // h, np.full((h, w), 128)]), ((x ^ y) & 255).astype(np.uint8)
    try:
        from skimage import data, transform
        for n in ["astronaut", "coffee", "chelsea", "rocket", "hubble_deep_field", "immunohistochemistry"]:
            im = transform.resize(getattr(data, n)(), (h, w), order=1)
            yield n, np.clip(im, 0, 1) * 255, opaque
    except Exception as e:
        print(f"(scikit-image photos skipped: {e})")

failures = 0
for (w, h) in [(1920, 1080), (4096, 2160), (1918, 1078)]:
    for name, rgb, a in frames(w, h):
        path = os.path.join(work, f"{name}_{w}x{h}.rgba")
        np.dstack([rgb, a]).astype(np.uint8).tofile(path)
        for fmt in ("hap_r", "hap", "hap_alpha", "hap_q", "alpha"):
          for q in (0, 1, 2):
            r = subprocess.run([exe, path, str(w), str(h), str(q), "8", os.path.join(work, "out.hap"), fmt],
                               capture_output=True, text=True)
            ok = r.returncode == 0 and "roundtrip OK" in r.stdout
            print(("  ok   " if ok else "  FAIL ") + (r.stdout.strip() or f"{fmt} {name} {w}x{h} q={q}"))
            if not ok:
                failures += 1
                print("       exit", r.returncode, r.stderr.strip()[:500])
        os.remove(path)
print(f"stress test: {'PASSED' if not failures else f'{failures} FAILURES'}")
sys.exit(1 if failures else 0)
