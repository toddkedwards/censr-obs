"""Fetch the GPU-OCR dependencies that are too large to commit:

  * PP-OCRv4 ONNX models (detection + recognition) -> data/models/
  * ONNX Runtime (DirectML) C/C++ package          -> third_party/ort-directml/
  * DirectML runtime                               -> third_party/directml/

Run once after cloning:  python tools/fetch_gpu_ocr.py
Requires internet. Uses pip to obtain the PP-OCR models (via rapidocr-onnxruntime,
which bundles tested ONNX models), and NuGet package zips for ORT + DirectML.
"""
import hashlib
import io
import os
import subprocess
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TP = os.path.join(ROOT, "third_party")
MODELS = os.path.join(ROOT, "data", "models")

ORT_VER = "1.20.1"
DML_VER = "1.15.4"

# Pin the model provider and verify the exact model bytes: these files are
# bundled into the code-signed release, so an unpinned/compromised upstream
# would otherwise silently change signed content. Update the version and the
# hashes together when intentionally bumping models.
RAPIDOCR_VER = "1.4.4"
MODEL_SHA256 = {
    "ch_PP-OCRv4_det_infer.onnx":
        "d2a7720d45a54257208b1e13e36a8479894cb74155a5efe29462512d42f49da9",
    "ch_PP-OCRv4_rec_infer.onnx":
        "48fc40f24f6d2a207a2b1091d3437eb3cc3eb6b676dc3ef9c37384005483683b",
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url, dest_zip):
    print(f"  downloading {url}")
    with urllib.request.urlopen(url) as r:
        data = r.read()
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        z.extractall(dest_zip)


def fetch_models():
    os.makedirs(MODELS, exist_ok=True)
    print("Fetching PP-OCRv4 ONNX models via rapidocr-onnxruntime ...")
    subprocess.run([sys.executable, "-m", "pip", "install", "--user", "--quiet",
                    f"rapidocr-onnxruntime=={RAPIDOCR_VER}"], check=True)
    # Make the freshly --user-installed package importable in this process (its
    # site dir may not be on sys.path yet, e.g. on CI).
    import importlib
    import site
    site.addsitedir(site.getusersitepackages())
    importlib.invalidate_caches()
    import rapidocr_onnxruntime  # noqa: E402
    src = os.path.join(os.path.dirname(rapidocr_onnxruntime.__file__), "models")
    import shutil
    for name in ("ch_PP-OCRv4_det_infer.onnx", "ch_PP-OCRv4_rec_infer.onnx"):
        dest = os.path.join(MODELS, name)
        shutil.copy2(os.path.join(src, name), dest)
        got = sha256(dest)
        if got != MODEL_SHA256[name]:
            raise SystemExit(
                f"model integrity check failed for {name}:\n"
                f"  expected {MODEL_SHA256[name]}\n  got      {got}")
        print(f"  {name}  (sha256 ok)")


def fetch_ort_gpu():
    """CUDA/TensorRT ONNX Runtime for a -DCENSR_ORT_GPU=ON build. Also requires
    CUDA Toolkit + cuDNN (+ TensorRT) installed on the machine at runtime."""
    print("Fetching ONNX Runtime GPU (CUDA/TensorRT) ...")
    # Native libs (dlls, providers, .lib) + headers.
    download(f"https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.Gpu.Windows/{ORT_VER}",
             os.path.join(TP, "ort-gpu"))
    download(f"https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.Gpu/{ORT_VER}",
             os.path.join(TP, "ort-gpu"))
    print("  Fetched to third_party/ort-gpu. The CUDA/TensorRT build additionally")
    print("  needs CUDA Toolkit + cuDNN (+ TensorRT) installed; then build with")
    print("  -DCENSR_ORT_GPU=ON and deploy onnxruntime.dll + onnxruntime_providers_*.dll.")


def main():
    import sys
    fetch_models()
    print("Fetching ONNX Runtime (DirectML) ...")
    download(f"https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/{ORT_VER}",
             os.path.join(TP, "ort-directml"))
    print("Fetching DirectML runtime ...")
    download(f"https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/{DML_VER}",
             os.path.join(TP, "directml"))
    if "--gpu" in sys.argv:
        fetch_ort_gpu()
    print("Done. Now: cmake --preset windows-x64-local && cmake --build --preset windows-x64-local")


if __name__ == "__main__":
    main()
