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

SOURCES = [("左右两张原图", "pair"), ("拼接结果", "stitch")]
TASKS = [("A · 详细描述 / 读取文字", "A"), ("B · 手持 / 触碰 / 指向的物品", "B")]


def image_view(**kwargs):
    return gr.Image(format="png", buttons=["fullscreen", "download"], **kwargs)


def seconds(value):
    return "—" if value is None else f"{value:.3f} s"


def stats_text(stats):
    return (f"{stats.get('model', '')} · 首字延迟 **{seconds(stats.get('ttft_s'))}** · "
            f"VLM 总耗时 **{seconds(stats.get('total_s'))}** · "
            f"输入 / 输出 token：{stats.get('prompt_tokens') or '—'} / {stats.get('completion_tokens') or '—'}"
            + (" · 已达到输出 token 上限，回答可能未完" if stats.get("finish_reason") == "length" else ""))


def stitch_text(result, cached=False):
    return (f"{'缓存 · 原计算耗时' if cached else '本次计算耗时'}：{result.ms:.1f} ms\n"
            f"{result.method} · {'成功' if result.ok else '失败'}\n"
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
            caption = f"{pair.name} · 左 {left.shape[1]}×{left.shape[0]} / 右 {right.shape[1]}×{right.shape[0]}（已按安装方向旋转）"
            return left, right, caption, result.image, matches, stitch_text(result, cached), state, "", ""
        except Exception as exc:
            return None, None, str(exc), None, None, "尚无拼接结果", {}, "", ""

    def offline_answer(self, dataset, pair_name, method, state, model, task, source):
        shown_prompt = vlm.prompt_text(task, source)
        answer = ""
        yield shown_prompt, answer, "正在发送当前图片和提示词…"
        try:
            pair = core.resolve_pair(self.captures, dataset, pair_name)
            if source == "stitch":
                if state.get("key") != [dataset, pair_name, method] or state.get("image") is None:
                    raise ValueError("当前图片 / 方法没有成功的拼接结果，请先拼接或选择左右原图。")
                images = [state["image"]]
            else:
                images = [core.read_image(pair / f"{s}.png") for s in ("left", "right")]
            for answer, stats in vlm.stream_answer(model, task, source, images):
                yield shown_prompt, answer, stats_text(stats)
        except Exception as exc:
            yield shown_prompt, answer, f"本次请求失败：{exc}"

    def live(self, mode, fmt, exposure, model, task, source, learned):
        clicked = time.monotonic()
        # Full snapshot on each yield keeps partial successes visible on downstream errors.
        out = [None, None, "", None, "", vlm.prompt_text(task, source), "", "", "", ""]
        record = {"mode": mode, "format": fmt, "exposure": exposure, "model": model,
                  "task": task, "requested_source": source}
        saved = None
        try:
            out[8] = "正在准备拍摄…"
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
                f"{s}：请求→收齐 {info['frame_done_s'][s]:.3f} s；点击→收齐 {info['click_to_frame_s'][s]:.3f} s；"
                f"{info['size'][s][0]}×{info['size'][s][1]} / {info['format'][s]}"
                + (f" q{info['quality'][s]}" if info['format'][s] == 'jpeg' else '')
                + f" / {info['bytes'][s]:,} B / 丢包 {info['lost_packets'][s]}"
                for s in ("left", "right"))
            out[2] += f"\n收齐时间差：{info['arrival_delta_ms']:.1f} ms\n实际拍摄时间差：" + (
                "未测（当前使用同时下命令方案 A，板端时钟未同步）" if delta is None else f"{delta} µs（共同时间基准）")
            saved = core.save_capture(self.captures, mode, images, info)
            out[9] = f"本次图片已保存：{saved}。离线页点击「刷新数据集」可查看。"
            out[4] = "正在自动拼接…"
            out[8] = f"连接 / 配置 / 预热 {setup_s:.3f} s → 拍摄传输 {capture_s:.3f} s → 拼接中"
            yield tuple(out)
            result, attempts, stitching_s = core.auto_stitch(images["left"], images["right"], learned)
            out[3] = result.image if result else None
            out[4] = f"全部拼接尝试耗时 {stitching_s:.3f} s\n" + "\n".join(attempts)
            actual_source = source if result or source == "pair" else "pair"
            if result:
                core.write_image(saved / f"stitch_{result.method}.png", result.image)
                (saved / "stitch_results.json").write_text(json.dumps([
                    {"method": result.method, "ok": True, "ms": result.ms, **result.detail}], indent=2), encoding="utf-8")
            else:
                out[4] += "\n所有候选失败；VLM 使用左右两张原图。"
            out[5] = vlm.prompt_text(task, actual_source)
            out[7] = "等待模型首字…"
            prefix = f"连接 / 配置 / 预热 {setup_s:.3f} s → 拍摄传输 {capture_s:.3f} s → 拼接 {stitching_s:.3f} s"
            out[8] = prefix + " → VLM 等待首字"
            yield tuple(out)
            record.update(capture=info, setup_s=setup_s, capture_s=capture_s, stitch_s=stitching_s,
                          stitch_attempts=attempts, actual_source=actual_source, prompt=out[5])
            payload = [result.image] if actual_source == "stitch" else [images["left"], images["right"]]
            for answer, stats in vlm.stream_answer(model, task, actual_source, payload):
                out[6], out[7] = answer, stats_text(stats)
                out[8] = (prefix + f" → VLM 首字 {seconds(stats['ttft_s'])} / 完成 {seconds(stats['total_s'])}"
                          f" · 点击后已过 {time.monotonic() - clicked:.3f} s")
                record.update(answer=answer, vlm=stats, click_total_s=time.monotonic() - clicked)
                yield tuple(out)
        except Exception as exc:
            record["error"] = f"{type(exc).__name__}: {exc}"
            out[8] = f"未完成：{exc}"
            yield tuple(out)
        finally:
            if saved is not None:
                (saved / "demo_result.json").write_text(json.dumps(record, indent=2, ensure_ascii=False), encoding="utf-8")

    def flash(self, mode):
        try:
            yield from self.camera.flash(mode)
        except Exception as exc:
            yield f"烧录未完成：{exc}"

    def build(self):
        names = list(core.datasets(self.captures))
        default = next((n for n in names if "QVGA" in n), names[0] if names else None)
        pairs = core.datasets(self.captures).get(default, [])
        with gr.Blocks(title="VueBuds · 双目视觉 DEMO", analytics_enabled=False) as app:
            gr.Markdown("# VueBuds · 双目视觉实验台\n从左右相机到拼接与理解。离线比较方法，实时观察完整链路。")
            with gr.Tab("DEMO 1 · 离线比较"):
                state = gr.State({})
                with gr.Row():
                    with gr.Column(scale=1, min_width=280):
                        gr.Markdown("### 1 · 选择图片")
                        dataset = gr.Dropdown(names, value=default, label="数据集")
                        pair = gr.Dropdown([p.name for p in pairs], value=pairs[0].name if pairs else None, label="图片对")
                        refresh = gr.Button("刷新数据集")
                        with gr.Row():
                            left = image_view(label="左相机", interactive=False, height=280)
                            right = image_view(label="右相机", interactive=False, height=280)
                        caption = gr.Textbox(label="图片信息", interactive=False, lines=2)
                    with gr.Column(scale=1, min_width=280):
                        gr.Markdown("### 2 · 比较拼接")
                        method = gr.Dropdown(stitch.DEFAULT_METHODS, value="sift", label="拼接方法")
                        recalc = gr.Button("重新计算当前方法")
                        pano = image_view(label="拼接结果", interactive=False, height=280)
                        detail = gr.Textbox(label="拼接状态 / 内点 / 耗时", interactive=False, lines=5)
                        with gr.Accordion("查看匹配连线", open=False):
                            matches = image_view(label="匹配点", interactive=False)
                    with gr.Column(scale=1, min_width=320):
                        gr.Markdown("### 3 · 实时请求 VLM")
                        source = gr.Radio(SOURCES, value="pair", label="输入图像")
                        model = gr.Dropdown(list(vlm.MODELS), value="qwen2.5-vl-7b", label="VLM 模型")
                        task = gr.Radio(TASKS, value="A", label="任务")
                        prompt = gr.Textbox(vlm.prompt_text("A", "pair"), label="本次发送的提示词", lines=8, interactive=False)
                        infer = gr.Button("运行 VLM", variant="primary")
                        answer = gr.Textbox(label="VLM 回答（实时流式）", lines=12, interactive=False)
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
            with gr.Tab("DEMO 2 · 实时拍摄"):
                gr.Markdown("桥的 **nRF USB** 口和两块相机板需连接。分辨率由固件决定，改变后请烧录双板。")
                with gr.Row():
                    mode = gr.Dropdown(list(core.MODES), value="QVGA", label="拍摄分辨率")
                    fmt = gr.Dropdown(list(core.FORMATS), value="jpeg:85", label="板上传输格式")
                    exposure = gr.Radio([("自动曝光", "auto"), ("右板跟随左板上一对曝光", "sync")], value="auto", label="曝光")
                    live_model = gr.Dropdown(list(vlm.MODELS), value="qwen2.5-vl-7b", label="VLM 模型")
                with gr.Row():
                    live_task = gr.Radio(TASKS, value="B", label="任务")
                    live_source = gr.Radio(SOURCES, value="stitch", label="VLM 图像输入")
                    learned = gr.Checkbox(False, label="CPU 方法失败后追加 LightGlue / LoFTR")
                with gr.Accordion("切换分辨率 · 双板烧录", open=False):
                    gr.Markdown("此按钮会编译并烧录上方所选分辨率到两块 nRF5340；约 3 分钟。烧录期间请保持连接。")
                    flash = gr.Button("烧录所选分辨率到双板")
                    flash_log = gr.Textbox(label="烧录进度", lines=8, interactive=False)
                capture = gr.Button("拍摄 → 自动拼接 → VLM", variant="primary")
                with gr.Row():
                    with gr.Column(scale=1, min_width=280):
                        gr.Markdown("### 1 · 左右相机")
                        with gr.Row():
                            live_left = image_view(label="实时左图", interactive=False, height=280)
                            live_right = image_view(label="实时右图", interactive=False, height=280)
                        timing = gr.Textbox(label="收图耗时 / 格式 / 大小", lines=6, interactive=False)
                    with gr.Column(scale=1, min_width=280):
                        gr.Markdown("### 2 · 自动拼接")
                        live_pano = image_view(label="自动拼接结果", height=280, interactive=False)
                        stitch_log = gr.Textbox(label="选择过程 / 拼接耗时", lines=6, interactive=False)
                        gr.Markdown("SIFT → ASIFT → ORB：采用首个通过内点数与几何检查的结果；全部失败则将两张原图送给 VLM。")
                    with gr.Column(scale=1, min_width=320):
                        gr.Markdown("### 3 · 视觉理解")
                        live_prompt = gr.Textbox(vlm.prompt_text("B", "stitch"), label="实际发送的提示词", lines=8, interactive=False)
                        live_answer = gr.Textbox(label="实时 VLM 回答", lines=12, interactive=False)
                        live_stats = gr.Markdown()
                timeline = gr.Markdown("等待拍摄。首次连接及预热会单独计时。")
                saved = gr.Textbox(label="保存位置", interactive=False)
                live_outputs = [live_left, live_right, timing, live_pano, stitch_log, live_prompt,
                                live_answer, live_stats, timeline, saved]
                capture.click(self.live, [mode, fmt, exposure, live_model, live_task, live_source, learned],
                              live_outputs, **common)
                flash.click(self.flash, mode, flash_log, **common)
                for component in (mode, fmt, exposure, live_model, live_task, live_source, learned):
                    component.change(lambda t, s: (None, None, "", None, "", vlm.prompt_text(t, s),
                                                   "", "", "设置已改变，请重新拍摄。", ""),
                                     [live_task, live_source], live_outputs, **common)
            gr.Markdown(f"数据目录：`{self.captures}` · 所有图像按原生像素传入模型。")
        return app.queue(default_concurrency_limit=1, max_size=16)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--captures", type=pathlib.Path, default=core.default_captures())
    ap.add_argument("--env-file", type=pathlib.Path)
    ap.add_argument("--bridge-port", help="默认自动查找桥的 USB 串口")
    ap.add_argument("--port", type=int, default=7860)
    ap.add_argument("--no-browser", action="store_true")
    args = ap.parse_args()
    env = args.env_file or next((p / ".env" for p in core.worktree_hosts() if (p / ".env").is_file()), None)
    vlm.load_env(env)
    camera = core.Camera(args.bridge_port)
    try:
        Demo(args.captures.resolve(), camera).build().launch(
            server_name="127.0.0.1", server_port=args.port, share=False, inbrowser=not args.no_browser,
            theme=gr.themes.Soft(primary_hue="teal"))
    finally:
        camera.close()


if __name__ == "__main__":
    main()
