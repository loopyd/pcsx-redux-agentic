#!/usr/bin/env python3
# Emit the compressed RGBA8888 data used by src/gui/splash.cc.
import sys
import zlib
from PIL import Image

if len(sys.argv) != 2:
    sys.exit(f"Usage: {sys.argv[0]} <image_file>")

image = Image.open(sys.argv[1]).convert("RGBA")
width, height = image.size
packed = zlib.compress(image.tobytes(), 9)
print("// Generated using tools/splash-generator/splash.py; zlib-compressed RGBA8888.")
print(f"static constexpr uint32_t s_splashWidth = {width};")
print(f"static constexpr uint32_t s_splashHeight = {height};")
print("static constexpr uint8_t s_splashImageData[] = {")
for offset in range(0, len(packed), 32):
    print("    " + ", ".join(f"0x{value:02x}" for value in packed[offset:offset + 32]) + ",")
print("};")
