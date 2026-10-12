"""Single-picture VLM sets from the phone photos of the 10-09 objects (vlm_eval.py input).

  python make_phone_eval.py captures/20261009_phone captures/20261009_phone_eval

For every <object>.jpg writes <out>/<variant>/<object>/image.png:
  color_2048  colour, longer side 2048 (the upper bound we send; 24 MP is too big for the models)
  gray_2048   the same in grayscale (HM0360 is monochrome: separates colour from resolution)
  gray_640    grayscale 640 x 480: the VGA pixel count without the HM0360's optics / noise
  gray_320    grayscale 320 x 240: the same for QVGA
"""
import pathlib
import sys

import cv2

VARIANTS = {"color_2048": (2048, False), "gray_2048": (2048, True), "gray_640": (640, True), "gray_320": (320, True)}


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__)
        return 1
    src, out = map(pathlib.Path, argv)
    for photo in sorted(src.glob("*.jpg")):
        im = cv2.imread(str(photo))  # applies the EXIF orientation
        for name, (side, gray) in VARIANTS.items():
            scale = side / max(im.shape[:2])
            small = cv2.resize(im, None, fx=scale, fy=scale, interpolation=cv2.INTER_AREA)
            if gray:
                small = cv2.cvtColor(small, cv2.COLOR_BGR2GRAY)
            d = out / name / photo.stem
            d.mkdir(parents=True, exist_ok=True)
            cv2.imwrite(str(d / "image.png"), small)
        print(photo.name, im.shape[1::-1], "->", ", ".join(VARIANTS))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
