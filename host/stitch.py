"""Left / right stitching for the two-board pictures, several methods, to compare:

  baselines
    opencv        cv2.Stitcher PANORAMA: OpenCV's full pipeline (spherical warp, bundle
                  adjustment, seam, multi-band blending)
    opencv_scans  cv2.Stitcher SCANS: affine model, no spherical warp
    orb           the VueBuds paper's pipeline (§3.2.3): ORB -> BFMatcher (Hamming) ->
                  findHomography (RANSAC) -> warpPerspective, left pasted on top, no cropping
  stronger matching (same single homography + paste as orb, only the matcher changes)
    sift          OpenCV SIFT
    asift         affine-simulated SIFT (cv2.AffineFeature): for large viewpoint changes
    lightglue     SuperPoint + LightGlue   (optional: needs torch + lightglue)
    loftr         LoFTR, dense matching    (optional: needs torch + kornia)
  parallax-tolerant (matches from --apap-matcher, default sift; feathered blend)
    apap          As-Projective-As-Possible (Moving DLT): a homography per 10 px cell, each
                  fitted with weights that fall off with distance to the cell
    multi_h       several homographies found by sequential RANSAC (e.g. wall, object); each
                  cell takes the one whose inliers are closest (a simplified multi-plane warp)

All take and return 8-bit grayscale images. On pairs saved by stereo.py:
  python stitch.py captures/stereo/<pair dir>      # window: originals on the left, results on the right
  python stitch.py captures/stereo/20261009_QVGA   # every pair, one at a time (Space / left click: next,
                                                   #   p / right click: previous, q: quit)
  python stitch.py <set dir> --no-show             # no window: write the files below, print a summary
  python stitch.py <set dir> --methods orb sift apap
Files per pair: stitch_<method>.png (when it worked), matches_<method>.png, compare.png,
stitch_results.json. Per set (--no-show): stitch_summary.csv, stitch_score.csv (manual 1-5 scores).
"""
import argparse
import csv
import json
import math
import os
import pathlib
import sys
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

import cv2
import numpy as np

FEATURES = 4000       # keypoints per image (ORB: 2000, as before)
ORB_FEATURES = 2000
RATIO = 0.75          # Lowe's ratio test on the 2 nearest matches
MIN_MATCHES = 10      # good matches needed to try a homography
MIN_INLIERS = 15      # RANSAC inliers needed to accept it
RANSAC_PX = 5.0
MIN_SIDE_SCALE = 0.5  # each side of the warped right image keeps 0.5x-2x its length
MAX_CANVAS = 4        # reject a homography that blows the canvas up past 4x the left image's width / height
CELL_PX = 10          # APAP / multi_h: one homography per CELL_PX x CELL_PX canvas cell
APAP_SIGMA = 0.06     # APAP weight fall-off, as a fraction of the left image's longer side
APAP_GAMMA = 0.05     # APAP weight floor (keeps far cells close to the global homography)
APAP_RANSAC = 0.04    # APAP keeps matches within this fraction of the image diagonal of the global fit
MULTI_H_MAX = 4


@dataclass
class StitchResult:
    method: str
    ok: bool
    image: Optional[np.ndarray]
    ms: float
    detail: dict = field(default_factory=dict)
    matches: Optional[np.ndarray] = None  # match visualisation, when the method matches features

    def summary(self) -> str:
        extra = " ".join(f"{k}={v}" for k, v in self.detail.items())
        return f"{self.method}: {'ok' if self.ok else 'FAILED'} {self.ms:.0f} ms {extra}".rstrip()


class Fail(Exception):
    pass


# ---------------------------------------------------------------- matching

def _match_cv(left, right, detector, norm, detail) -> tuple[np.ndarray, np.ndarray]:
    kp_l, des_l = detector.detectAndCompute(left, None)
    kp_r, des_r = detector.detectAndCompute(right, None)
    detail["keypoints"] = f"{len(kp_l)}/{len(kp_r)}"
    if des_l is None or des_r is None or len(kp_l) < 2 or len(kp_r) < 2:
        raise Fail("no keypoints")
    pairs = cv2.BFMatcher(norm).knnMatch(des_r, des_l, k=2)
    good = [p[0] for p in pairs if len(p) == 2 and p[0].distance < RATIO * p[1].distance]
    src = np.float32([kp_r[m.queryIdx].pt for m in good]).reshape(-1, 2)
    dst = np.float32([kp_l[m.trainIdx].pt for m in good]).reshape(-1, 2)
    return src, dst


_torch_models: dict = {}


def _torch_device():
    import torch
    # The scoop Python has no CA store, so the first weight download fails certificate checks;
    # pip's bundled certifi file works.
    ca = pathlib.Path(sys.prefix) / "Lib" / "site-packages" / "pip" / "_vendor" / "certifi" / "cacert.pem"
    if "SSL_CERT_FILE" not in os.environ and ca.exists():
        os.environ["SSL_CERT_FILE"] = str(ca)
    return torch.device("cuda" if torch.cuda.is_available() else "cpu")


def _match_lightglue(left, right, detail):
    import torch
    from lightglue import LightGlue, SuperPoint
    from lightglue.utils import rbd
    dev = _torch_device()
    if "lightglue" not in _torch_models:
        _torch_models["lightglue"] = (SuperPoint(max_num_keypoints=FEATURES).eval().to(dev),
                                      LightGlue(features="superpoint").eval().to(dev))
    extractor, matcher = _torch_models["lightglue"]
    t = lambda im: torch.from_numpy(im).float().div(255)[None].to(dev)
    with torch.inference_mode():
        f_l, f_r = extractor.extract(t(left)), extractor.extract(t(right))
        m = rbd(matcher({"image0": f_r, "image1": f_l}))
    f_l, f_r = rbd(f_l), rbd(f_r)
    idx = m["matches"].cpu().numpy()
    detail["keypoints"] = f"{len(f_l['keypoints'])}/{len(f_r['keypoints'])}"
    detail["device"] = dev.type
    return f_r["keypoints"].cpu().numpy()[idx[:, 0]], f_l["keypoints"].cpu().numpy()[idx[:, 1]]


def _match_loftr(left, right, detail):
    import torch
    import kornia.feature as KF
    dev = _torch_device()
    if "loftr" not in _torch_models:
        _torch_models["loftr"] = KF.LoFTR(pretrained="indoor").eval().to(dev)
    t = lambda im: torch.from_numpy(im).float().div(255)[None, None].to(dev)
    with torch.inference_mode():
        out = _torch_models["loftr"]({"image0": t(right), "image1": t(left)})
    conf = out["confidence"].cpu().numpy()
    keep = conf > 0.5
    detail["device"] = dev.type
    return out["keypoints0"].cpu().numpy()[keep], out["keypoints1"].cpu().numpy()[keep]


MATCHERS: dict[str, Callable] = {
    "orb": lambda l, r, d: _match_cv(l, r, cv2.ORB_create(ORB_FEATURES), cv2.NORM_HAMMING, d),
    "sift": lambda l, r, d: _match_cv(l, r, cv2.SIFT_create(FEATURES), cv2.NORM_L2, d),
    # ASIFT: SIFT on affine-simulated views, for large viewpoint changes (OpenCV 5 has no AKAZE)
    "asift": lambda l, r, d: _match_cv(l, r, cv2.AffineFeature_create(cv2.SIFT_create(FEATURES // 4)), cv2.NORM_L2, d),
    "lightglue": _match_lightglue,
    "loftr": _match_loftr,
}


def matcher_available(name: str) -> bool:
    try:
        if name == "lightglue":
            import torch, lightglue  # noqa: F401
        elif name == "loftr":
            import torch, kornia  # noqa: F401
        return True
    except ImportError:
        return False


# ---------------------------------------------------------------- geometry helpers

def _plausible(H: np.ndarray, w_r: int, h_r: int) -> Optional[str]:
    """None if the warped right image stays a convex quad with sides 0.5x-2x long, else the sides."""
    corners = np.float32([[0, 0], [w_r, 0], [w_r, h_r], [0, h_r]]).reshape(-1, 1, 2)
    quad = cv2.perspectiveTransform(corners, H).reshape(4, 2)
    sides = np.linalg.norm(quad - np.roll(quad, -1, axis=0), axis=1) / np.float32([w_r, h_r, w_r, h_r])
    if not cv2.isContourConvex(quad.reshape(-1, 1, 2)) or sides.min() < MIN_SIDE_SCALE or sides.max() > 1 / MIN_SIDE_SCALE:
        return " ".join(f"{s:.2f}" for s in sides)
    return None


def _global_h(src, dst, w_r, h_r, detail, thresh=RANSAC_PX):
    detail["matches"] = len(src)
    if len(src) < MIN_MATCHES:
        raise Fail("too few matches")
    H, mask = cv2.findHomography(src.reshape(-1, 1, 2), dst.reshape(-1, 1, 2), cv2.RANSAC, thresh)
    inliers = int(mask.sum()) if mask is not None else 0
    detail["inliers"] = inliers
    if H is None or inliers < MIN_INLIERS:
        raise Fail("too few inliers")
    bad = _plausible(H, w_r, h_r)
    if bad:
        detail["sides"] = bad
        raise Fail("implausible homography")
    return H, mask.ravel().astype(bool)


def _canvas(left, right, H):
    """Offset (x0, y0) and size of a canvas holding the left image and the right one warped by H."""
    h_l, w_l = left.shape
    h_r, w_r = right.shape
    c_l = np.float32([[0, 0], [w_l, 0], [w_l, h_l], [0, h_l]]).reshape(-1, 1, 2)
    c_r = cv2.perspectiveTransform(np.float32([[0, 0], [w_r, 0], [w_r, h_r], [0, h_r]]).reshape(-1, 1, 2), H)
    allc = np.concatenate([c_l, c_r])
    x0, y0 = np.floor(allc.min(axis=(0, 1))).astype(int)
    x1, y1 = np.ceil(allc.max(axis=(0, 1))).astype(int)
    width, height = int(x1 - x0), int(y1 - y0)
    if width > MAX_CANVAS * w_l or height > MAX_CANVAS * h_l:
        raise Fail("canvas too large")
    return int(x0), int(y0), width, height


def _paste(left, warped_right, x0, y0):
    canvas = warped_right.copy()
    h_l, w_l = left.shape
    canvas[-y0:-y0 + h_l, -x0:-x0 + w_l] = left
    return canvas


def _feather(left, warped_right, valid_right, x0, y0):
    """Blend the overlap with weights growing away from each image's border."""
    height, width = warped_right.shape
    h_l, w_l = left.shape
    big_l = np.zeros((height, width), np.float32)
    big_l[-y0:-y0 + h_l, -x0:-x0 + w_l] = left
    valid_l = np.zeros((height, width), np.uint8)
    valid_l[-y0:-y0 + h_l, -x0:-x0 + w_l] = 1
    w_l_map = cv2.distanceTransform(np.pad(valid_l, 1), cv2.DIST_L2, 3)[1:-1, 1:-1]
    w_r_map = cv2.distanceTransform(np.pad(valid_right.astype(np.uint8), 1), cv2.DIST_L2, 3)[1:-1, 1:-1]
    total = w_l_map + w_r_map
    out = np.where(total > 0, (big_l * w_l_map + warped_right.astype(np.float32) * w_r_map) / np.maximum(total, 1e-6), 0)
    return np.clip(out, 0, 255).astype(np.uint8)


def _draw_matches(left, right, src, dst, inlier):
    """right | left side by side, inlier matches green, outliers red (at most 300 lines each)."""
    h = max(left.shape[0], right.shape[0])
    vis = np.zeros((h, right.shape[1] + left.shape[1], 3), np.uint8)
    vis[:right.shape[0], :right.shape[1]] = cv2.cvtColor(right, cv2.COLOR_GRAY2BGR)
    vis[:left.shape[0], right.shape[1]:] = cv2.cvtColor(left, cv2.COLOR_GRAY2BGR)
    rng = np.random.default_rng(0)
    for keep, color in ((~inlier, (0, 0, 255)), (inlier, (0, 220, 0))):
        idx = np.flatnonzero(keep)
        for i in rng.permutation(idx)[:300]:
            p = tuple(int(v) for v in src[i])
            q = (int(dst[i][0]) + right.shape[1], int(dst[i][1]))
            cv2.line(vis, p, q, color, 1, cv2.LINE_AA)
            cv2.circle(vis, p, 2, color, -1)
            cv2.circle(vis, q, 2, color, -1)
    _label(vis, "right", 24)
    cv2.putText(vis, "left", (right.shape[1] + 8, 24), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (80, 255, 80), 1, cv2.LINE_AA)
    return vis


def _warp_cells(right, cell_h_inv, x0, y0, width, height):
    """Warp `right` onto the canvas with one canvas->right homography per CELL_PX cell."""
    ys, xs = np.mgrid[0:height, 0:width].astype(np.float32)
    iy = np.minimum((ys // CELL_PX).astype(int), cell_h_inv.shape[0] - 1)
    ix = np.minimum((xs // CELL_PX).astype(int), cell_h_inv.shape[1] - 1)
    Hp = cell_h_inv[iy, ix]                       # (height, width, 3, 3)
    X = np.stack([xs + x0, ys + y0, np.ones_like(xs)], axis=-1)  # left-image coordinates
    R = np.einsum("hwij,hwj->hwi", Hp, X)
    with np.errstate(divide="ignore", invalid="ignore"):
        mx = (R[..., 0] / R[..., 2]).astype(np.float32)
        my = (R[..., 1] / R[..., 2]).astype(np.float32)
    mx[~np.isfinite(mx)] = -1
    my[~np.isfinite(my)] = -1
    warped = cv2.remap(right, mx, my, cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT, borderValue=0)
    valid = (mx >= 0) & (my >= 0) & (mx <= right.shape[1] - 1) & (my <= right.shape[0] - 1)
    return warped, valid


def _cell_centres(x0, y0, width, height):
    gy, gx = math.ceil(height / CELL_PX), math.ceil(width / CELL_PX)
    cy, cx = np.mgrid[0:gy, 0:gx].astype(np.float64)
    return np.stack([cx * CELL_PX + CELL_PX / 2 + x0, cy * CELL_PX + CELL_PX / 2 + y0], axis=-1)  # (gy, gx, 2), left coords


def _normalise(p):
    m = p.mean(axis=0)
    s = math.sqrt(2) / max(np.mean(np.linalg.norm(p - m, axis=1)), 1e-9)
    return np.array([[s, 0, -s * m[0]], [0, s, -s * m[1]], [0, 0, 1]])


def _apap_cells(pts_l, pts_r, centres, sigma, gamma):
    """Moving DLT: a left->right homography per cell centre, from distance-weighted correspondences."""
    T1, T2 = _normalise(pts_l), _normalise(pts_r)
    a = (np.c_[pts_l, np.ones(len(pts_l))] @ T1.T)
    b = (np.c_[pts_r, np.ones(len(pts_r))] @ T2.T)
    x, y, xp, yp = a[:, 0], a[:, 1], b[:, 0], b[:, 1]
    z, o = np.zeros_like(x), np.ones_like(x)
    r1 = np.stack([z, z, z, -x, -y, -o, yp * x, yp * y, yp], axis=1)
    r2 = np.stack([x, y, o, z, z, z, -xp * x, -xp * y, -xp], axis=1)
    M = np.einsum("ni,nj->nij", r1, r1) + np.einsum("ni,nj->nij", r2, r2)       # (N, 9, 9)
    d2 = ((centres[..., None, :] - pts_l[None, None]) ** 2).sum(-1)               # (gy, gx, N)
    w = np.maximum(np.exp(-d2 / sigma ** 2), gamma)
    C = np.einsum("yxn,nij->yxij", w ** 2, M)
    _, vecs = np.linalg.eigh(C)
    Hn = vecs[..., :, 0].reshape(*C.shape[:2], 3, 3)
    H = np.linalg.inv(T2) @ Hn @ T1
    return H / H[..., 2:3, 2:3]


# ---------------------------------------------------------------- methods

_STITCHER_STATUS = {
    cv2.Stitcher_OK: "ok",
    cv2.Stitcher_ERR_NEED_MORE_IMGS: "need more images (too few matches)",
    cv2.Stitcher_ERR_HOMOGRAPHY_EST_FAIL: "homography estimation failed",
    cv2.Stitcher_ERR_CAMERA_PARAMS_ADJUST_FAIL: "camera parameter adjustment failed",
}


def _cv_stitcher(mode, name):
    def run(left, right):
        t0 = time.perf_counter()
        try:
            status, pano = cv2.Stitcher_create(mode).stitch(
                [cv2.cvtColor(left, cv2.COLOR_GRAY2BGR), cv2.cvtColor(right, cv2.COLOR_GRAY2BGR)])
        except cv2.error as e:
            return StitchResult(name, False, None, (time.perf_counter() - t0) * 1000, {"status": f"cv2.error {e.code}"})
        ok = status == cv2.Stitcher_OK and pano is not None
        return StitchResult(name, ok, cv2.cvtColor(pano, cv2.COLOR_BGR2GRAY) if ok else None,
                            (time.perf_counter() - t0) * 1000, {"status": _STITCHER_STATUS.get(status, status)})
    return run


def _homography_method(matcher):
    """Single homography, left pasted on top: the paper's pipeline with a given matcher."""
    def run(left, right):
        t0 = time.perf_counter()
        detail: dict = {}
        vis = None
        try:
            src, dst = MATCHERS[matcher](left, right, detail)
            H, inl = _global_h(src, dst, right.shape[1], right.shape[0], detail)
            vis = _draw_matches(left, right, src, dst, inl)
            x0, y0, width, height = _canvas(left, right, H)
            T = np.array([[1, 0, -x0], [0, 1, -y0], [0, 0, 1]], dtype=np.float64)
            canvas = _paste(left, cv2.warpPerspective(right, T @ H, (width, height)), x0, y0)
            detail["size"] = f"{width}x{height}"
            return StitchResult(matcher, True, canvas, (time.perf_counter() - t0) * 1000, detail, vis)
        except Fail as e:
            detail["reason"] = str(e)
            if vis is None and "src" in locals() and len(src):
                vis = _draw_matches(left, right, src, dst, np.zeros(len(src), bool))
            return StitchResult(matcher, False, None, (time.perf_counter() - t0) * 1000, detail, vis)
    return run


def _local_method(name, matcher_name):
    """APAP or multi_h on top of a matcher; feathered blend so misalignment shows as ghosting."""
    def run(left, right):
        t0 = time.perf_counter()
        detail: dict = {"matcher": matcher_name}
        vis = None
        try:
            src, dst = MATCHERS[matcher_name](left, right, detail)
            diag = math.hypot(*left.shape)
            # Global fit for the canvas; keep a looser inlier set so parallax matches survive.
            H, _ = _global_h(src, dst, right.shape[1], right.shape[0], detail)
            x0, y0, width, height = _canvas(left, right, H)
            centres = _cell_centres(x0, y0, width, height)
            if name == "apap":
                err = np.linalg.norm(cv2.perspectiveTransform(src.reshape(-1, 1, 2), H).reshape(-1, 2) - dst, axis=1)
                keep = err < APAP_RANSAC * diag
                detail["used"] = int(keep.sum())
                vis = _draw_matches(left, right, src, dst, keep)
                sigma = APAP_SIGMA * max(left.shape)
                cell_h = _apap_cells(dst[keep].astype(np.float64), src[keep].astype(np.float64), centres, sigma, APAP_GAMMA)
            else:  # multi_h: sequential RANSAC, each cell takes the homography whose inliers are closest
                remaining = np.ones(len(src), bool)
                planes = []
                while len(planes) < MULTI_H_MAX and remaining.sum() >= MIN_INLIERS:
                    idx = np.flatnonzero(remaining)
                    Hk, mk = cv2.findHomography(src[idx].reshape(-1, 1, 2), dst[idx].reshape(-1, 1, 2), cv2.RANSAC, RANSAC_PX)
                    if Hk is None or mk.sum() < MIN_INLIERS or _plausible(Hk, right.shape[1], right.shape[0]):
                        break
                    members = idx[mk.ravel().astype(bool)]
                    planes.append((Hk, members))
                    remaining[members] = False
                detail["planes"] = "+".join(str(len(m)) for _, m in planes)
                labels = np.full(len(src), -1)
                for k, (_, m) in enumerate(planes):
                    labels[m] = k
                vis = _draw_matches(left, right, src, dst, labels >= 0)
                sigma = APAP_SIGMA * max(left.shape)
                scores = np.stack([np.exp(-((centres[..., None, :] - dst[m][None, None]) ** 2).sum(-1) / sigma ** 2).sum(-1)
                                   for _, m in planes], axis=-1)
                choice = np.where(scores.max(-1) > 1e-3, scores.argmax(-1), 0)
                inv = np.stack([np.linalg.inv(Hk) for Hk, _ in planes])
                cell_h = inv[choice]
            warped, valid = _warp_cells(right, cell_h, x0, y0, width, height)
            canvas = _feather(left, warped, valid, x0, y0)
            detail["size"] = f"{width}x{height}"
            return StitchResult(name, True, canvas, (time.perf_counter() - t0) * 1000, detail, vis)
        except Fail as e:
            detail["reason"] = str(e)
            return StitchResult(name, False, None, (time.perf_counter() - t0) * 1000, detail, vis)
    return run


def build_methods(apap_matcher: str = "sift") -> dict[str, Callable]:
    return {
        "opencv": _cv_stitcher(cv2.Stitcher_PANORAMA, "opencv"),
        "opencv_scans": _cv_stitcher(cv2.Stitcher_SCANS, "opencv_scans"),
        "orb": _homography_method("orb"),
        "sift": _homography_method("sift"),
        "asift": _homography_method("asift"),
        "lightglue": _homography_method("lightglue"),
        "loftr": _homography_method("loftr"),
        "apap": _local_method("apap", apap_matcher),
        "multi_h": _local_method("multi_h", apap_matcher),
    }


DEFAULT_METHODS = ["opencv", "opencv_scans", "orb", "sift", "asift", "lightglue", "loftr", "apap", "multi_h"]


def stitch_opencv(left, right):
    return build_methods()["opencv"](left, right)


def stitch_orb(left, right):
    return build_methods()["orb"](left, right)


def stitch_all(left: np.ndarray, right: np.ndarray, methods: Optional[list[str]] = None,
               apap_matcher: str = "sift") -> list[StitchResult]:
    table = build_methods(apap_matcher)
    names = [m for m in (methods or DEFAULT_METHODS)
             if m not in ("lightglue", "loftr") or matcher_available(m)]
    return [table[m](left, right) for m in names]


def stitch_dir(d: pathlib.Path, methods: Optional[list[str]] = None, apap_matcher: str = "sift",
               height: int = 640) -> tuple[np.ndarray, np.ndarray, list[StitchResult]]:
    """Stitch d/left.png + d/right.png; writes stitch_/matches_<method>.png, compare.png, stitch_results.json."""
    left = cv2.imread(str(d / "left.png"), cv2.IMREAD_GRAYSCALE)
    right = cv2.imread(str(d / "right.png"), cv2.IMREAD_GRAYSCALE)
    results = stitch_all(left, right, methods, apap_matcher)
    for r in results:
        for kind, img in (("stitch", r.image if r.ok else None), ("matches", r.matches)):
            out = d / f"{kind}_{r.method}.png"
            if img is not None:
                cv2.imwrite(str(out), img)
            elif out.exists():
                out.unlink()
    # Merge with methods run earlier (a run with --methods only replaces those), in DEFAULT_METHODS order.
    rows = {r["method"]: r for r in _load_results(d)}
    rows.update({r.method: {"method": r.method, "ok": r.ok, "ms": round(r.ms, 1), **r.detail} for r in results})
    ordered = [rows[m] for m in DEFAULT_METHODS if m in rows]
    (d / "stitch_results.json").write_text(json.dumps(ordered, indent=2), encoding="utf-8")
    fresh = {r.method: r for r in results}
    everything = [fresh.get(row["method"]) or _result_from_disk(d, row) for row in ordered]
    cv2.imwrite(str(d / "compare.png"), compose(d.name, left, right, everything, height))
    return left, right, results


def _load_results(d: pathlib.Path) -> list[dict]:
    f = d / "stitch_results.json"
    return json.loads(f.read_text(encoding="utf-8")) if f.exists() else []


def _result_from_disk(d: pathlib.Path, row: dict) -> StitchResult:
    img = cv2.imread(str(d / f"stitch_{row['method']}.png"), cv2.IMREAD_GRAYSCALE) if row["ok"] else None
    detail = {k: v for k, v in row.items() if k not in ("method", "ok", "ms")}
    return StitchResult(row["method"], row["ok"] and img is not None, img, row["ms"], detail)


# ---------------------------------------------------------------- viewer: originals left, results right

WINDOW = "vuebuds stitch"


def _fit(img: np.ndarray, w: int, h: int) -> np.ndarray:
    """img scaled to fit inside w x h, on a black w x h BGR tile (top-left aligned)."""
    tile = np.zeros((h, w, 3), np.uint8)
    scale = min(w / img.shape[1], h / img.shape[0])
    size = (max(1, round(img.shape[1] * scale)), max(1, round(img.shape[0] * scale)))
    small = cv2.resize(img, size, interpolation=cv2.INTER_AREA if scale < 1 else cv2.INTER_LINEAR)
    tile[:size[1], :size[0]] = cv2.cvtColor(small, cv2.COLOR_GRAY2BGR) if small.ndim == 2 else small
    return tile


def _label(img: np.ndarray, text: str, y: int = 24) -> np.ndarray:
    """Green text on a darkened box (a thick black outline drifts: Hershey glyphs widen with thickness)."""
    (w, h), base = cv2.getTextSize(text, cv2.FONT_HERSHEY_SIMPLEX, 0.6, 1)
    x0, y0, x1, y1 = 4, max(0, y - h - 4), min(img.shape[1], 12 + w), min(img.shape[0], y + base + 2)
    img[y0:y1, x0:x1] = (img[y0:y1, x0:x1] * 0.35).astype(np.uint8)
    cv2.putText(img, text, (8, y), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (80, 255, 80), 1, cv2.LINE_AA)
    return img


def compose(name: str, left: np.ndarray, right: np.ndarray, results: list[StitchResult], h: int) -> np.ndarray:
    """Left: left | right originals, h high. Right: a grid of the stitch results (3 columns)."""
    cell_w = round(left.shape[1] * h / left.shape[0])
    originals = np.hstack([_label(_fit(left, cell_w, h), "left"), _label(_fit(right, cell_w, h), "right")])
    cols = min(3, max(1, len(results)))
    rows = math.ceil(len(results) / cols)
    tile_h = h // rows
    tile_w = round(tile_h * 1.25)
    tiles = []
    for r in results:
        if r.ok:
            tile = _label(_fit(r.image, tile_w, tile_h), f"{r.method} {r.ms:.0f} ms", y=20)
        else:
            reason = r.detail.get("reason") or r.detail.get("status")
            tile = _label(np.zeros((tile_h, tile_w, 3), np.uint8), f"{r.method}: {reason}", y=20)
        cv2.rectangle(tile, (0, 0), (tile_w - 1, tile_h - 1), (90, 90, 90), 1)
        tiles.append(tile)
    while len(tiles) < rows * cols:
        tiles.append(np.zeros((tile_h, tile_w, 3), np.uint8))
    grid = np.vstack([np.hstack(tiles[i * cols:(i + 1) * cols]) for i in range(rows)])
    grid = cv2.copyMakeBorder(grid, 0, h - grid.shape[0], 0, 0, cv2.BORDER_CONSTANT)
    view = np.hstack([originals, np.full((h, 6, 3), 255, np.uint8), grid])
    return _label(view, name, y=h - 12)


def show(dirs: list[pathlib.Path], height: int, methods, apap_matcher) -> None:
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
            left, right, results = stitch_dir(d, methods, apap_matcher, height)
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


def write_set_files(set_dir: pathlib.Path, rows: list[dict], methods: list[str]):
    with open(set_dir / "stitch_summary.csv", "w", newline="", encoding="utf-8-sig") as f:
        keys = ["pair", "method", "ok", "ms", "reason", "status", "keypoints", "matches", "inliers", "used",
                "planes", "size", "matcher", "device", "sides"]
        w = csv.DictWriter(f, fieldnames=keys, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)
    # Manual scores: never touch existing rows, only append (pair, method) rows not there yet.
    score = set_dir / "stitch_score.csv"
    have = set()
    if score.exists():
        with open(score, newline="", encoding="utf-8-sig") as f:
            have = {(r["pair"], r["method"]) for r in csv.DictReader(f)}
    new = [r for r in rows if (r["pair"], r["method"]) not in have]
    if new:
        with open(score, "a", newline="", encoding="utf-8-sig" if not have else "utf-8") as f:
            w = csv.writer(f)
            if not have:
                w.writerow(["pair", "method", "score_1to5", "note"])
            for r in new:
                w.writerow([r["pair"], r["method"], "", ""])


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", type=pathlib.Path,
                    help="a pair directory (left.png / right.png), or a directory of them")
    ap.add_argument("--methods", nargs="+", choices=DEFAULT_METHODS, help="default: all available")
    ap.add_argument("--apap-matcher", default="sift", choices=sorted(MATCHERS), help="matches used by apap / multi_h")
    ap.add_argument("--no-show", action="store_true", help="only stitch and write files, no window")
    ap.add_argument("--height", type=int, default=640, help="window / compare.png height (pixels)")
    args = ap.parse_args(argv)
    for m in ("lightglue", "loftr"):
        if (args.methods is None or m in args.methods) and not matcher_available(m):
            print(f"note: {m} skipped (torch / {'lightglue' if m == 'lightglue' else 'kornia'} not installed)")
    dirs = []
    for a in args.paths:
        dirs += [a] if (a / "left.png").exists() else sorted(p.parent for p in a.glob("*/left.png"))
    if not dirs:
        print("no left.png / right.png found")
        return 1
    if not args.no_show:
        show(dirs, args.height, args.methods, args.apap_matcher)
        return 0
    ok = {}
    for d in dirs:
        _, _, results = stitch_dir(d, args.methods, args.apap_matcher, args.height)
        print(f"{d.name}: " + " | ".join(f"{r.method} {'ok' if r.ok else 'x'}" for r in results), flush=True)
        for r in results:
            ok[r.method] = ok.get(r.method, 0) + r.ok
    print(f"{len(dirs)} pairs, ok: " + ", ".join(f"{m} {n}" for m, n in ok.items()))
    # Set files cover every method run so far on each pair, not just this run's.
    for set_dir in {d.parent for d in dirs}:
        rows = [{"pair": p.name, **row} for p in sorted(x.parent for x in set_dir.glob("*/stitch_results.json"))
                for row in _load_results(p)]
        write_set_files(set_dir, rows, list(ok))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
