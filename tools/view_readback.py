"""Convert the filter's raw readback dump (tools/out/readback.bin) to PNG.
Header: uint32 width, uint32 height (LE), then width*height*4 RGBA bytes.
"""
import os
import struct
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(HERE, "out", "readback.bin")
PNG = os.path.join(HERE, "out", "readback.png")

with open(BIN, "rb") as f:
    w, h = struct.unpack("<II", f.read(8))
    data = f.read(w * h * 4)

img = Image.frombytes("RGBA", (w, h), data)
img.convert("RGB").save(PNG)
print(f"readback {w}x{h} -> {PNG}")
