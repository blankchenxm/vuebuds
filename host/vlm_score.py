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
"""
import csv
import json
import pathlib
import re
import sys
from collections import defaultdict

# Keywords that identify each truth object in an answer (lower case).
KEYWORDS = {
    "crackers": ["cracker", "sourdough"],
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
            hits = [o for o in KEYWORDS if mentions(text, o) and o != "book"]
            if hits:
                break
        row["truth_target"] = truth_t.get("object", "")
        row["model_target"] = (target.get("name", "") if isinstance(target, dict) else "")[:60]
        row["target_correct"] = hits == [truth_t.get("object")] or (truth_t.get("object") in hits and len(hits) == 1)
        row["truth_action"] = truth_t.get("action", "")
        row["model_action"] = target.get("action", "") if isinstance(target, dict) else ""
    return row


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__)
        return 1
    table = defaultdict(lambda: defaultdict(list))
    for set_dir in map(pathlib.Path, argv):
        truth = json.loads((set_dir / "truth.json").read_text(encoding="utf-8"))
        rows = [score_answer(json.loads(f.read_text(encoding="utf-8")), truth)
                for f in sorted(set_dir.glob("*/vlm/*.json"))]
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
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
