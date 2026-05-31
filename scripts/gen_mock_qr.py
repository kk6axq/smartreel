#!/usr/bin/env python3
"""Generate the mock parts catalog + scannable QR codes for the HMI Load flow.

Outputs (under <repo>/sd-assets/):
  parts.json            -> copy to the SD card root as /sdcard/parts.json
  qr/MOCK-PART-#####.png -> one QR per part (print these and scan them)
  qr/contact-sheet.png   -> all codes on one labelled sheet for easy printing

The QR payload is exactly the label "MOCK-PART-#####"; the firmware looks
that string up in parts.json to resolve the part. Run with the scripts venv:

    scripts/.venv/bin/python scripts/gen_mock_qr.py
"""
import json
import os

import qrcode
from PIL import Image, ImageDraw, ImageFont

# qr label -> part record. IDs/names mirror the built-in kCatalog in
# esp32-hmi/src/ui/app_state.cpp so scanned parts line up with rack
# contents (and so the pick flow can find them).
PARTS = [
    ("MOCK-PART-00001", "R-10K-0805",  "RES 10kohm 1% 0805",    "0805",   "Yageo"),
    ("MOCK-PART-00002", "C-100N-0603", "CAP 100nF X7R 0603",    "0603",   "Murata"),
    ("MOCK-PART-00003", "IC-LM358",    "OPA LM358 SOIC-8",      "SOIC8",  "TI"),
    ("MOCK-PART-00004", "D-LED-G-0603","LED green 0603",        "0603",   "Lite-On"),
    ("MOCK-PART-00005", "IC-ESP32S3",  "MCU ESP32-S3-WROOM",    "Module", "Espressif"),
    ("MOCK-PART-00006", "Q-2N7002",    "MOS 2N7002 SOT-23",     "SOT23",  "ON Semi"),
    ("MOCK-PART-00007", "C-10U-0805",  "CAP 10uF X5R 0805",     "0805",   "Samsung"),
    ("MOCK-PART-00008", "CONN-USBC",   "CONN USB-C receptacle", "SMD",    "Molex"),
]

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(REPO, "sd-assets")
QR_DIR = os.path.join(OUT_DIR, "qr")
QR_PX = 480  # rendered module-snapped size per code


def _font(size):
    for path in (
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ):
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def write_parts_json():
    doc = {
        "version": 1,
        "parts": [
            {"qr": qr, "id": pid, "name": name, "pkg": pkg, "mfg": mfg}
            for (qr, pid, name, pkg, mfg) in PARTS
        ],
    }
    path = os.path.join(OUT_DIR, "parts.json")
    with open(path, "w") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    print(f"  parts.json   -> {path}  ({len(PARTS)} parts)")


def make_qr(label):
    qr = qrcode.QRCode(
        error_correction=qrcode.constants.ERROR_CORRECT_M,
        box_size=10,
        border=4,
    )
    qr.add_data(label)
    qr.make(fit=True)
    img = qr.make_image(fill_color="black", back_color="white").convert("RGB")
    return img.resize((QR_PX, QR_PX), Image.NEAREST)


def write_individual():
    for (qr, pid, name, *_rest) in PARTS:
        img = make_qr(qr)
        # caption strip under the code
        cap_h = 70
        canvas = Image.new("RGB", (QR_PX, QR_PX + cap_h), "white")
        canvas.paste(img, (0, 0))
        d = ImageDraw.Draw(canvas)
        d.text((QR_PX // 2, QR_PX + 14), qr, fill="black",
               font=_font(28), anchor="mm")
        d.text((QR_PX // 2, QR_PX + 48), f"{pid}  -  {name}", fill="#555555",
               font=_font(18), anchor="mm")
        path = os.path.join(QR_DIR, f"{qr}.png")
        canvas.save(path)
    print(f"  qr/*.png     -> {QR_DIR}  ({len(PARTS)} codes)")


def write_contact_sheet():
    cols = 4
    cell = 300
    pad = 24
    cap = 56
    rows = (len(PARTS) + cols - 1) // cols
    title_h = 70
    W = cols * cell + (cols + 1) * pad
    H = title_h + rows * (cell + cap) + (rows + 1) * pad
    sheet = Image.new("RGB", (W, H), "white")
    d = ImageDraw.Draw(sheet)
    d.text((pad, 24), "SmartReel mock parts - scan to load", fill="black",
           font=_font(34))
    for i, (qr, pid, name, *_rest) in enumerate(PARTS):
        r, c = divmod(i, cols)
        x = pad + c * (cell + pad)
        y = title_h + pad + r * (cell + cap + pad)
        sheet.paste(make_qr(qr).resize((cell, cell), Image.NEAREST), (x, y))
        d.text((x + cell // 2, y + cell + 14), qr, fill="black",
               font=_font(20), anchor="mm")
        d.text((x + cell // 2, y + cell + 38), pid, fill="#555555",
               font=_font(16), anchor="mm")
    path = os.path.join(QR_DIR, "contact-sheet.png")
    sheet.save(path)
    print(f"  contact-sheet -> {path}")


def main():
    os.makedirs(QR_DIR, exist_ok=True)
    print("Generating mock QR assets:")
    write_parts_json()
    write_individual()
    write_contact_sheet()
    print("done. Copy sd-assets/parts.json to the SD card root (/sdcard/parts.json).")


if __name__ == "__main__":
    main()
