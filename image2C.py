#!/usr/bin/env python3
"""
Convert a PNG image to a C array structure (RGB565, big-endian, octal escapes).
Automatically scales the image proportionally to fit within 160x160.
Outputs to lionstdio_logo.h.
"""

import sys
from PIL import Image

def rgb888_to_rgb565_be(r, g, b):
    """
    Convert 24-bit RGB to 16-bit RGB565 (big-endian byte order).
    Returns two bytes: high byte first, low byte second.
    """
    r5 = (r >> 3) & 0x1F
    g6 = (g >> 2) & 0x3F
    b5 = (b >> 3) & 0x1F
    rgb565 = (r5 << 11) | (g6 << 5) | b5
    return (rgb565 >> 8) & 0xFF, rgb565 & 0xFF

def compute_scaled_size(orig_width, orig_height, max_dim=160):
    """Return (new_width, new_height) scaled proportionally to fit within max_dim."""
    scale = min(max_dim / orig_width, max_dim / orig_height)
    new_width = int(orig_width * scale)
    new_height = int(orig_height * scale)
    # Ensure at least 1x1
    return max(1, new_width), max(1, new_height)

def image_to_c_array(image_path, max_dim=160):
    # Open and convert to RGB
    img = Image.open(image_path).convert("RGB")
    orig_width, orig_height = img.size

    # Compute target size (proportional, max dimension <= max_dim)
    target_width, target_height = compute_scaled_size(orig_width, orig_height, max_dim)

    # Resize
    if (orig_width, orig_height) != (target_width, target_height):
        img = img.resize((target_width, target_height), Image.Resampling.LANCZOS)

    # Collect raw bytes in big-endian order (high byte first)
    raw_bytes = []
    for y in range(target_height):
        for x in range(target_width):
            r, g, b = img.getpixel((x, y))
            hi, lo = rgb888_to_rgb565_be(r, g, b)
            raw_bytes.append(hi)
            raw_bytes.append(lo)

    # Convert each byte to octal escape sequence \ooo
    octal_str = ''.join(f'\\{b:03o}' for b in raw_bytes)

    # Generate C header
    total_pixels = target_width * target_height
    array_size = total_pixels * 2 + 1   # +1 for null terminator (string literal)

    lines = []
    lines.append(f"static const struct {{")
    lines.append(f"  unsigned int  width;")
    lines.append(f"  unsigned int  height;")
    lines.append(f"  unsigned int  bytes_per_pixel; /* 2:RGB16, 3:RGB, 4:RGBA */")
    lines.append(f"  unsigned char pixel_data[{array_size}];")
    lines.append(f"}} image_lionstdio = {{")
    lines.append(f"  {target_width}, {target_height}, 2,")

    # Split the octal string into lines of about 80 characters
    max_line_len = 80
    line = '  "'
    for i in range(0, len(octal_str), 4):   # each escape is 4 chars: backslash + 3 digits
        chunk = octal_str[i:i+4]
        if len(line) + len(chunk) > max_line_len:
            lines.append(line + '"')
            line = '  "' + chunk
        else:
            line += chunk
    if line != '  "':
        lines.append(line + '"')
    else:
        lines.append('  ""')

    lines.append("};")

    with open("image_lionstdio.h", 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines))

    print(f"Successfully generated image_lionstdio.h")
    print(f"Original size: {orig_width}x{orig_height} -> Scaled to: {target_width}x{target_height}")
    print(f"Total bytes in pixel_data: {len(raw_bytes)}")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: python png2c.py <input_image.png>")
        sys.exit(1)

    input_png = sys.argv[1]
    image_to_c_array(input_png, max_dim=160)