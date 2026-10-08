"""Left / right stitching for the two-board pictures, two ways, to compare later:

  stitch_opencv  cv2.Stitcher (PANORAMA): OpenCV's full pipeline, used as the reference
  stitch_orb     the VueBuds paper's lightweight pipeline (§3.2.3): ORB keypoints ->
                 BFMatcher (Hamming) -> findHomography (RANSAC) -> warpPerspective, no
                 cropping; when it fails the two pictures are used as they are

Both take and return 8-bit grayscale images. Re-run on saved pairs:
  python stitch.py captures/stereo/<pair dir>      # reads left.png / right.png, writes stitch_*.png
"""
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


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print(__doc__)
        return 1
    d = pathlib.Path(argv[0])
    left = cv2.imread(str(d / "left.png"), cv2.IMREAD_GRAYSCALE)
    right = cv2.imread(str(d / "right.png"), cv2.IMREAD_GRAYSCALE)
    if left is None or right is None:
        print(f"{d}: left.png / right.png not found")
        return 1
    for r in stitch_all(left, right):
        print(r.summary())
        if r.ok:
            cv2.imwrite(str(d / f"stitch_{r.method}.png"), r.image)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
