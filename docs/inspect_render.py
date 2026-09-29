"""Reports where the vertical table borders land in a rendered page image."""

import sys
from collections import Counter

from PIL import Image

path = sys.argv[1]
image = Image.open(path).convert("RGB")
width, height = image.size
pixels = image.load()

print(f"{path}: {width}x{height}")

rows_checked = 0
border_columns = Counter()

for y in range(0, height, 4):
    for x in range(width):
        red, green, blue = pixels[x, y]
        # the table borders are a medium blue/grey; text is nearly black
        if blue > 120 and blue - red > 40 and red < 150:
            border_columns[x] += 1

    rows_checked += 1

likely = [x for x, count in border_columns.items() if count > 5]
print("column histogram peaks:", sorted(likely)[:40])
print("rightmost page pixel with ink:", max(
    (x for x in range(width) for y in range(height) if sum(pixels[x, y]) < 600), default=None))
