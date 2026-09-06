"""Assemble a distributable Censr plugin ZIP.

Runs `cmake --install` into a staging dir (producing the OBS plugin layout
censr/bin/64bit + censr/data, with the ONNX Runtime DLLs and PP-OCR models
bundled) and zips it to dist/censr-<version>-windows-x64.zip.

The ZIP extracts straight into  %ProgramData%\\obs-studio\\plugins\\  .

Usage: python tools/package.py [--config RelWithDebInfo] [--build-dir build_x64]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def cmake_exe():
    for c in ("cmake", r"C:\Program Files\CMake\bin\cmake.exe"):
        if shutil.which(c) or os.path.exists(c):
            return c
    sys.exit("cmake not found")


def build_installer():
    import glob
    cands = (glob.glob(r"C:\Program Files*\Inno Setup*\ISCC.exe") +
             glob.glob(os.path.join(os.environ.get("LOCALAPPDATA", ""),
                                    r"Programs\Inno Setup*\ISCC.exe")))
    iscc = next((c for c in cands if os.path.exists(c)), shutil.which("iscc"))
    if not iscc:
        print("Inno Setup (ISCC) not found; skipping installer.")
        print("  Install with: winget install JRSoftware.InnoSetup")
        return
    print("Building installer with Inno Setup ...")
    subprocess.run([iscc, os.path.join(ROOT, "packaging", "censr.iss")], check=True)


def find_signtool():
    import glob
    cands = sorted(glob.glob(
        r"C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe"),
        reverse=True)
    return cands[0] if cands else shutil.which("signtool")


def sign_config():
    """Signing config from env; None if signing isn't configured. Priority:
      1. Azure Trusted Signing — CENSR_TS_ENDPOINT + CENSR_TS_ACCOUNT +
         CENSR_TS_PROFILE + CENSR_TS_DLIB (path to Azure.CodeSigning.Dlib.dll);
         authenticated via the standard Azure env (AZURE_CLIENT_ID/TENANT_ID/
         CLIENT_SECRET, Azure CLI login, or managed identity).
      2. A .pfx — CENSR_SIGN_PFX (+ CENSR_SIGN_PASSWORD).
      3. A store cert — CENSR_SIGN_THUMBPRINT.
    CENSR_SIGN_TIMESTAMP overrides the timestamp server."""
    ep = os.environ.get("CENSR_TS_ENDPOINT")
    if ep and all(os.environ.get(k) for k in
                  ("CENSR_TS_ACCOUNT", "CENSR_TS_PROFILE", "CENSR_TS_DLIB")):
        return {
            "mode": "ts",
            "endpoint": ep,
            "account": os.environ["CENSR_TS_ACCOUNT"],
            "profile": os.environ["CENSR_TS_PROFILE"],
            "dlib": os.environ["CENSR_TS_DLIB"],
            "timestamp": os.environ.get("CENSR_SIGN_TIMESTAMP",
                                        "http://timestamp.acs.microsoft.com"),
        }
    pfx = os.environ.get("CENSR_SIGN_PFX")
    thumb = os.environ.get("CENSR_SIGN_THUMBPRINT")
    if not pfx and not thumb:
        return None
    return {
        "mode": "cert",
        "pfx": pfx,
        "password": os.environ.get("CENSR_SIGN_PASSWORD"),
        "thumbprint": thumb,
        "timestamp": os.environ.get("CENSR_SIGN_TIMESTAMP",
                                    "http://timestamp.digicert.com"),
    }


def sign_file(path, cfg, signtool):
    if cfg["mode"] == "ts":
        meta = {"Endpoint": cfg["endpoint"],
                "CodeSigningAccountName": cfg["account"],
                "CertificateProfileName": cfg["profile"]}
        mdf = os.path.join(tempfile.gettempdir(), "censr_ts_metadata.json")
        with open(mdf, "w", encoding="utf-8") as f:
            json.dump(meta, f)
        args = [signtool, "sign", "/v", "/fd", "SHA256",
                "/tr", cfg["timestamp"], "/td", "SHA256",
                "/dlib", cfg["dlib"], "/dmdf", mdf, path]
    else:
        args = [signtool, "sign", "/fd", "SHA256",
                "/tr", cfg["timestamp"], "/td", "SHA256"]
        if cfg["pfx"]:
            args += ["/f", cfg["pfx"]]
            if cfg["password"]:
                args += ["/p", cfg["password"]]
        else:
            args += ["/sha1", cfg["thumbprint"]]
        args.append(path)
    subprocess.run(args, check=True)
    print(f"  signed {os.path.basename(path)}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="RelWithDebInfo")
    ap.add_argument("--build-dir", default="build_x64")
    ap.add_argument("--installer", action="store_true",
                    help="also build an Inno Setup installer .exe")
    args = ap.parse_args()

    spec = json.load(open(os.path.join(ROOT, "buildspec.json"), encoding="utf-8"))
    name, version = spec["name"], spec["version"]

    stage = os.path.join(ROOT, "dist", "stage")
    if os.path.exists(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)

    print(f"Installing {name} {version} -> {stage}")
    subprocess.run([cmake_exe(), "--install", os.path.join(ROOT, args.build_dir),
                    "--config", args.config, "--prefix", stage], check=True)

    plugin_root = os.path.join(stage, name)
    if not os.path.isdir(plugin_root):
        sys.exit(f"expected {plugin_root} after install")

    # Verify the essentials are present.
    required = [
        f"{name}/bin/64bit/{name}.dll",
        f"{name}/bin/64bit/onnxruntime.dll",
        f"{name}/bin/64bit/DirectML.dll",
        f"{name}/data/locale/en-US.ini",
        f"{name}/data/models/ch_PP-OCRv4_det_infer.onnx",
        f"{name}/data/models/ch_PP-OCRv4_rec_infer.onnx",
    ]
    missing = [r for r in required if not os.path.exists(os.path.join(stage, r))]
    if missing:
        print("WARNING: missing from package:")
        for m in missing:
            print("  -", m)

    # Code-sign the plugin binary (before it's zipped/installed) if a cert is
    # configured; otherwise leave it unsigned.
    cfg = sign_config()
    signtool = find_signtool() if cfg else None
    if cfg and not signtool:
        print("WARNING: signing configured but signtool.exe not found; skipping")
        cfg = None
    if cfg:
        print(f"Signing plugin binary ({cfg['mode']}) ...")
        sign_file(os.path.join(stage, name, "bin", "64bit", f"{name}.dll"),
                  cfg, signtool)
    else:
        print("(unsigned — set CENSR_TS_* (Trusted Signing) or CENSR_SIGN_PFX "
              "/ CENSR_SIGN_THUMBPRINT to sign)")

    out = os.path.join(ROOT, "dist", f"{name}-{version}-windows-x64.zip")
    total = 0
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for base, _, files in os.walk(plugin_root):
            for f in files:
                full = os.path.join(base, f)
                arc = os.path.relpath(full, stage)  # keep top-level "censr/..."
                z.write(full, arc)
                total += os.path.getsize(full)

    print(f"\nPackaged: {out}")
    print(f"  {os.path.getsize(out)/1e6:.1f} MB zipped, {total/1e6:.1f} MB unpacked")
    print("  Install: extract into %ProgramData%\\obs-studio\\plugins\\")

    if args.installer:
        build_installer()
        if cfg:
            installer = os.path.join(
                ROOT, "dist", f"{name}-{version}-windows-x64-installer.exe")
            if os.path.exists(installer):
                print("Signing installer ...")
                sign_file(installer, cfg, signtool)


if __name__ == "__main__":
    main()
