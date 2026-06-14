#!/usr/bin/env python3
"""Generate to-scale printable LED-number labels for the SmartReel front rail.

Geometry is taken 1:1 from the source DXF:
  - rail outline rectangle: x 0..240 mm, y -1.373..41.227 mm  (240 x 42.6 mm)
  - 16 LED windows: 6 mm wide x 5 mm tall, x-centers 8.5 + 15*i mm,
    bottom edge y = -0.373 mm (top y = 4.627 mm)

Each label is a black sticker face with white knock-out windows where the LEDs
shine through and a white knock-out number printed above each window. One PDF is
produced per 16-LED module, numbered 1-16, 17-32, ... up to 256 (16 modules).

Print at 100% / "Actual size" -- do NOT "fit to page" -- so the 1:1 scale holds.
A 50 mm scale-check bar is printed in the margin to verify.
"""
from reportlab.pdfgen import canvas
from reportlab.lib.pagesizes import letter, landscape
from reportlab.lib.units import mm
from reportlab.pdfbase.pdfmetrics import stringWidth

# --- geometry from the DXF (millimetres) ---
RAIL_W = 240.0
RAIL_H = 42.6
OUTLINE_Y0 = -1.373          # bottom of rail in DXF coords
LED_W, LED_H = 6.0, 5.0
LED_BOTTOM = -0.373          # DXF y of LED window bottom
LED_CENTERS_X = [8.5 + 15.0 * i for i in range(16)]

# shift DXF coords so the rail bottom sits at local y = 0
DY = -OUTLINE_Y0             # add this to every DXF y
LED_BOTTOM_L = LED_BOTTOM + DY        # 1.0 mm
LED_TOP_L = LED_BOTTOM_L + LED_H      # 6.0 mm

# --- label styling ---
FONT = "Helvetica-Bold"
FONT_PT = 24
NUM_CENTER_Y = (LED_TOP_L + RAIL_H) / 2.0   # vertical centre of the area above LEDs
CAP_RATIO = 0.717                            # Helvetica cap height / font size

PAGE_W, PAGE_H = landscape(letter)           # 792 x 612 pt
ox = (PAGE_W - RAIL_W * mm) / 2.0            # origin of rail on page (lower-left)
oy = (PAGE_H - RAIL_H * mm) / 2.0


def draw_module(c, start_num, module_idx):
    c.setLineJoin(1)
    c.setLineCap(1)

    # solid black rail face
    c.setFillColorRGB(0, 0, 0)
    c.rect(ox, oy, RAIL_W * mm, RAIL_H * mm, stroke=0, fill=1)

    # subtle dividers between cells so each number reads as "owning" one LED
    c.setStrokeColorRGB(0.32, 0.32, 0.32)
    c.setLineWidth(0.3)
    for i in range(len(LED_CENTERS_X) - 1):
        dx = (LED_CENTERS_X[i] + LED_CENTERS_X[i + 1]) / 2.0
        c.line(ox + dx * mm, oy + (LED_BOTTOM_L - 0.5) * mm,
               ox + dx * mm, oy + (RAIL_H - 2.5) * mm)

    # white knock-out LED windows + numbers
    c.setFillColorRGB(1, 1, 1)
    cap_h = FONT_PT * CAP_RATIO
    base_y = oy + NUM_CENTER_Y * mm - cap_h / 2.0
    for i, cx in enumerate(LED_CENTERS_X):
        n = start_num + i
        # LED window
        c.rect(ox + (cx - LED_W / 2) * mm, oy + LED_BOTTOM_L * mm,
               LED_W * mm, LED_H * mm, stroke=0, fill=1)
        # number, centred over the window; shrink wide (3-digit) numbers so a
        # clear gap to the neighbour is always kept (max 12.5 mm glyph width)
        s = str(n)
        fpt = FONT_PT
        max_w = 12.5 * mm
        w = stringWidth(s, FONT, fpt)
        if w > max_w:
            fpt *= max_w / w
            w = max_w
        c.setFont(FONT, fpt)
        c.drawString(ox + cx * mm - w / 2.0, base_y, s)

    # thin cut guide exactly on the outline so the black-to-white edge is crisp
    c.setStrokeColorRGB(0.6, 0.6, 0.6)
    c.setLineWidth(0.25)
    c.rect(ox, oy, RAIL_W * mm, RAIL_H * mm, stroke=1, fill=0)
    # corner crop ticks
    t = 4 * mm
    for px in (ox, ox + RAIL_W * mm):
        for py in (oy, oy + RAIL_H * mm):
            sx = -1 if px == ox else 1
            sy = -1 if py == oy else 1
            c.line(px, py, px + sx * t, py)
            c.line(px, py, px, py + sy * t)

    # caption + scale-check bar in the bottom margin
    c.setFillColorRGB(0, 0, 0)
    c.setFont("Helvetica", 9)
    cap = "SmartReel front-rail label  -  Module %d  -  LEDs %d-%d  -  print at 100%% (actual size)" % (
        module_idx, start_num, start_num + 15)
    c.drawString(ox, oy - 16 * mm, cap)

    sb_y = oy - 24 * mm
    c.setStrokeColorRGB(0, 0, 0)
    c.setLineWidth(0.5)
    c.line(ox, sb_y, ox + 50 * mm, sb_y)
    for xx in (0, 50):
        c.line(ox + xx * mm, sb_y - 1.5 * mm, ox + xx * mm, sb_y + 1.5 * mm)
    c.setFont("Helvetica", 8)
    c.drawString(ox + 52 * mm, sb_y - 1 * mm, "50 mm scale check")


def main():
    import sys
    modules = 16  # 16 modules x 16 LEDs = 256
    # individual PDFs
    for m in range(modules):
        start = m * 16 + 1
        fn = "label_module_%02d_%03d-%03d.pdf" % (m + 1, start, start + 15)
        c = canvas.Canvas(fn, pagesize=(PAGE_W, PAGE_H))
        draw_module(c, start, m + 1)
        c.showPage()
        c.save()
        print("wrote", fn)
    # combined PDF (all 16 modules, one per page)
    c = canvas.Canvas("labels_all_1-256.pdf", pagesize=(PAGE_W, PAGE_H))
    for m in range(modules):
        draw_module(c, m * 16 + 1, m + 1)
        c.showPage()
    c.save()
    print("wrote labels_all_1-256.pdf")


if __name__ == "__main__":
    main()
