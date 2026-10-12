"""Streaming demo adapter; batch evaluation prompts/backends stay in vlm_eval.py."""
import base64
import json
import os
import pathlib
import time
import urllib.request

import cv2

import vlm_eval as vlm

MODELS = {k: v for k, v in vlm.MODELS.items() if v[0] in vlm.BACKENDS}
SYSTEM_STITCH = (
    "You will receive one grayscale, low-resolution photo stitched from two small cameras worn "
    "at the user's left and right ears. Seams or ghosting can appear in the overlap. "
    + vlm.SYSTEM_SINGLE.split("You will receive one photo. ", 1)[1]
)


def prompts(task, source):
    if task not in vlm.TASKS or source not in ("pair", "stitch"):
        raise ValueError("Unknown task or image source")
    if source == "stitch":
        prompt = vlm.TASK_A_SINGLE if task == "A" else vlm.TASK_B.replace(
            "In these images", "In this image").replace("in each image", "in the image")
        return SYSTEM_STITCH, prompt
    return vlm.SYSTEM, vlm.TASKS[task] + "\n\nImage 1 is the left camera; Image 2 is the right camera."


def prompt_text(task, source):
    system, user = prompts(task, source)
    return f"SYSTEM\n{system}\n\nUSER\n{user}"


def load_env(path):
    """Only load configuration from an explicitly selected local file; never log values."""
    if path and pathlib.Path(path).is_file():
        for line in pathlib.Path(path).read_text(encoding="utf-8-sig").splitlines():
            if "=" in line and not line.lstrip().startswith("#"):
                key, value = line.split("=", 1)
                os.environ.setdefault(key.strip(), value.strip().strip('\"').strip("'"))


def png_b64(image):
    ok, data = cv2.imencode(".png", image)
    if not ok:
        raise ValueError("Cannot encode VLM image")
    return base64.b64encode(data).decode("ascii")


def stream_answer(name, task, source, images, *, timeout=vlm.TIMEOUT_S):
    """Yield (accumulated text, immutable stats); reject failed/truncated streams.

    TTFT starts before PNG encoding/request and ends at first nonempty answer text.
    The prompt displayed in the demo is exactly the two text messages sent here.
    """
    backend, model, extra = MODELS[name]
    if len(images) != (2 if source == "pair" else 1):
        raise ValueError("Image count does not match the selected input")
    system, prompt = prompts(task, source)
    started = time.perf_counter()
    encoded = [png_b64(i) for i in images]
    stats = dict(ttft_s=None, total_s=None, prompt_tokens=None, completion_tokens=None,
                 finish_reason=None, model=name, source=source)
    headers = {"Content-Type": "application/json"}
    if backend == "ollama":
        host = os.environ.get("OLLAMA_HOST", "http://localhost:11434").rstrip("/")
        if not host.startswith(("http://", "https://")):
            host = "http://" + host
        url = host + "/api/chat"
        body = {"model": model, "stream": True,
                "options": {"temperature": 0, "num_predict": vlm.MAX_TOKENS,
                            "num_ctx": 8192, "repeat_penalty": 1.15},
                "messages": [{"role": "system", "content": system},
                             {"role": "user", "content": prompt, "images": encoded}], **extra}
    else:
        key = os.environ.get("OPENROUTER_API_KEY")
        if not key:
            raise RuntimeError("OPENROUTER_API_KEY is missing. Use --env-file or select a local Ollama model.")
        url = "https://openrouter.ai/api/v1/chat/completions"
        headers.update(Authorization=f"Bearer {key}", **{"X-Title": "vuebuds-demo"})
        content = [{"type": "text", "text": prompt}] + [
            {"type": "image_url", "image_url": {"url": f"data:image/png;base64,{im}"}} for im in encoded]
        body = {"model": model, "temperature": 0, "max_tokens": vlm.MAX_TOKENS, "stream": True,
                "stream_options": {"include_usage": True},
                "messages": [{"role": "system", "content": system},
                             {"role": "user", "content": content}], **extra}
    req = urllib.request.Request(url, json.dumps(body).encode("utf-8"), headers)
    answer, finished = "", False
    with urllib.request.urlopen(req, timeout=timeout) as response:
        for raw in response:
            if time.perf_counter() - started > timeout:
                raise TimeoutError("VLM request timed out")
            line = raw.decode("utf-8").strip()
            if not line:
                continue
            if backend == "openrouter":
                if not line.startswith("data:"):
                    continue
                line = line[5:].strip()
                if line == "[DONE]":
                    finished = True
                    break
            msg = json.loads(line)
            if "error" in msg:
                raise RuntimeError(f"VLM service error: {msg['error']}")
            if backend == "ollama":
                piece = msg.get("message", {}).get("content", "")
                if msg.get("done"):
                    finished = True
                    stats.update(prompt_tokens=msg.get("prompt_eval_count"),
                                 completion_tokens=msg.get("eval_count"), finish_reason=msg.get("done_reason"))
            else:
                usage = msg.get("usage") or {}
                if usage:
                    stats.update(prompt_tokens=usage.get("prompt_tokens"), completion_tokens=usage.get("completion_tokens"))
                piece = ""
                for choice in msg.get("choices", []):
                    piece += (choice.get("delta") or {}).get("content") or ""
                    if choice.get("finish_reason"):
                        stats["finish_reason"] = choice["finish_reason"]
            if piece:
                if stats["ttft_s"] is None:
                    stats["ttft_s"] = time.perf_counter() - started
                answer += piece
                yield answer, dict(stats, elapsed_s=time.perf_counter() - started)
            if backend == "ollama" and finished:
                break
    if not finished:
        raise RuntimeError("VLM stream interrupted. The displayed answer is incomplete; try again.")
    if not answer.strip():
        raise RuntimeError("VLM returned an empty answer.")
    stats["total_s"] = time.perf_counter() - started
    yield answer, stats.copy()
