#!/usr/bin/env python3
"""Convert original project artwork into launcher and executable resources."""
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def main():
    output = ROOT / 'out'
    output.mkdir(exist_ok=True)
    with Image.open(ROOT / 'assets/launcher-background.png') as image:
        image.convert('RGB').save(output / 'launcher-background.bmp', format='BMP')
    with Image.open(ROOT / 'assets/icon.png') as image:
        image.convert('RGB').save(output / 'aegis.bmp', format='BMP')
        image.convert('RGBA').save(output / 'aegis.ico', format='ICO',
            sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
    print('Original launcher artwork prepared in out/')


if __name__ == '__main__':
    main()
