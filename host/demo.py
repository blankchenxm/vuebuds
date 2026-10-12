"""Two local Gradio demos: saved stereo comparisons and the live camera-to-VLM chain.

python demo.py [--captures PATH] [--env-file PATH] [--bridge-port COM25] [--port 7860]
"""
import argparse
import json
import pathlib
import time

import cv2
import gradio as gr

import demo_core as core
import demo_vlm as vlm
import stitch

SOURCES = [("Original pair", "pair"), ("Stitched image", "stitch")]
TASKS = [("A · Describe scene / read text", "A"), ("B · Identify held / touched / pointed objects", "B")]


def image_view(**kwargs):
    return gr.Image(format="png", buttons=["fullscreen", "download"], **kwargs)


def seconds(value):
    return "—" if value is None else f"{value:.3f} s"


def stats_text(stats):
    return (f"{stats.get('model', '')} · First token **{seconds(stats.get('ttft_s'))}** · "
            f"VLM total **{seconds(stats.get('total_s'))}** · "
            f"Input / output tokens: {stats.get('prompt_tokens') or '—'} / {stats.get('completion_tokens') or '—'}"
            + (" · Output token limit reached; answer may be incomplete" if stats.get("finish_reason") == "length" else ""))


def stitch_text(result, cached=False):
    return (f"{'Cached · original compute time' if cached else 'Compute time'}: {result.ms:.1f} ms\n"
            f"{result.method} · {'Success' if result.ok else 'failed'}\n"
            + json.dumps(result.detail, ensure_ascii=False, indent=2))


class Demo:
    def __init__(self, captures, camera):
        self.captures, self.camera = pathlib.Path(captures), camera

    def refresh(self):
        names = list(core.datasets(self.captures))
        default = next((n for n in names if "QVGA" in n), names[0] if names else None)
        return gr.update(choices=names, value=default)

    def choose_dataset(self, name):
        pairs = core.datasets(self.captures).get(name, [])
        return gr.update(choices=[p.name for p in pairs], value=pairs[0].name if pairs else None)

    def offline(self, dataset, pair_name, method, force=False):
        try:
            pair = core.resolve_pair(self.captures, dataset, pair_name)
            left, right = (core.read_image(pair / f"{s}.png") for s in ("left", "right"))
            result, cached = core.cached_stitch(pair, method, force)
            state = {"key": [dataset, pair_name, method], "image": result.image if result.ok else None}
            matches = cv2.cvtColor(result.matches, cv2.COLOR_BGR2RGB) if result.matches is not None else None
            caption = f"{pair.name} · Left {left.shape[1]}×{left.shape[0]} / Right {right.shape[1]}×{right.shape[0]} (rotated to camera orientation)"
            return left, right, caption, result.image, matches, stitch_text(result, cached), state, "", ""
        except Exception as exc:
            return None, None, str(exc), None, None, "No stitched image yet", {}, "", ""

    def offline_answer(self, dataset, pair_name, method, state, model, task, source):
        shown_prompt = vlm.prompt_text(task, source)
        answer = ""
        yield shown_prompt, answer, "Sending the selected images and prompt…"
        try:
            pair = core.resolve_pair(self.captures, dataset, pair_name)
            if source == "stitch":
                if state.get("key") != [dataset, pair_name, method] or state.get("image") is None:
                    raise ValueError("No valid stitch for this pair and method. Stitch first or select Original pair.")
                images = [state["image"]]
            else:
                images = [core.read_image(pair / f"{s}.png") for s in ("left", "right")]
            for answer, stats in vlm.stream_answer(model, task, source, images):
                yield shown_prompt, answer, stats_text(stats)
        except Exception as exc:
            yield shown_prompt, answer, f"Request failed: {exc}"

    def live(self, mode, fmt, exposure, model, task, source, learned):
        clicked = time.monotonic()
        # Full snapshot on each yield keeps partial successes visible on downstream errors.
        out = [None, None, "", None, "", vlm.prompt_text(task, source), "", "", "", ""]
        record = {"mode": mode, "format": fmt, "exposure": exposure, "model": model,
                  "task": task, "requested_source": source}
        saved = None
        try:
            out[8] = "Preparing capture…"
            yield tuple(out)
            for event in self.camera.capture(mode, fmt, exposure, clicked):
                if isinstance(event, str):
                    out[2] = event
                    yield tuple(out)
                else:
                    images, info = event
            out[0], out[1] = images["left"], images["right"]
            capture_s = max(info["frame_done_s"].values())
            setup_s = max(info["click_to_frame_s"].values()) - capture_s
            delta = info["capture_delta_us"]
            out[2] = "\n".join(
                f"{s}: request → received {info['frame_done_s'][s]:.3f} s; click → received {info['click_to_frame_s'][s]:.3f} s; "
                f"{info['size'][s][0]}×{info['size'][s][1]} / {info['format'][s]}"
                + (f" q{info['quality'][s]}" if info['format'][s] == 'jpeg' else '')
                + f" / {info['bytes'][s]:,} B / lost packets {info['lost_packets'][s]}"
                for s in ("left", "right"))
            out[2] += f"\nArrival difference: {info['arrival_delta_ms']:.1f} ms\nCapture time difference: " + (
                "Not measured (simultaneous commands; board clocks are not synchronized)" if delta is None else f"{delta} µs (shared time base)")
            saved = core.save_capture(self.captures, mode, images, info)
            out[9] = f"Saved: {saved}. Select Refresh datasets in Offline studio to view."
            out[4] = "Stitching automatically…"
            out[8] = f"Connect / configure / warm up {setup_s:.3f} s → Capture & transfer {capture_s:.3f} s → Stitching…"
            yield tuple(out)
            result, attempts, stitching_s = core.auto_stitch(images["left"], images["right"], learned)
            out[3] = result.image if result else None
            out[4] = f"Total stitching time {stitching_s:.3f} s\n" + "\n".join(attempts)
            actual_source = source if result or source == "pair" else "pair"
            if result:
                core.write_image(saved / f"stitch_{result.method}.png", result.image)
                (saved / "stitch_results.json").write_text(json.dumps([
                    {"method": result.method, "ok": True, "ms": result.ms, **result.detail}], indent=2), encoding="utf-8")
            else:
                out[4] += "\nAll candidates failed; VLM will use the original pair."
            out[5] = vlm.prompt_text(task, actual_source)
            out[7] = "Waiting for the first token…"
            prefix = f"Connect / configure / warm up {setup_s:.3f} s → Capture & transfer {capture_s:.3f} s → Stitch {stitching_s:.3f} s"
            out[8] = prefix + " → Waiting for first token"
            yield tuple(out)
            record.update(capture=info, setup_s=setup_s, capture_s=capture_s, stitch_s=stitching_s,
                          stitch_attempts=attempts, actual_source=actual_source, prompt=out[5])
            payload = [result.image] if actual_source == "stitch" else [images["left"], images["right"]]
            for answer, stats in vlm.stream_answer(model, task, actual_source, payload):
                out[6], out[7] = answer, stats_text(stats)
                out[8] = (prefix + f" → VLM first token {seconds(stats['ttft_s'])} / total {seconds(stats['total_s'])}"
                          f" · Elapsed since click {time.monotonic() - clicked:.3f} s")
                record.update(answer=answer, vlm=stats, click_total_s=time.monotonic() - clicked)
                yield tuple(out)
        except Exception as exc:
            record["error"] = f"{type(exc).__name__}: {exc}"
            out[8] = f"Incomplete: {exc}"
            yield tuple(out)
        finally:
            if saved is not None:
                (saved / "demo_result.json").write_text(json.dumps(record, indent=2, ensure_ascii=False), encoding="utf-8")

    def flash(self, mode):
        try:
            yield from self.camera.flash(mode)
        except Exception as exc:
            yield f"Flashing incomplete: {exc}"

    def build(self):
        names = list(core.datasets(self.captures))
        default = next((n for n in names if "QVGA" in n), names[0] if names else None)
        pairs = core.datasets(self.captures).get(default, [])
        with gr.Blocks(title="VueBuds · Stereo Vision Studio", analytics_enabled=False) as app:
            gr.Markdown("VUEBUDS / VISION LAB", elem_classes="studio-brand")
            gr.Markdown("# Stereo Vision Studio\nExplore two perspectives. Compare stitching methods, then let a vision model interpret the scene.", elem_classes="studio-hero")
            with gr.Tab("DEMO 1 · Offline studio"):
                state = gr.State({})
                with gr.Row(elem_classes="studio-grid"):
                    with gr.Column(scale=1, min_width=380, elem_classes="studio-panel"):
                        gr.Markdown("### 01 / Source images")
                        dataset = gr.Dropdown(names, value=default, label="Dataset")
                        pair = gr.Dropdown([p.name for p in pairs], value=pairs[0].name if pairs else None, label="Image pair")
                        refresh = gr.Button("Refresh datasets")
                        with gr.Row():
                            left = image_view(label="Left camera", interactive=False, height=360)
                            right = image_view(label="Right camera", interactive=False, height=360)
                        caption = gr.Textbox(label="Image details", interactive=False, lines=2)
                    with gr.Column(scale=1, min_width=380, elem_classes="studio-panel"):
                        gr.Markdown("### 02 / Stitch & compare")
                        method = gr.Dropdown(stitch.DEFAULT_METHODS, value="sift", label="Stitching method")
                        recalc = gr.Button("Recompute stitch")
                        pano = image_view(label="Stitched image", interactive=False, height=360)
                        detail = gr.Textbox(label="Stitch status / inliers / timing", interactive=False, lines=5)
                        with gr.Accordion("Inspect feature matches", open=False):
                            matches = image_view(label="Feature matches", interactive=False)
                    with gr.Column(scale=1, min_width=380, elem_classes="studio-panel"):
                        gr.Markdown("### 03 / Visual understanding")
                        source = gr.Radio(SOURCES, value="pair", label="Image input")
                        model = gr.Dropdown(list(vlm.MODELS), value="qwen2.5-vl-7b", label="Vision model")
                        task = gr.Radio(TASKS, value="A", label="Task")
                        prompt = gr.Textbox(vlm.prompt_text("A", "pair"), label="Prompt preview", lines=8, interactive=False)
                        infer = gr.Button("Run vision model", variant="primary")
                        answer = gr.Textbox(label="Model response · streaming", lines=12, interactive=False)
                        stats = gr.Markdown()
                # Serialize all state-changing offline events and GPU work. A VLM request also validates
                # dataset/pair/method against its stored stitch, so queued changes cannot send stale images.
                common = dict(concurrency_id="demo", concurrency_limit=1)
                ins = [dataset, pair, method]
                outs = [left, right, caption, pano, matches, detail, state, answer, stats]
                app.load(self.offline, ins, outs, **common)
                refresh.click(self.refresh, outputs=dataset, **common)
                dataset.change(self.choose_dataset, dataset, pair, **common)
                pair.change(self.offline, ins, outs, **common)
                method.change(self.offline, ins, outs, **common)
                recalc.click(lambda d, p, m: self.offline(d, p, m, True), ins, outs, **common)
                for component in (source, task, model):
                    component.change(lambda t, s: (vlm.prompt_text(t, s), "", ""), [task, source],
                                     [prompt, answer, stats], **common)
                infer.click(self.offline_answer, [dataset, pair, method, state, model, task, source],
                            [prompt, answer, stats], **common)
            with gr.Tab("DEMO 2 · Live capture"):
                gr.Markdown("Connect the bridge via **nRF USB** and power both cameras. To change resolution, flash both boards using the panel below.", elem_classes="studio-note")
                with gr.Row():
                    mode = gr.Dropdown(list(core.MODES), value="QVGA", label="Capture resolution")
                    fmt = gr.Dropdown(list(core.FORMATS), value="jpeg:85", label="Transfer format")
                    exposure = gr.Radio([("Automatic", "auto"), ("Right follows previous left exposure", "sync")], value="auto", label="Exposure")
                    live_model = gr.Dropdown(list(vlm.MODELS), value="qwen2.5-vl-7b", label="Vision model")
                with gr.Row():
                    live_task = gr.Radio(TASKS, value="B", label="Task")
                    live_source = gr.Radio(SOURCES, value="stitch", label="Image input")
                    learned = gr.Checkbox(False, label="Try LightGlue / LoFTR if CPU methods fail")
                with gr.Accordion("Camera firmware · change resolution", open=False):
                    gr.Markdown("Build and flash the selected resolution to both nRF5340 boards. Allow about 3 minutes and keep USB connected.")
                    flash = gr.Button("Flash both cameras")
                    flash_log = gr.Textbox(label="Firmware progress", lines=8, interactive=False)
                capture = gr.Button("Capture → Stitch → Understand", variant="primary", elem_classes="studio-capture")
                with gr.Row(elem_classes="studio-grid"):
                    with gr.Column(scale=1, min_width=380, elem_classes="studio-panel"):
                        gr.Markdown("### 01 / Camera pair")
                        with gr.Row():
                            live_left = image_view(label="Left camera · live", interactive=False, height=360)
                            live_right = image_view(label="Right camera · live", interactive=False, height=360)
                        timing = gr.Textbox(label="Transfer timing / format / size", lines=6, interactive=False)
                    with gr.Column(scale=1, min_width=380, elem_classes="studio-panel"):
                        gr.Markdown("### 02 / Automatic stitch")
                        live_pano = image_view(label="Automatic stitch", height=360, interactive=False)
                        stitch_log = gr.Textbox(label="Method selection / timing", lines=6, interactive=False)
                        gr.Markdown("SIFT → ASIFT → ORB. Uses the first result that passes inlier and geometry checks. If all fail, the model receives the original pair.")
                    with gr.Column(scale=1, min_width=380, elem_classes="studio-panel"):
                        gr.Markdown("### 03 / Visual understanding")
                        live_prompt = gr.Textbox(vlm.prompt_text("B", "stitch"), label="Prompt preview", lines=8, interactive=False)
                        live_answer = gr.Textbox(label="Model response · streaming", lines=12, interactive=False)
                        live_stats = gr.Markdown()
                timeline = gr.Markdown("Ready to capture. Initial connection and warm-up are timed separately.", elem_classes="studio-status")
                saved = gr.Textbox(label="Saved capture", interactive=False)
                live_outputs = [live_left, live_right, timing, live_pano, stitch_log, live_prompt,
                                live_answer, live_stats, timeline, saved]
                capture.click(self.live, [mode, fmt, exposure, live_model, live_task, live_source, learned],
                              live_outputs, **common)
                flash.click(self.flash, mode, flash_log, **common)
                for component in (mode, fmt, exposure, live_model, live_task, live_source, learned):
                    component.change(lambda t, s: (None, None, "", None, "", vlm.prompt_text(t, s),
                                                   "", "", "Settings changed. Capture a new pair to continue.", ""),
                                     [live_task, live_source], live_outputs, **common)
            gr.Markdown(f"Capture directory: `{self.captures}` · Images are sent to the model at native resolution.", elem_classes="studio-footer")
        return app.queue(default_concurrency_limit=1, max_size=16)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--captures", type=pathlib.Path, default=core.default_captures())
    ap.add_argument("--env-file", type=pathlib.Path)
    ap.add_argument("--bridge-port", help="Automatically detects the bridge USB serial port by default")
    ap.add_argument("--port", type=int, default=7860)
    ap.add_argument("--no-browser", action="store_true")
    args = ap.parse_args()
    env = args.env_file or next((p / ".env" for p in core.worktree_hosts() if (p / ".env").is_file()), None)
    vlm.load_env(env)
    camera = core.Camera(args.bridge_port)
    try:
        Demo(args.captures.resolve(), camera).build().launch(
            server_name="127.0.0.1", server_port=args.port, share=False, inbrowser=not args.no_browser,
            theme=gr.themes.Soft(primary_hue="teal", neutral_hue="slate", text_size="lg", spacing_size="lg", font=["Segoe UI", "Arial", "sans-serif"]),
            footer_links=[],
            css=pathlib.Path(__file__).with_suffix(".css").read_text(encoding="utf-8"))
    finally:
        camera.close()


if __name__ == "__main__":
    main()
