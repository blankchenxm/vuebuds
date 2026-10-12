"""PC check of the firmware's JPEG / PNG encoders (src/codec*.c via codec_test.exe) against
OpenCV on real pictures: size, PSNR to the original, and that every file decodes.

    python run_codec_test.py <codec_test.exe> <picture.png> [<picture.png> ...]
"""
import pathlib
import subprocess
import sys
import tempfile

import cv2
import numpy as np


def psnr(a, b):
    mse = np.mean((a.astype(float) - b.astype(float)) ** 2)
    return 99.0 if mse == 0 else 10 * np.log10(255 ** 2 / mse)


def main():
    exe, pictures = sys.argv[1], sys.argv[2:]
    for pic in pictures:
        img = cv2.imread(pic, cv2.IMREAD_GRAYSCALE)
        h, w = img.shape
        with tempfile.TemporaryDirectory() as d:
            raw = pathlib.Path(d) / "in.raw"
            raw.write_bytes(img.tobytes())
            out = subprocess.run([exe, str(raw), str(w), str(h), d], capture_output=True, text=True).stdout
            times = {l.split()[0]: l.split()[-2] for l in out.splitlines() if l.endswith(" ms")}
            print(f"{pic} {w}x{h} ({w * h // 1024} KB raw)")
            for q in (95, 90, 85, 75, 60):
                data = (pathlib.Path(d) / f"q{q}.jpg").read_bytes()
                dec = cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_GRAYSCALE)
                ok, ref = cv2.imencode(".jpg", img, [cv2.IMWRITE_JPEG_QUALITY, q])
                refdec = cv2.imdecode(ref, cv2.IMREAD_GRAYSCALE)
                print(f"  JPEG q{q}: firmware {len(data) / 1024:6.1f} KB {psnr(img, dec):5.1f} dB | "
                      f"OpenCV {len(ref) / 1024:6.1f} KB {psnr(img, refdec):5.1f} dB | PC {times.get(f'q{q}.jpg')} ms")
            data = (pathlib.Path(d) / "l6.png").read_bytes()
            dec = cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_GRAYSCALE)
            ok, ref = cv2.imencode(".png", img)
            same = dec is not None and np.array_equal(dec, img)
            print(f"  PNG: firmware {len(data) / 1024:6.1f} KB lossless={same} | OpenCV {len(ref) / 1024:6.1f} KB "
                  f"| PC {times.get('l6.png')} ms")


if __name__ == "__main__":
    main()
