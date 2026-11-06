#!/usr/bin/env python3
"""Convert an HTML file into a C string literal .inc file for embedding."""

import sys
import os

def html_to_c_literal(html_path, var_name, label):
    """Convert HTML to C string literal array."""
    with open(html_path, 'r', encoding='utf-8') as f:
        content = f.read()

    lines = []
    lines.append(f"/* {label} */")
    lines.append(f"static const char {var_name}[] =")

    # Split by newlines and escape each line
    for line in content.split('\n'):
        escaped = line.replace('\\', '\\\\').replace('"', '\\"')
        lines.append(f'"{escaped}\\n"')

    lines.append(";")
    lines.append(f"static const unsigned int {var_name}_len = sizeof({var_name}) - 1;")
    return '\n'.join(lines)

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("Usage: html2inc.py <input.html> <output.inc> [var_name] [label]")
        sys.exit(1)

    html_path = sys.argv[1]
    inc_path = sys.argv[2]
    var_name = sys.argv[3] if len(sys.argv) > 3 else 'embedded_html'
    label = sys.argv[4] if len(sys.argv) > 4 else f'HTML embedded in {var_name}'

    result = html_to_c_literal(html_path, var_name, label)
    with open(inc_path, 'w', encoding='utf-8') as f:
        f.write(result)
        f.write('\n')

    orig_size = os.path.getsize(html_path)
    inc_size = len(result)
    print(f"Generated {inc_path}: {orig_size} bytes HTML -> {inc_size} bytes C literal")
