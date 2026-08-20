#!/usr/bin/env python3
"""Regenerate hex arrays in server_const.c from source HTML files."""
import sys, os

src_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
html_path = os.path.join(src_dir, "tools/www/index.html")
c_path = os.path.join(src_dir, "src/server_const.c")

# Read source
with open(html_path, "rb") as f:
    html_bytes = f.read()

with open(c_path, "r") as f:
    const_c = f.read()

# Find markers
start_marker = "const unsigned char index_html[] = {"
end_marker = "const unsigned int index_html_len"

start = const_c.find(start_marker)
end = const_c.find(end_marker)
assert start != -1, "start marker not found"
assert end != -1, "end marker not found"

# Three parts:
part1 = const_c[:start]  # everything before the array declaration
part3 = const_c[end:]    # everything from index_html_len onward (includes end_marker)

# Generate hex array
hex_lines = []
for i in range(0, len(html_bytes), 16):
    chunk = html_bytes[i:i+16]
    hex_str = ", ".join(f"0x{b:02x}" for b in chunk)
    hex_lines.append(f"  {hex_str},")

# New middle part: declaration + hex data + closing semicolon + blank line
part2 = start_marker + "\n" + "\n".join(hex_lines) + "\n};\n\n"

new_content = part1 + part2 + part3

with open(c_path, "w") as f:
    f.write(new_content)

print(f"OK: HTML={len(html_bytes)}B -> C={len(new_content)}B")
