"""Score vlm_eval.py answers against <set>/truth.json (rough, automatic; read the answers too).

  python vlm_score.py captures/stereo/20261009_QVGA captures/stereo/20261009_VGA

Per answer:
  task A  text recall by print size: share of the truth words (of the objects in that pair)
          found anywhere in the answer, for large / medium / small text
          invented words: words the model lists as read text (parsed JSON "texts") that appear
          in no truth text of any object in the set, i.e. likely made up
          objects: share of the pair's truth objects mentioned (by keyword)
  task B  target correct: the JSON target names the truth object (by keyword) and no other
Writes <set>/vlm_scores.csv and prints a model x set table.

Text size: every truth text with a height (cap_px_vga / cap_px_phone_full in truth.json) is
scored per answer as read (>= 60 % of its words in the answer) or not, with its cap height in
pixels in that picture: VGA sets cap_px_vga, QVGA half of it, phone sets (captures/*_phone_eval/
<variant>_<width>) cap_px_phone_full * width / 5712. Writes <set>/vlm_text_hits.csv and prints
the read rate per model and source by pixel height.
"""
import csv
import json
import pathlib
import re
import sys
from collections import defaultdict
from typing import Callable, Optional

# Keywords that identify each truth object in an answer (lower case).
KEYWORDS = {
    "crackers": ["cracker", "sourdough"],
    "crackers_phone": ["cracker", "sourdough"],  # the box's other face (phone photo)
    "almond": ["almond", "kirkland"],
    "antacid": ["antacid"],
    "ramen": ["ramen", "noodle"],
    "krylon": ["krylon", "uv-resistant", "uv resistant", "clear coat", "matte"],
    "supershield": ["super shield", "supershield", "mg chemical", "nickel"],
    "multimeter": ["multimeter", "meter"],
    "book": ["book", "gawande", "being mortal"],
}


def words(text: str) -> list[str]:
    return [w for w in re.findall(r"[a-z0-9%]+", text.lower()) if len(w) >= 2 or w.isdigit()]


def mentions(text: str, obj: str) -> bool:
    t = text.lower()
    return any(k in t for k in KEYWORDS.get(obj, []))


def score_answer(r: dict, truth: dict) -> dict:
    pair = truth["pairs"][r["pair"]]
    answer = r.get("answer") or ""
    parsed = r.get("parsed") if isinstance(r.get("parsed"), dict) else {}
    row = {"set": r["set"], "pair": r["pair"], "model": r["model"], "task": r["task"],
           "json_ok": bool(parsed), "seconds": r.get("seconds")}
    if r["task"] == "A":
        ans_words = set(words(answer))
        for size in ("large", "medium", "small"):
            tw = [w for o in pair["objects"] for t in truth["objects"][o]["texts"]
                  if t["size"] == size and not t["text"].startswith("(") for w in words(t["text"])]
            row[f"recall_{size}"] = round(sum(w in ans_words for w in tw) / len(tw), 2) if tw else ""
        vocab = {w for o in truth["objects"].values() for t in o["texts"] for w in words(t["text"])}
        read = [w for t in parsed.get("texts", []) if isinstance(t, dict) for w in words(str(t.get("text", "")))]
        invented = sorted({w for w in read if w not in vocab and not w.isdigit()})
        row["invented_words"] = len(invented)
        row["invented"] = " ".join(invented[:15])
        row["objects_found"] = round(sum(mentions(answer, o) for o in pair["objects"]) / len(pair["objects"]), 2)
    else:
        target = (parsed.get("target") or {}) if parsed else {}
        target = target if isinstance(target, dict) else {}
        truth_t = pair.get("target", {})
        # The target's name decides; its features / texts only when the name names no object
        # (features often compare the target with the other objects).
        hits = []
        for text in (str(target.get("name", "")), " ".join(str(target.get(k, "")) for k in ("features", "texts"))):
            hits = [o for o in pair["objects"] if mentions(text, o) and o != "book"]  # objects in this scene
            if hits:
                break
        row["truth_target"] = truth_t.get("object", "")
        row["model_target"] = (target.get("name", "") if isinstance(target, dict) else "")[:60]
        row["target_correct"] = hits == [truth_t.get("object")] or (truth_t.get("object") in hits and len(hits) == 1)
        row["truth_action"] = truth_t.get("action", "")
        row["model_action"] = target.get("action", "") if isinstance(target, dict) else ""
    return row


PHONE_FULL_W = 5712
HEIGHT_BINS = [0, 2, 3, 4, 6, 8, 12, 20, 1e9]


def source_of(set_dir: pathlib.Path) -> tuple[str, Callable[[dict], Optional[float]]]:
    """(source label, text -> cap height in px in this set's pictures, or None)."""
    name = set_dir.name
    if name.endswith("_VGA") and not name.endswith("_QVGA"):
        return "HM0360 VGA", lambda t: t.get("cap_px_vga")
    if name.endswith("_QVGA"):
        return "HM0360 QVGA", lambda t: t["cap_px_vga"] / 2 if t.get("cap_px_vga") else None
    variant, _, width = name.rpartition("_")
    k = int(width) / PHONE_FULL_W
    return f"phone {name}", lambda t: t["cap_px_phone_full"] * k if t.get("cap_px_phone_full") else None


def text_hits(r: dict, truth: dict, height) -> list[dict]:
    if r["task"] != "A":
        return []
    ans_words = set(words(r.get("answer") or ""))
    rows = []
    for o in truth["pairs"][r["pair"]]["objects"]:
        for t in truth["objects"][o]["texts"]:
            h = height(t)
            tw = words(t["text"])
            if h is None or not tw:
                continue
            got = sum(w in ans_words for w in tw) / len(tw)
            rows.append({"pair": r["pair"], "model": r["model"], "object": o, "text": t["text"], "size": t["size"],
                         "cap_px": round(h, 1), "words_found": round(got, 2), "read": got >= 0.6})
    return rows


def bin_label(lo, hi):
    return f"<{hi:g}" if lo == 0 else (f">={lo:g}" if hi > 1e8 else f"{lo:g}-{hi:g}")


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__)
        return 1
    table = defaultdict(lambda: defaultdict(list))
    by_height = defaultdict(lambda: defaultdict(list))  # (model, source) -> bin -> [read]
    for set_dir in map(pathlib.Path, argv):
        truth = json.loads((set_dir / "truth.json").read_text(encoding="utf-8"))
        answers = [json.loads(f.read_text(encoding="utf-8")) for f in sorted(set_dir.glob("*/vlm/*.json"))]
        rows = [score_answer(r, truth) for r in answers]
        source, height = source_of(set_dir)
        hits = [h for r in answers for h in text_hits(r, truth, height)]
        if hits:
            with open(set_dir / "vlm_text_hits.csv", "w", newline="", encoding="utf-8-sig") as f:
                w = csv.DictWriter(f, fieldnames=list(hits[0]))
                w.writeheader()
                w.writerows(hits)
        for h in hits:
            for lo, hi in zip(HEIGHT_BINS, HEIGHT_BINS[1:]):
                if lo <= h["cap_px"] < hi:
                    by_height[(h["model"], source)][bin_label(lo, hi)].append(h["read"])
        keys = sorted({k for r in rows for k in r}, key=lambda k: list(rows[0]).index(k) if k in rows[0] else 99)
        with open(set_dir / "vlm_scores.csv", "w", newline="", encoding="utf-8-sig") as f:
            w = csv.DictWriter(f, fieldnames=keys)
            w.writeheader()
            w.writerows(rows)
        for r in rows:
            t = table[(r["model"], set_dir.name)]
            for k in ("recall_large", "recall_medium", "recall_small", "invented_words", "objects_found", "seconds"):
                if r.get(k) not in (None, ""):
                    t[f"{r['task']}:{k}"].append(float(r[k]))
            if r["task"] == "B":
                t["B:target_correct"].append(float(r["target_correct"]))
    cols = ["A:recall_large", "A:recall_medium", "A:recall_small", "A:invented_words", "A:objects_found",
            "B:target_correct", "A:seconds"]
    print(f"{'model':14s} {'set':15s} " + " ".join(f"{c[2:]:>15s}" for c in cols))
    for (model, s), t in sorted(table.items()):
        cells = []
        for c in cols:
            v = t.get(c, [])
            cells.append(f"{sum(v) / len(v):15.2f}" if v else f"{'-':>15s}")
        print(f"{model:14s} {s:15s} " + " ".join(cells))
    if by_height:
        labels = [bin_label(lo, hi) for lo, hi in zip(HEIGHT_BINS, HEIGHT_BINS[1:])]
        print("\ntexts read, by cap height in px in the picture (read / texts)")
        print(f"{'model':14s} {'source':22s} " + " ".join(f"{l:>8s}" for l in labels))
        for (model, source), bins in sorted(by_height.items()):
            cells = [f"{sum(bins[l])}/{len(bins[l])}" if bins.get(l) else "-" for l in labels]
            print(f"{model:14s} {source:22s} " + " ".join(f"{c:>8s}" for c in cells))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
