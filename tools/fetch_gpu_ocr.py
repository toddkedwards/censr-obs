"""Fetch the GPU-OCR dependencies that are too large to commit:

  * PP-OCRv4 ONNX models (detection + recognition) -> data/models/
  * ONNX Runtime (DirectML) C/C++ package          -> third_party/ort-directml/
  * DirectML runtime                               -> third_party/directml/

Run once after cloning:  python tools/fetch_gpu_ocr.py
Requires internet. Uses pip to obtain the PP-OCR models (via rapidocr-onnxruntime,
which bundles tested ONNX models), and NuGet package zips for ORT + DirectML.
"""
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
                    "rapidocr-onnxruntime"], check=True)
    import rapidocr_onnxruntime  # noqa: E402
    src = os.path.join(os.path.dirname(rapidocr_onnxruntime.__file__), "models")
    import shutil
    for name in ("ch_PP-OCRv4_det_infer.onnx", "ch_PP-OCRv4_rec_infer.onnx"):
        shutil.copy2(os.path.join(src, name), os.path.join(MODELS, name))
        print(f"  {name}")


def main():
    fetch_models()
    print("Fetching ONNX Runtime (DirectML) ...")
    download(f"https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/{ORT_VER}",
             os.path.join(TP, "ort-directml"))
    print("Fetching DirectML runtime ...")
    download(f"https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/{DML_VER}",
             os.path.join(TP, "directml"))
    print("Done. Now: cmake --preset windows-x64-local && cmake --build --preset windows-x64-local")


if __name__ == "__main__":
    main()
