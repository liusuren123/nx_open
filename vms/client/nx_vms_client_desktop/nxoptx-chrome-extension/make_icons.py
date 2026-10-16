"""Generates the extension icons: a green play triangle on a dark rounded square.

Needs Pillow: `python -m pip install pillow`. Run from this directory.
"""

import os

from PIL import Image, ImageDraw

SIZES = (16, 32, 48, 128)
BACKGROUND = (31, 31, 31, 255)
FOREGROUND = (76, 191, 39, 255)


def make(size):
    # Draw large and shrink, so the corners and the triangle stay smooth.
    scale = 8
    canvas = size * scale
    image = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    draw.rounded_rectangle(
        [0, 0, canvas - 1, canvas - 1], radius=int(canvas * 0.22), fill=BACKGROUND)

    center_x, center_y = canvas * 0.545, canvas * 0.5
    half_width, half_height = canvas * 0.20, canvas * 0.25
    draw.polygon(
        [
            (center_x - half_width, center_y - half_height),
            (center_x - half_width, center_y + half_height),
            (center_x + half_width, center_y),
        ],
        fill=FOREGROUND)

    return image.resize((size, size), Image.LANCZOS)


def main():
    directory = os.path.join(os.path.dirname(os.path.abspath(__file__)), "icons")
    os.makedirs(directory, exist_ok=True)
    for size in SIZES:
        path = os.path.join(directory, f"icon{size}.png")
        make(size).save(path)
        print("wrote", path)


if __name__ == "__main__":
    main()
