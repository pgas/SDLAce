#!/usr/bin/env python3
import os
import sys

def convert_png_to_header(png_path, header_path, var_name):
    if not os.path.exists(png_path):
        print(f"Error: {png_path} does not exist.")
        sys.exit(1)

    with open(png_path, "rb") as f:
        data = f.read()

    os.makedirs(os.path.dirname(header_path), exist_ok=True)

    with open(header_path, "w") as f:
        guard = os.path.basename(header_path).replace(".", "_").upper()
        f.write(f"#ifndef {guard}\n#define {guard}\n\n")
        f.write(f"static const unsigned int {var_name}_len = {len(data)};\n")
        f.write(f"static const unsigned char {var_name}[] = {{\n")
        for i in range(0, len(data), 12):
            chunk = data[i:i+12]
            hex_str = ", ".join(f"0x{b:02x}" for b in chunk)
            f.write(f"  {hex_str},\n")
        f.write(f"}};\n\n#endif /* {guard} */\n")

    print(f"Successfully generated {header_path} ({len(data)} bytes) from {png_path}")

if __name__ == "__main__":
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.abspath(os.path.join(script_dir, ".."))
    
    png_file = os.path.join(script_dir, "ace.png")
    header_file = os.path.join(project_root, "src", "ace_logo_data.h")
    
    convert_png_to_header(png_file, header_file, "ace_logo_png")
