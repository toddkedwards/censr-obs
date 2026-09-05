"""Generate a 1080p test fixture for Censr: a mock terminal/editor screen
containing fake secrets (API keys, a password line, an email) and a QR code.
All secrets are fake/example values. Output: tools/fixtures/secrets_1080p.png
"""
import os
import qrcode
from PIL import Image, ImageDraw, ImageFont

W, H = 1920, 1080
OUT = os.path.join(os.path.dirname(__file__), "fixtures", "secrets_1080p.png")

BG = (24, 26, 33)
FG = (220, 223, 228)
DIM = (120, 128, 140)
GREEN = (126, 200, 140)

def load_font(size, bold=False):
    for name in (("consolab.ttf" if bold else "consola.ttf"), "cour.ttf"):
        p = os.path.join(r"C:\Windows\Fonts", name)
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()

img = Image.new("RGB", (W, H), BG)
d = ImageDraw.Draw(img)

mono = load_font(30)
mono_b = load_font(30, bold=True)
small = load_font(22)

# Title bar
d.rectangle([0, 0, W, 44], fill=(34, 37, 46))
d.text((16, 8), "todd@devbox: ~/project  —  bash", font=small, fill=DIM)

# Content lines. Each tuple: (x, y, text, font, color)
lines = [
    (40, 90,  "$ cat .env", mono, GREEN),
    (40, 140, "API_KEY=sk-live-4eC39HqLyjWDarjtT1zdp7dc8fFAKE0example", mono, FG),
    (40, 190, "password = hunter2SuperSecretValue!", mono, FG),
    (40, 240, "SUPPORT_EMAIL=todd@example.com", mono, FG),
    (40, 290, "AWS_ACCESS_KEY_ID=AKIAIOSFODNN7EXAMPLE", mono, FG),
    (40, 340, "GITHUB_TOKEN=ghp_16CharsExampleToken0123456789abWXYZ", mono, FG),
    (40, 410, "$ echo 'scan to pair device:'", mono, GREEN),
]
for (x, y, t, f, c) in lines:
    d.text((x, y), t, font=f, fill=c)

# QR code (fake pairing payload) lower-left
qr = qrcode.QRCode(border=2, box_size=8)
qr.add_data("https://example.com/pair?token=FAKE-CENSR-TEST-42")
qr.make(fit=True)
qr_img = qr.make_image(fill_color="black", back_color="white").convert("RGB")
img.paste(qr_img, (60, 470))

# A caption under the QR
d.text((60, 470 + qr_img.height + 10), "device pairing QR (fake)", font=small, fill=DIM)

# Some innocuous text on the right that must NOT be covered
d.text((1150, 140), "# Notes", font=mono_b, fill=FG)
for i, t in enumerate([
    "Refactor the render loop",
    "Ship the 30fps gate demo",
    "Nothing secret over here",
]):
    d.text((1150, 190 + i * 44), "- " + t, font=mono, fill=DIM)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
img.save(OUT)
print("wrote", OUT, img.size)
