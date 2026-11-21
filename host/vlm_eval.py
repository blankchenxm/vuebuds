"""VLM understanding of the two-board pictures: how much can a VLM read at QVGA vs VGA?

Each request carries the left and right pictures of one pair (Image 1 = left camera,
Image 2 = right camera), sent at native resolution, with an English prompt:
  A  describe everything in as much detail as possible, without guessing (every pair)
  B  which object is the user holding / pointing at, and its features (pairs with a hand,
     listed in <set>/eval_config.json as "task_b")

  python vlm_eval.py captures/stereo/20261009_QVGA --models qwen3-vl-8b qwen3.5-9b --task A B
  python vlm_eval.py captures/stereo/20261009_VGA --models all --task A
  python vlm_eval.py --list                     # known models and backends

Single-picture sets (a phone photo per object, no left / right): each item directory holds
image.png instead of left.png / right.png; task A then uses SYSTEM_SINGLE / TASK_A_SINGLE.
  python vlm_eval.py captures/20261009_phone_eval/gray_640 --models qwen2.5-vl-7b --task A

Results: <pair>/vlm/<model>_<task>.json (prompt, raw answer, parsed JSON, time, tokens)
and <set>/vlm_summary.csv. Existing results are skipped unless --force.
OpenRouter key: OPENROUTER_API_KEY in the environment or in host/.env (gitignored).
Ollama: a local server at OLLAMA_HOST (default http://localhost:11434).
"""
import argparse
import base64
import concurrent.futures
import csv
import datetime
import json
import os
import pathlib
import re
import sys
import time
import urllib.error
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent

# name -> (backend, model id, extra request fields)
MODELS = {
    "qwen2.5-vl-7b": ("ollama", "qwen2.5vl:7b", {}),          # the paper's pick; OpenRouter only has 72B
    "qwen3-vl-8b": ("openrouter", "qwen/qwen3-vl-8b-instruct", {}),
    "qwen3.5-9b": ("openrouter", "qwen/qwen3.5-9b", {"reasoning": {"enabled": False}}),
    "minicpm-v4.5": ("ollama", "minicpm-v4.5:8b", {}),
    "gemma4-e4b": ("ollama", "gemma4:e4b", {"think": False}),  # thinks by default: ate the token budget
    "moondream3": ("moondream", "moondream3", {}),            # not on OpenRouter / Ollama: not wired yet
}

SYSTEM = (
    "You will receive two grayscale, low-resolution photos taken at the same moment by two small "
    "cameras worn at the user's left and right ears. Image 1 is from the LEFT camera, Image 2 is "
    "from the RIGHT camera. Answer only from what is actually visible in the images. Do not guess, "
    "infer brand names from typical packaging, or complete partially visible words from memory. "
    "If text cannot be read, write [unreadable]; if only part of it can be read, give only the "
    "readable characters and mark the rest with [?]."
)

TASK_A = """Describe the scene in as much detail as possible.

1. Objects: for every object, say what it is, where it is in each image, its shape, packaging and how light or dark it is.
2. Text: transcribe ALL text you can read, character by character. For each piece of text give the object and the position on it, its size (large heading / medium / small print) and your certainty (clear / partial / guess).
3. Background and surroundings.
4. What each image shows that the other does not.

After the description, output one JSON block in this format:
```json
{"objects": [{"name": "...", "image": "left|right|both", "position": "...", "details": "..."}],
 "texts": [{"text": "...", "object": "...", "size": "large|medium|small", "certainty": "clear|partial|guess"}],
 "background": "..."}
```"""

SYSTEM_SINGLE = (
    "You will receive one photo. Answer only from what is actually visible in it. Do not guess, "
    "infer brand names from typical packaging, or complete partially visible words from memory. "
    "If text cannot be read, write [unreadable]; if only part of it can be read, give only the "
    "readable characters and mark the rest with [?]."
)

TASK_A_SINGLE = """Describe the scene in as much detail as possible.

1. Objects: for every object, say what it is, where it is in the image, its shape, packaging and colors (or how light or dark it is).
2. Text: transcribe ALL text you can read, character by character. For each piece of text give the object and the position on it, its size (large heading / medium / small print) and your certainty (clear / partial / guess).
3. Background and surroundings.

After the description, output one JSON block in this format:
```json
{"objects": [{"name": "...", "position": "...", "details": "..."}],
 "texts": [{"text": "...", "object": "...", "size": "large|medium|small", "certainty": "clear|partial|guess"}],
 "background": "..."}
```"""

TASK_B = """In these images a person's hand is holding, touching or pointing at one of the objects.

1. Which object is the user holding / touching / pointing at? Which action is it (hold, touch, point)?
2. What is your evidence (hand position, finger direction, contact point)?
3. Describe that object in as much detail as possible: category, brand, all readable text, shape, size compared with the other objects, and its position in each image.
4. List the other objects in the scene.
5. How certain are you, and is there any ambiguity?

After the answer, output one JSON block in this format:
```json
{"target": {"name": "...", "action": "hold|touch|point", "evidence": "...", "features": ["..."], "texts": ["..."]},
 "other_objects": ["..."],
 "confidence": "high|medium|low", "ambiguity": "..."}
```"""

TASKS = {"A": TASK_A, "B": TASK_B}
MAX_TOKENS = 2000
TIMEOUT_S = 600  # a local Ollama on a laptop can take minutes per answer
RETRIES = 6


def load_env():
    env = HERE / ".env"
    if env.exists():
        for line in env.read_text(encoding="utf-8").splitlines():
            if "=" in line and not line.lstrip().startswith("#"):
                k, v = line.split("=", 1)
                os.environ.setdefault(k.strip(), v.strip())


def b64(path: pathlib.Path) -> str:
    return base64.b64encode(path.read_bytes()).decode("ascii")


def post_json(url: str, body: dict, headers: dict) -> dict:
    req = urllib.request.Request(url, json.dumps(body).encode("utf-8"), {"Content-Type": "application/json", **headers})
    with urllib.request.urlopen(req, timeout=TIMEOUT_S) as r:
        return json.loads(r.read().decode("utf-8"))


def ask_openrouter(model: str, extra: dict, prompt: str, images: list[pathlib.Path], system: str = SYSTEM) -> dict:
    key = os.environ.get("OPENROUTER_API_KEY")
    if not key:
        raise RuntimeError("OPENROUTER_API_KEY not set (environment or host/.env)")
    content = [{"type": "text", "text": prompt}]
    for i, p in enumerate(images, 1):
        if len(images) > 1:
            content.append({"type": "text", "text": f"Image {i} ({'left' if i == 1 else 'right'} camera):"})
        content.append({"type": "image_url", "image_url": {"url": f"data:image/png;base64,{b64(p)}"}})
    body = {"model": model, "temperature": 0, "max_tokens": MAX_TOKENS,
            "messages": [{"role": "system", "content": system}, {"role": "user", "content": content}], **extra}
    r = post_json("https://openrouter.ai/api/v1/chat/completions", body,
                  {"Authorization": f"Bearer {key}", "X-Title": "vuebuds-vlm-eval"})
    if "error" in r:
        raise RuntimeError(f"OpenRouter error: {r['error']}")
    choice = r["choices"][0]
    return {"answer": choice["message"].get("content") or "", "finish_reason": choice.get("finish_reason"),
            "usage": r.get("usage", {}), "provider": r.get("provider"), "model_returned": r.get("model")}


def ask_ollama(model: str, extra: dict, prompt: str, images: list[pathlib.Path], system: str = SYSTEM) -> dict:
    host = os.environ.get("OLLAMA_HOST", "http://localhost:11434").rstrip("/")
    if not host.startswith("http"):
        host = "http://" + host
    text = prompt + ("\n\nImage 1 is the left camera, Image 2 is the right camera (in that order)."
                     if len(images) > 1 else "")
    # num_ctx: Ollama's default 4096 is too small for two images + prompt + a 2000-token answer.
    # repeat_penalty: at temperature ~0 the local models fall into loops ("ALMOND ALMOND ..." until
    # the token limit); 1.15 stops that (2026-10-11). The OpenRouter models run without it.
    options = {"temperature": 0, "num_predict": MAX_TOKENS, "num_ctx": 8192, "repeat_penalty": 1.15}
    body = {"model": model, "stream": False, "options": options,
            "messages": [{"role": "system", "content": system},
                         {"role": "user", "content": text, "images": [b64(p) for p in images]}], **extra}
    r = post_json(f"{host}/api/chat", body, {})
    return {"answer": r["message"]["content"], "finish_reason": r.get("done_reason"),
            "usage": {"prompt_tokens": r.get("prompt_eval_count"), "completion_tokens": r.get("eval_count")},
            "provider": "ollama-local", "model_returned": r.get("model")}


BACKENDS = {"openrouter": ask_openrouter, "ollama": ask_ollama}


def parse_json(answer: str):
    """The last ```json block (or the last {...} span) of the answer, or None."""
    blocks = re.findall(r"```(?:json)?\s*(\{.*?\})\s*```", answer, re.S)
    candidates = blocks[::-1] or [answer[answer.find("{"): answer.rfind("}") + 1]]
    for c in candidates:
        try:
            return json.loads(c)
        except (json.JSONDecodeError, ValueError):
            continue
    return None


def run_one(pair: pathlib.Path, name: str, task: str, force: bool) -> dict:
    out = pair / "vlm" / f"{name}_{task}.json"
    if out.exists() and not force:
        return json.loads(out.read_text(encoding="utf-8"))
    backend, model, extra = MODELS[name]
    if backend not in BACKENDS:
        raise RuntimeError(f"{name}: backend '{backend}' not wired up")
    single = (pair / "image.png").exists()
    images = [pair / "image.png"] if single else [pair / "left.png", pair / "right.png"]
    system, prompt = (SYSTEM_SINGLE, TASK_A_SINGLE) if single else (SYSTEM, TASKS[task])
    if single and task != "A":
        raise RuntimeError("single-picture items only have task A")
    t0 = time.perf_counter()
    last = None
    for attempt in range(RETRIES):
        try:
            r = BACKENDS[backend](model, extra, prompt, images, system)
            break
        except (urllib.error.URLError, RuntimeError, KeyError, TimeoutError) as e:
            last = e
            limited = isinstance(e, urllib.error.HTTPError) and e.code == 429
            # Upstream rate limits (429) clear after a minute or two: back off much longer.
            wait = min(30 * 2 ** attempt, 300) if limited else 5 * (attempt + 1)
            print(f"  {pair.name} {name} {task}: attempt {attempt + 1} failed: {e}; retry in {wait} s")
            time.sleep(wait)
    else:
        raise RuntimeError(f"{name} {task} on {pair.name} failed: {last}")
    result = {
        "pair": pair.name, "set": pair.parent.name, "model": name, "backend": backend, "model_id": model,
        "task": task, "time": datetime.datetime.now().isoformat(timespec="seconds"),
        "seconds": round(time.perf_counter() - t0, 2), "system": system, "prompt": prompt,
        "temperature": 0, "max_tokens": MAX_TOKENS, "repeat_penalty": 1.15 if backend == "ollama" else None,
        "extra": extra, **r, "parsed": parse_json(r["answer"]),
    }
    out.parent.mkdir(exist_ok=True)
    out.write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    return result


def write_summary(set_dir: pathlib.Path):
    rows = []
    for f in sorted(set_dir.glob("*/vlm/*.json")):
        r = json.loads(f.read_text(encoding="utf-8"))
        p = r.get("parsed") or {}
        target = p.get("target") or {} if isinstance(p, dict) else {}
        rows.append({
            "pair": r["pair"], "model": r["model"], "task": r["task"], "seconds": r["seconds"],
            "prompt_tokens": (r.get("usage") or {}).get("prompt_tokens"),
            "completion_tokens": (r.get("usage") or {}).get("completion_tokens"),
            "finish_reason": r.get("finish_reason"), "json_ok": bool(r.get("parsed")),
            "n_objects": len(p.get("objects", [])) if isinstance(p, dict) else "",
            "n_texts": len(p.get("texts", [])) if isinstance(p, dict) else "",
            "texts": " | ".join(str(t.get("text", "")) for t in p.get("texts", []) if isinstance(t, dict)) if isinstance(p, dict) else "",
            "target": target.get("name", "") if isinstance(target, dict) else "",
            "action": target.get("action", "") if isinstance(target, dict) else "",
            "confidence": p.get("confidence", "") if isinstance(p, dict) else "",
        })
    if rows:
        with open(set_dir / "vlm_summary.csv", "w", newline="", encoding="utf-8-sig") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0]))
            w.writeheader()
            w.writerows(rows)
    return len(rows)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("set_dir", nargs="?", type=pathlib.Path, help="e.g. captures/stereo/20261009_QVGA")
    ap.add_argument("--models", nargs="+", default=["all"], help="model names (see --list) or 'all'")
    ap.add_argument("--task", nargs="+", default=["A", "B"], choices=sorted(TASKS))
    ap.add_argument("--pairs", nargs="+", help="only these pair directory names")
    ap.add_argument("--force", action="store_true", help="redo existing results")
    ap.add_argument("--jobs", type=int, default=6, help="requests in flight at once")
    ap.add_argument("--list", action="store_true", help="list the known models")
    args = ap.parse_args(argv)
    if args.list or args.set_dir is None:
        for n, (b, m, _) in MODELS.items():
            print(f"{n:15s} {b:11s} {m}{'' if b in BACKENDS else '   (backend not wired up)'}")
        return 0
    load_env()
    names = list(MODELS) if args.models == ["all"] else args.models
    unknown = [n for n in names if n not in MODELS]
    if unknown:
        print(f"unknown models: {unknown}; see --list")
        return 1
    config = json.loads((args.set_dir / "eval_config.json").read_text(encoding="utf-8")) \
        if (args.set_dir / "eval_config.json").exists() else {}
    pairs = sorted({p.parent for p in [*args.set_dir.glob("*/left.png"), *args.set_dir.glob("*/image.png")]})
    if args.pairs:
        pairs = [p for p in pairs if p.name in args.pairs]
    jobs = [(pair, name, task) for task in args.task for name in names
            for pair in (pairs if task == "A" else [p for p in pairs if p.name in config.get("task_b", [])])]

    def work(job):
        pair, name, task = job
        try:
            r = run_one(pair, name, task, args.force)
            return True, (f"{pair.name} {name:13s} {task}: {r['seconds']:6.1f} s, "
                          f"{(r.get('usage') or {}).get('completion_tokens')} tokens, json {'ok' if r.get('parsed') else 'MISSING'}")
        except Exception as e:  # keep going: one model / pair failing should not stop the run
            return False, f"{pair.name} {name:13s} {task}: FAILED {e}"

    # API requests run in parallel; a local Ollama server works through them one at a time anyway.
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for ok, line in pool.map(work, jobs):
            failed += not ok
            print(line, flush=True)
    print(f"summary rows: {write_summary(args.set_dir)} -> {args.set_dir / 'vlm_summary.csv'}; failed {failed}")
    return 0 if failed == 0 else 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
