#!/usr/bin/env python3
"""Usage: art.py OUTDIR

Draws the Bedrock artwork into OUTDIR:
  background.png   640x480 BIOS menu background (SYSLINUX vesamenu)
  logo.png         UEFI menu logo, transparent (GRUB theme)
  select_[wce].png UEFI menu selection bar (GRUB theme)
  splash.ppm       boot splash for busybox fbsplash
  splash2x.ppm     the same at twice the size, for high-resolution screens
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

FONTS = "/usr/share/fonts/truetype/dejavu/"
WHITE = (240, 240, 240)
GREY = (138, 143, 152)
ACCENT = (232, 163, 61)
BAR = (38, 42, 49)


def logo(s, bg):
    """Wordmark, accent rule and tagline in a 480x160 box, times s."""
    im = Image.new("RGBA", (480 * s, 160 * s), bg)
    d = ImageDraw.Draw(im)
    big = ImageFont.truetype(FONTS + "DejaVuSans-Bold.ttf", 56 * s)
    small = ImageFont.truetype(FONTS + "DejaVuSans.ttf", 16 * s)
    word, track = "BEDROCK", 12 * s
    widths = [d.textlength(c, font=big) for c in word]
    x = (im.width - sum(widths) - track * (len(word) - 1)) / 2
    for c, w in zip(word, widths):
        d.text((x, 80 * s), c, font=big, fill=WHITE, anchor="ls")
        x += w + track
    d.rectangle((im.width // 2 - 20 * s, 98 * s, im.width // 2 + 20 * s - 1, 100 * s), fill=ACCENT)
    d.text((im.width / 2, 134 * s), "built by erdem6.com", font=small, fill=GREY, anchor="ms")
    return im


out = sys.argv[1]
os.makedirs(out, exist_ok=True)
bg = Image.new("RGB", (640, 480), (0, 0, 0))
bg.paste(logo(1, (0, 0, 0, 255)), (80, 96))
bg.save(os.path.join(out, "background.png"))
logo(1, (0, 0, 0, 0)).save(os.path.join(out, "logo.png"))
for side in "wce":  # selection bar; the side pieces pad the text
    Image.new("RGB", (8 if side == "c" else 12, 8), BAR).save(os.path.join(out, f"select_{side}.png"))
logo(1, (0, 0, 0, 255)).convert("RGB").save(os.path.join(out, "splash.ppm"))
logo(2, (0, 0, 0, 255)).convert("RGB").save(os.path.join(out, "splash2x.ppm"))
