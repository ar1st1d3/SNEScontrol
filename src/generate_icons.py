#!/usr/bin/env python3
import os
from PIL import Image

def main():
    source_path = "OIP.Ffp9fIjabiJKp4y-JJAAEAHaHa.webp"
    if not os.path.exists(source_path):
        print(f"Error: {source_path} not found")
        return 1

    img = Image.open(source_path).convert("RGBA")
    
    # Save main icon
    img.save("snescontrol.png", format="PNG")
    print("Generated: snescontrol.png")

    sizes = [16, 24, 32, 48, 64, 128, 256, 512]
    for s in sizes:
        d = os.path.join("icons", f"{s}x{s}")
        os.makedirs(d, exist_ok=True)
        resized = img.resize((s, s), Image.Resampling.LANCZOS)
        out_path = os.path.join(d, "snescontrol.png")
        resized.save(out_path, format="PNG")
        print(f"Generated: {out_path}")

    # Scalable / hicolor
    hicolor_dir = os.path.join("icons", "hicolor")
    os.makedirs(hicolor_dir, exist_ok=True)
    img.save(os.path.join(hicolor_dir, "snescontrol.png"), format="PNG")
    print("All icons successfully generated.")
    return 0

if __name__ == "__main__":
    exit(main())
