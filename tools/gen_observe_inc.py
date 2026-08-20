#!/usr/bin/env python3
"""Generate observe_html.inc from observe.html.
Each HTML line becomes a C string literal: "escaped_content\\n"
"""
import os

src_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
html_path = os.path.join(src_dir, "tools/www/observe.html")
inc_path = os.path.join(src_dir, "src/observe_html.inc")

with open(html_path, "r") as f:
    content = f.read()

lines = content.split("\n")

out = ["/* Observe Dashboard HTML */"]
out.append("static const char observe_html[] =")

for line in lines:
    frag = line + "\n"
    frag = frag.replace("\\", "\\\\")   # \ -> \\  (must be first!)
    frag = frag.replace('"', '\\"')     # " -> \"
    frag = frag.replace("\n", "\\n")    # actual newline -> \n escape
    out.append(f'"{frag}"')

out.append(";")
out.append("static const unsigned int observe_html_len = sizeof(observe_html) - 1;")
out.append("")

with open(inc_path, "w") as f:
    f.write("\n".join(out))
