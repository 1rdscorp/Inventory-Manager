"""Draws packaging/app.ico (the same box symbol as the app's browser-tab icon). Needs Pillow."""
import os
from PIL import Image, ImageDraw

S = 1024
k = S / 24  # the design is drawn on a 24x24 grid
BLUE = (34, 87, 197, 255)
WHITE = (255, 255, 255, 255)


def p(x, y):
    return (x * k, y * k)


img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle([0, 0, S - 1, S - 1], radius=6 * k, fill=BLUE)

width = round(1.8 * k)
outline = [p(12, 5), p(18, 8.2), p(18, 15.8), p(12, 19), p(6, 15.8), p(6, 8.2), p(12, 5)]
inner = [p(6, 8.2), p(12, 11.4), p(18, 8.2)]
spine = [p(12, 11.4), p(12, 19)]
for line in (outline, inner, spine):
    d.line(line, fill=WHITE, width=width, joint="curve")
    for x, y in (line[0], line[-1]):  # round the line ends
        r = width / 2
        d.ellipse([x - r, y - r, x + r, y + r], fill=WHITE)

sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "app.ico")
img.resize((256, 256), Image.LANCZOS).save(out, format="ICO", sizes=[(n, n) for n in sizes])
print("wrote", out)
