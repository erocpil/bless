#!/usr/bin/env python3
"""Generate entropy_html.inc from entropy.html.
Each HTML line becomes a C string literal: "escaped_content\n"
"""
import os

src_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
html_path = os.path.join(src_dir, "tools/www/entropy.html")
inc_path = os.path.join(src_dir, "src/entropy_html.inc")

with open(html_path, "r") as f:
    content = f.read()

# Split into lines; we want the newline at end of each line as \n escape
lines = content.split("\n")

out = ["/* entropy dashboard HTML served at /entropy */"]
out.append("static const char entropy_html[] =")

for line in lines:
    # Append newline (it was stripped by split)
    # Then C-escape: " -> \", actual newline -> \n, \ -> \\
    frag = line + "\n"
    frag = frag.replace("\\", "\\\\")   # \ -> \\  (must be first!)
    frag = frag.replace('"', '\\"')     # " -> \"
    frag = frag.replace("\n", "\\n")    # actual newline -> \n escape
    out.append(f'"{frag}"')

out.append(";")
out.append("static const unsigned int entropy_html_len = sizeof(entropy_html) - 1;")
out.append("")

with open(inc_path, "w") as f:
    f.write("\n".join(out))
