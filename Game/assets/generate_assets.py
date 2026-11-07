#!/usr/bin/env python3
"""Generates the game's original pixel-art PNGs (ships, bullet, explosion, wall tile, star tile).

A development tool only: the game just loads the committed PNGs and needs neither Python nor
Pillow. Run `python3 generate_assets.py` from this folder to regenerate them (deterministic).
"""
import random
from PIL import Image

SHIP_HALF = [  # left half of a 16x16 top-down ship, nose up; mirrored to make the whole sprite
    ".......o", "......oa", "......oa", ".....oac", ".....oaw", "....oaaw", "....oaac", "...oaaab",
    "..oaaaab", ".oaaoabb", "oaaooabb", "oaoo.oab", "oo...oab", "o....oee", ".....oef", "......ef",
]
SHIP_COLORS = {  # main, shade, highlight
    "ship_1": ((60, 140, 255), (30, 70, 170), (150, 210, 255)),   # blue
    "ship_2": ((255, 150, 40), (180, 80, 20), (255, 215, 120)),   # orange
    "ship_3": ((70, 220, 110), (30, 140, 60), (160, 255, 180)),   # green
    "ship_4": ((230, 80, 200), (140, 40, 120), (255, 160, 235)),  # magenta
    "ship_replay": ((190, 255, 215, 175), (110, 190, 150, 175), (240, 255, 245, 175)),  # ghost
}


def save(img, name, scale):
    img.resize((img.width * scale, img.height * scale), Image.NEAREST).save(name)


def ship(name, colors):
    main, shade, high = colors
    alpha = main[3] if len(main) == 4 else 255
    palette = {"o": (14, 18, 40, alpha), "a": main, "b": shade, "c": high, "w": (200, 250, 255, alpha),
               "e": (255, 140, 30, alpha), "f": (255, 235, 120, alpha)}
    img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    for y, half in enumerate(SHIP_HALF):
        for x, ch in enumerate(half + half[::-1]):
            if ch in palette:
                c = palette[ch]
                img.putpixel((x, y), c if len(c) == 4 else c + (255,))
    save(img, name + ".png", 3)  # 48x48


def bullet():
    img = Image.new("RGBA", (6, 6), (0, 0, 0, 0))
    for y in range(6):
        for x in range(6):
            d = ((x - 2.5) ** 2 + (y - 2.5) ** 2) ** 0.5
            if d < 1.0:
                img.putpixel((x, y), (255, 255, 255, 255))
            elif d < 2.0:
                img.putpixel((x, y), (255, 235, 90, 255))
            elif d < 3.0:
                img.putpixel((x, y), (255, 150, 30, 150))
    save(img, "bullet.png", 3)  # 18x18


def explosion():
    frames, grid, scale = 6, 16, 4  # 6 frames of 64x64 in one 384x64 strip
    radii = [2.5, 4.5, 6.5, 7.5, 7.8, 7.8]
    sheet = Image.new("RGBA", (frames * grid, grid), (0, 0, 0, 0))
    for i in range(frames):
        for y in range(grid):
            for x in range(grid):
                d = ((x - 7.5) ** 2 + (y - 7.5) ** 2) ** 0.5
                t = d / radii[i]
                dither = ((x * 73856093 ^ y * 19349663 ^ (i + 1) * 83492791) % 100) / 100.0  # fixed pseudo-random
                if i >= 4:  # embers: sparse specks flying outwards
                    if t < 1.0 and dither < 0.18 + 0.1 * (5 - i) and d > 3.0:
                        c = (255, 120, 40, 230 - 70 * (i - 4)) if i == 4 else (150, 90, 70, 150)
                        sheet.putpixel((i * grid + x, y), c)
                    continue
                if t > 1.0 or (i == 3 and t < 0.45):
                    continue
                if t < 0.35:
                    c = (255, 255, 230, 255)
                elif t < 0.65:
                    c = (255, 225, 80, 255)
                elif t < 0.9 or dither < 0.5:
                    c = (255, 140, 30, 255)
                else:
                    c = (200, 50, 30, 255)
                sheet.putpixel((i * grid + x, y), c)
    save(sheet, "explosion.png", scale)


def wall():
    img = Image.new("RGBA", (16, 16), (38, 44, 78, 255))
    for i in range(16):
        for edge in (0, 15):
            img.putpixel((i, edge), (62, 72, 122, 255))
            img.putpixel((edge, i), (62, 72, 122, 255))
        img.putpixel((i, 8), (30, 35, 64, 255))
    for x, y in ((2, 2), (13, 2), (2, 13), (13, 13), (2, 10), (13, 10), (2, 5), (13, 5)):
        img.putpixel((x, y), (120, 135, 190, 255))
    save(img, "wall.png", 2)  # 32x32


def stars():
    rng = random.Random(7)
    img = Image.new("RGBA", (512, 512), (0, 0, 0, 0))
    colors = [(255, 255, 255), (170, 200, 255), (255, 235, 170), (200, 170, 255)]
    for _ in range(150):
        x, y = rng.randrange(0, 510), rng.randrange(0, 510)
        r, g, b = rng.choice(colors)
        a = rng.choice([70, 110, 160, 230])
        for dx in range(2):
            for dy in range(2):
                img.putpixel((x + dx, y + dy), (r, g, b, a))
    for _ in range(12):  # a few brighter plus-shaped stars
        x, y = rng.randrange(4, 508), rng.randrange(4, 508)
        for dx, dy in ((0, 0), (1, 0), (-1, 0), (0, 1), (0, -1)):
            img.putpixel((x + dx, y + dy), (255, 255, 255, 255 if (dx, dy) == (0, 0) else 140))
    img.save("stars.png")


if __name__ == "__main__":
    for name, colors in SHIP_COLORS.items():
        ship(name, colors)
    bullet()
    explosion()
    wall()
    stars()
