"""Left / right stitching for the two-board pictures, two ways, to compare later:

  stitch_opencv  cv2.Stitcher (PANORAMA): OpenCV's full pipeline, used as the reference
  stitch_orb     the VueBuds paper's lightweight pipeline (§3.2.3): ORB keypoints ->
                 BFMatcher (Hamming) -> findHomography (RANSAC) -> warpPerspective, no
                 cropping; when it fails the two pictures are used as they are

Both take and return 8-bit grayscale images. Run on pairs saved by stereo.py (each pair's
stitch_opencv.png / stitch_orb.png is written next to its left.png / right.png):
  python stitch.py captures/stereo/<pair dir>      # window: left | right originals, stitch results on the right
  python stitch.py captures/stereo                 # every pair, one at a time (Space / left click: next,
                                                   #   p / right click: previous, q: quit)
  python stitch.py captures/stereo --no-show       # no window: stitch all, print the success counts
"""
import argparse
import pathlib
import sys
import time
from dataclasses import dataclass, field
from typing import Optional

import cv2
import numpy as np

ORB_FEATURES = 2000
RATIO = 0.75          # Lowe's ratio test on the 2 nearest matches
MIN_MATCHES = 10      # good matches needed to try a homography
MIN_INLIERS = 15      # RANSAC inliers needed to accept it
RANSAC_PX = 5.0
MIN_SIDE_SCALE = 0.5  # each side of the warped right image keeps 0.5x-2x its length
MAX_CANVAS = 4       # reject a homography that blows the canvas up past 4x the left image's width / height


@dataclass
class StitchResult:
    method: str
    ok: bool
    image: Optional[np.ndarray]
    ms: float
    detail: dict = field(default_factory=dict)

    def summary(self) -> str:
        extra = " ".join(f"{k}={v}" for k, v in self.detail.items())
        return f"{self.method}: {'ok' if self.ok else 'FAILED'} {self.ms:.0f} ms {extra}".rstrip()


_STITCHER_STATUS = {
    cv2.Stitcher_OK: "ok",
    cv2.Stitcher_ERR_NEED_MORE_IMGS: "need more images (too few matches)",
    cv2.Stitcher_ERR_HOMOGRAPHY_EST_FAIL: "homography estimation failed",
    cv2.Stitcher_ERR_CAMERA_PARAMS_ADJUST_FAIL: "camera parameter adjustment failed",
}


def stitch_opencv(left: np.ndarray, right: np.ndarray) -> StitchResult:
    t0 = time.perf_counter()
    stitcher = cv2.Stitcher_create(cv2.Stitcher_PANORAMA)
    try:
        status, pano = stitcher.stitch([cv2.cvtColor(left, cv2.COLOR_GRAY2BGR), cv2.cvtColor(right, cv2.COLOR_GRAY2BGR)])
    except cv2.error as e:
        return StitchResult("opencv", False, None, (time.perf_counter() - t0) * 1000, {"status": f"cv2.error {e.code}"})
    ms = (time.perf_counter() - t0) * 1000
    ok = status == cv2.Stitcher_OK and pano is not None
    image = cv2.cvtColor(pano, cv2.COLOR_BGR2GRAY) if ok else None
    return StitchResult("opencv", ok, image, ms, {"status": _STITCHER_STATUS.get(status, status)})


def stitch_orb(left: np.ndarray, right: np.ndarray) -> StitchResult:
    t0 = time.perf_counter()
    detail: dict = {}

    def fail(reason: str) -> StitchResult:
        detail["reason"] = reason
        return StitchResult("orb", False, None, (time.perf_counter() - t0) * 1000, detail)

    # 1. ORB keypoints and 256-bit binary descriptors
    orb = cv2.ORB_create(ORB_FEATURES)
    kp_l, des_l = orb.detectAndCompute(left, None)
    kp_r, des_r = orb.detectAndCompute(right, None)
    detail["keypoints"] = f"{len(kp_l)}/{len(kp_r)}"
    if des_l is None or des_r is None or len(kp_l) < 2 or len(kp_r) < 2:
        return fail("no keypoints")

    # 2. Match right -> left by Hamming distance, keep the clearly-best ones
    pairs = cv2.BFMatcher(cv2.NORM_HAMMING).knnMatch(des_r, des_l, k=2)
    good = [p[0] for p in pairs if len(p) == 2 and p[0].distance < RATIO * p[1].distance]
    detail["matches"] = len(good)
    if len(good) < MIN_MATCHES:
        return fail("too few matches")

    # 3. Homography right -> left; RANSAC rejects the wrong matches
    src = np.float32([kp_r[m.queryIdx].pt for m in good]).reshape(-1, 1, 2)
    dst = np.float32([kp_l[m.trainIdx].pt for m in good]).reshape(-1, 1, 2)
    H, mask = cv2.findHomography(src, dst, cv2.RANSAC, RANSAC_PX)
    inliers = int(mask.sum()) if mask is not None else 0
    detail["inliers"] = inliers
    if H is None or inliers < MIN_INLIERS:
        return fail("too few inliers")

    # Sanity: the warped right image must stay a plausible, non-flipped quadrilateral whose
    # sides keep roughly their length (a few matches on one object can fit a degenerate sliver)
    h_l, w_l = left.shape
    h_r, w_r = right.shape
    corners_r = np.float32([[0, 0], [w_r, 0], [w_r, h_r], [0, h_r]]).reshape(-1, 1, 2)
    warped = cv2.perspectiveTransform(corners_r, H)
    quad = warped.reshape(4, 2)
    sides = np.linalg.norm(quad - np.roll(quad, -1, axis=0), axis=1) / np.float32([w_r, h_r, w_r, h_r])
    if not cv2.isContourConvex(warped) or sides.min() < MIN_SIDE_SCALE or sides.max() > 1 / MIN_SIDE_SCALE:
        detail["sides"] = " ".join(f"{s:.2f}" for s in sides)
        return fail("implausible homography")

    # 4. Warp the right image onto a canvas holding both, left pasted on top, no cropping
    corners_l = np.float32([[0, 0], [w_l, 0], [w_l, h_l], [0, h_l]]).reshape(-1, 1, 2)
    allc = np.concatenate([corners_l, warped])
    x0, y0 = np.floor(allc.min(axis=(0, 1))).astype(int)
    x1, y1 = np.ceil(allc.max(axis=(0, 1))).astype(int)
    width, height = x1 - x0, y1 - y0
    if width > MAX_CANVAS * w_l or height > MAX_CANVAS * h_l:
        return fail("canvas too large")
    T = np.array([[1, 0, -x0], [0, 1, -y0], [0, 0, 1]], dtype=np.float64)
    canvas = cv2.warpPerspective(right, T @ H, (width, height))
    canvas[-y0:-y0 + h_l, -x0:-x0 + w_l] = left
    detail["size"] = f"{width}x{height}"
    return StitchResult("orb", True, canvas, (time.perf_counter() - t0) * 1000, detail)


def stitch_all(left: np.ndarray, right: np.ndarray) -> list[StitchResult]:
    return [stitch_opencv(left, right), stitch_orb(left, right)]


def stitch_dir(d: pathlib.Path) -> tuple[np.ndarray, np.ndarray, list[StitchResult]]:
    """Stitch d/left.png + d/right.png; writes stitch_<method>.png (removes a stale one on failure)."""
    left = cv2.imread(str(d / "left.png"), cv2.IMREAD_GRAYSCALE)
    right = cv2.imread(str(d / "right.png"), cv2.IMREAD_GRAYSCALE)
    results = stitch_all(left, right)
    for r in results:
        out = d / f"stitch_{r.method}.png"
        if r.ok:
            cv2.imwrite(str(out), r.image)
        elif out.exists():
            out.unlink()
    return left, right, results


# ---- viewer: originals on the left, stitch results on the right ----

WINDOW = "vuebuds stitch"


def _fit(img: np.ndarray, w: int, h: int) -> np.ndarray:
    """img scaled to fit inside w x h, on a black w x h BGR tile (top-left aligned)."""
    tile = np.zeros((h, w, 3), np.uint8)
    scale = min(w / img.shape[1], h / img.shape[0])
    size = (max(1, round(img.shape[1] * scale)), max(1, round(img.shape[0] * scale)))
    small = cv2.resize(img, size, interpolation=cv2.INTER_AREA if scale < 1 else cv2.INTER_LINEAR)
    tile[:size[1], :size[0]] = cv2.cvtColor(small, cv2.COLOR_GRAY2BGR)
    return tile


def _label(img: np.ndarray, text: str, y: int = 24) -> np.ndarray:
    """Green text on a darkened box (a thick black outline drifts: Hershey glyphs widen with thickness)."""
    (w, h), base = cv2.getTextSize(text, cv2.FONT_HERSHEY_SIMPLEX, 0.6, 1)
    x0, y0, x1, y1 = 4, max(0, y - h - 4), min(img.shape[1], 12 + w), min(img.shape[0], y + base + 2)
    img[y0:y1, x0:x1] = (img[y0:y1, x0:x1] * 0.35).astype(np.uint8)
    cv2.putText(img, text, (8, y), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (80, 255, 80), 1, cv2.LINE_AA)
    return img


def compose(name: str, left: np.ndarray, right: np.ndarray, results: list[StitchResult], h: int) -> np.ndarray:
    """Left half: left | right originals, h high. Right half: each stitch result, stacked."""
    cell_w = round(left.shape[1] * h / left.shape[0])
    originals = np.hstack([_label(_fit(left, cell_w, h), "left"), _label(_fit(right, cell_w, h), "right")])
    res_w, res_h = h, h // len(results)
    tiles = []
    for r in results:
        if r.ok:
            tile = _label(_fit(r.image, res_w, res_h), f"{r.method}: ok {r.ms:.0f} ms")
        else:
            reason = r.detail.get("reason") or r.detail.get("status")
            tile = _label(np.zeros((res_h, res_w, 3), np.uint8), f"{r.method}: FAILED ({reason})")
        cv2.line(tile, (0, res_h - 1), (res_w, res_h - 1), (255, 255, 255), 1)
        tiles.append(tile)
    gap = np.full((h, 6, 3), 255, np.uint8)
    view = np.hstack([originals, gap, np.vstack(tiles)])
    return _label(view, name, y=h - 12)


def show(dirs: list[pathlib.Path], height: int) -> None:
    """One pair at a time. Next: Space / Enter / n / left click; previous: p / right click; q / Esc quits."""
    cache: dict[int, np.ndarray] = {}
    clicks: list[int] = []
    cv2.namedWindow(WINDOW, cv2.WINDOW_AUTOSIZE)
    cv2.setMouseCallback(WINDOW, lambda event, *_: clicks.append(event)
                         if event in (cv2.EVENT_LBUTTONDOWN, cv2.EVENT_RBUTTONDOWN) else None)
    i = 0
    while True:
        if i not in cache:
            d = dirs[i]
            left, right, results = stitch_dir(d)
            print(f"{d.name}: " + " | ".join(r.summary() for r in results))
            cache[i] = compose(f"{d.name}  ({i + 1}/{len(dirs)})", left, right, results, height)
        cv2.setWindowTitle(WINDOW, f"{WINDOW} - {dirs[i].name} ({i + 1}/{len(dirs)})"
                                   + ("  |  Space / left click: next, p / right click: previous" if len(dirs) > 1 else ""))
        cv2.imshow(WINDOW, cache[i])
        key = cv2.waitKey(30) & 0xFF
        if key in (ord("q"), 27) or cv2.getWindowProperty(WINDOW, cv2.WND_PROP_VISIBLE) < 1:
            break
        click = clicks.pop(0) if clicks else None
        if key in (ord(" "), 13, ord("n")) or click == cv2.EVENT_LBUTTONDOWN:
            i = min(i + 1, len(dirs) - 1)
        elif key == ord("p") or click == cv2.EVENT_RBUTTONDOWN:
            i = max(i - 1, 0)
    cv2.destroyAllWindows()


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", type=pathlib.Path,
                    help="a pair directory (left.png / right.png), or a directory of them (captures/stereo)")
    ap.add_argument("--no-show", action="store_true", help="only stitch and print, no window")
    ap.add_argument("--height", type=int, default=640, help="window height (pixels)")
    args = ap.parse_args(argv)
    dirs = []
    for a in args.paths:
        dirs += [a] if (a / "left.png").exists() else sorted(p.parent for p in a.glob("*/left.png"))
    if not dirs:
        print("no left.png / right.png found")
        return 1
    if not args.no_show:
        show(dirs, args.height)
        return 0
    ok = {"opencv": 0, "orb": 0}
    for d in dirs:
        _, _, results = stitch_dir(d)
        print(f"{d.name}: " + " | ".join(r.summary() for r in results))
        for r in results:
            ok[r.method] += r.ok
    print(f"{len(dirs)} pairs: " + ", ".join(f"{m} ok {n}" for m, n in ok.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

