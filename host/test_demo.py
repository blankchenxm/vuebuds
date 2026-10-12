"""Deterministic regressions; no camera, model server, key, or network required."""
import io
import json
import pathlib
import tempfile
import time
import types
import unittest
from unittest.mock import patch

import numpy as np

import demo
import demo_core as core
import demo_vlm as vlm
import protocol
import stitch
import stereo


class StreamTests(unittest.TestCase):
    def test_ollama_stream_prompt_images_usage_and_ttft(self):
        messages = [
            {"message": {"thinking": "invisible reasoning"}},
            {"message": {"content": "你"}},
            {"message": {"content": "好"}},
            {"done": True, "eval_count": 2, "prompt_eval_count": 500, "done_reason": "stop"}]
        data = ("\n".join(json.dumps(m) for m in messages) + "\n").encode()
        with patch("urllib.request.urlopen", return_value=io.BytesIO(data)) as request:
            results = list(vlm.stream_answer("qwen2.5-vl-7b", "B", "pair", [np.zeros((8, 8), np.uint8)] * 2))
            body = json.loads(request.call_args.args[0].data)
        self.assertEqual(results[-1][0], "你好")
        self.assertIsNotNone(results[0][1]["ttft_s"])
        self.assertIsNone(results[0][1]["total_s"])  # yielded stats are snapshots
        self.assertEqual(results[-1][1]["completion_tokens"], 2)
        self.assertEqual(body["messages"][1]["content"], vlm.prompts("B", "pair")[1])
        self.assertEqual(len(body["messages"][1]["images"]), 2)

    def test_openrouter_comments_usage_and_done(self):
        data = b': keepalive\n\ndata: {"choices":[{"delta":{"content":"hello"}}]}\n\ndata: {"choices":[],"usage":{"prompt_tokens":20,"completion_tokens":1}}\n\ndata: [DONE]\n'
        with patch.dict("os.environ", {"OPENROUTER_API_KEY": "test-only"}), patch(
                "urllib.request.urlopen", return_value=io.BytesIO(data)) as request:
            results = list(vlm.stream_answer("qwen3-vl-8b", "A", "stitch", [np.zeros((8, 8), np.uint8)]))
            body = json.loads(request.call_args.args[0].data)
        self.assertEqual(results[-1][0], "hello")
        self.assertEqual(results[-1][1]["prompt_tokens"], 20)
        self.assertEqual(len(body["messages"][1]["content"]), 2)
        self.assertEqual(body["messages"][1]["content"][0]["text"], vlm.prompts("A", "stitch")[1])

    def test_error_and_truncated_streams_do_not_succeed(self):
        for data in (b'{"error":"model missing"}\n', b'{"message":{"content":"partial"}}\n', b'{"done":true}\n'):
            with self.subTest(data=data), patch("urllib.request.urlopen", return_value=io.BytesIO(data)):
                with self.assertRaises(RuntimeError):
                    list(vlm.stream_answer("qwen2.5-vl-7b", "A", "stitch", [np.zeros((8, 8), np.uint8)]))


class DemoTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.pair = self.root / "QVGA" / "pair01"
        self.pair.mkdir(parents=True)
        self.img = np.zeros((32, 24), np.uint8)
        for side in ("left", "right"):
            core.write_image(self.pair / f"{side}.png", self.img)

    def tearDown(self):
        self.temp.cleanup()

    def test_discovery_excludes_incomplete_pair_and_path_escape(self):
        bad = self.root / "QVGA" / "bad"
        bad.mkdir()
        core.write_image(bad / "left.png", self.img)
        self.assertEqual(core.datasets(self.root)["QVGA"], [self.pair])
        with self.assertRaises(ValueError):
            core.resolve_pair(self.root, "QVGA", "../outside")

    def test_cached_success_and_force_recompute(self):
        core.write_image(self.pair / "stitch_sift.png", self.img)
        (self.pair / "stitch_results.json").write_text(json.dumps([{"method": "sift", "ok": True, "ms": 13}]))
        with patch.object(core, "run_stitch") as run:
            result, cached = core.cached_stitch(self.pair, "sift")
            self.assertTrue(cached)
            self.assertEqual(result.ms, 13)
            run.assert_not_called()
            core.cached_stitch(self.pair, "sift", True)
            run.assert_called_once()

    def test_auto_first_valid_and_all_failed_fallback(self):
        def run(l, r, method):
            return stitch.StitchResult(method, method == "asift", self.img if method == "asift" else None, 1)
        with patch.object(core, "run_stitch", side_effect=run) as call:
            result, attempts, elapsed = core.auto_stitch(self.img, self.img)
        self.assertEqual(result.method, "asift")
        self.assertEqual(call.call_count, 2)
        with patch.object(core, "run_stitch", return_value=stitch.StitchResult("x", False, None, 1)):
            self.assertIsNone(core.auto_stitch(self.img, self.img)[0])

    def test_stale_stitch_is_never_sent(self):
        app = demo.Demo(self.root, None)
        with patch.object(vlm, "stream_answer") as stream:
            result = list(app.offline_answer("QVGA", "pair01", "sift", {"key": ["QVGA", "other", "sift"],
                "image": self.img}, "qwen2.5-vl-7b", "A", "stitch"))
        stream.assert_not_called()
        self.assertIn("failed", result[-1][2])

    def test_capture_clock_only_compared_when_common_and_wrap_safe(self):
        pair = stereo.Pair(1, False)
        for side, capture in (("left", 2**32 - 20), ("right", 30)):
            pair.frames[side] = protocol.Frame(1, 24, 32, self.img.tobytes(), 3, 0, 0.2,
                                              time.monotonic(), capture_us=capture)
        self.assertIsNone(core.pair_info(pair, pair.t_request)["capture_delta_us"])
        for f in pair.frames.values():
            f.header_flags = protocol.HEADER_COMMON_CLOCK
        self.assertEqual(core.pair_info(pair, pair.t_request)["capture_delta_us"], 50)

    def test_live_failed_stitch_sends_pair_and_saves_result(self):
        images = {"left": self.img, "right": self.img}
        pair = stereo.Pair(1, False)
        for side in images:
            pair.frames[side] = protocol.Frame(1, 24, 32, self.img.tobytes(), 1, 0, .1, time.monotonic())
        info = core.pair_info(pair, pair.t_request)
        class FakeCamera:
            def capture(self, *args):
                yield "warmup"
                yield images, info
        app = demo.Demo(self.root, FakeCamera())
        stats = dict(ttft_s=.1, total_s=.3, prompt_tokens=1, completion_tokens=1)
        with patch.object(core, "auto_stitch", return_value=(None, ["failed"], .1)), patch.object(
                vlm, "stream_answer", return_value=iter([("answer", stats)])) as stream:
            final = list(app.live("QVGA", "raw", "auto", "qwen2.5-vl-7b", "B", "stitch", False))[-1]
        self.assertEqual(stream.call_args.args[2], "pair")
        self.assertEqual(len(stream.call_args.args[3]), 2)
        self.assertEqual(final[6], "answer")
        record = next(self.root.glob("demo_*/*/demo_result.json"))
        self.assertEqual(json.loads(record.read_text(encoding="utf-8"))["actual_source"], "pair")

    def test_camera_failure_closes_and_releases_lock(self):
        camera = core.Camera()
        camera.requires_flash = True
        with self.assertRaises(RuntimeError):
            list(camera.capture("QVGA", "raw", "auto", time.monotonic()))
        self.assertFalse(camera.lock.locked())
        self.assertIsNone(camera.stereo)

    def test_live_capture_error_replaces_stale_warmup_message(self):
        class FakeCamera:
            def capture(self, *args):
                yield "Warming up…"
                raise TimeoutError("Missing: left")
        final = list(demo.Demo(self.root, FakeCamera()).live(
            "VGA", "jpeg:85", "auto", "qwen2.5-vl-7b", "A", "pair", False))[-1]
        self.assertEqual(final[2], "Capture failed: Missing: left")
        self.assertEqual(final[8], "Incomplete: Missing: left")

    def test_live_success_sends_only_stitched_image(self):
        images = {"left": self.img, "right": self.img}
        pair = stereo.Pair(1, False)
        for side in images:
            pair.frames[side] = protocol.Frame(1, 24, 32, self.img.tobytes(), 1, 0, .1, time.monotonic())
        class FakeCamera:
            def capture(self, *args):
                yield images, core.pair_info(pair, pair.t_request)
        pano = np.ones((32, 40), np.uint8)
        result = stitch.StitchResult("sift", True, pano, 1, {"inliers": 50})
        with patch.object(core, "auto_stitch", return_value=(result, ["sift ok"], .1)), patch.object(
                vlm, "stream_answer", return_value=iter([("answer", dict(ttft_s=.1, total_s=.2))])) as stream:
            final = list(demo.Demo(self.root, FakeCamera()).live(
                "QVGA", "raw", "auto", "qwen2.5-vl-7b", "A", "stitch", False))[-1]
        self.assertEqual(stream.call_args.args[2], "stitch")
        self.assertEqual(len(stream.call_args.args[3]), 1)
        np.testing.assert_array_equal(stream.call_args.args[3][0], pano)
        self.assertEqual(final[5], vlm.prompt_text("A", "stitch"))
        saved = next(self.root.glob("demo_*/*/stitch_sift.png"))
        np.testing.assert_array_equal(core.read_image(saved), pano)

    def test_error_cleanup_drops_ble_links_before_closing_serial(self):
        from unittest.mock import Mock
        camera = core.Camera()
        st = Mock()
        camera.stereo = st
        with patch.object(core.time, "sleep"):
            camera.close(reset=True)
        st.bridge.configure.assert_called_once_with([])
        st.close.assert_called_once()
        self.assertIsNone(camera.stereo)

    def test_bridge_write_error_fails_without_waiting_for_frame_timeout(self):
        from collections import deque
        camera = core.Camera()
        link = types.SimpleNamespace(write_results=[])
        def trigger(**kwargs):
            link.write_results.append((time.monotonic(), 0, -107))
            return True
        camera.stereo = types.SimpleNamespace(done=deque(), trigger=trigger, bridge=link, pending=None)
        try:
            with self.assertRaisesRegex(RuntimeError, "Bridge failed to forward"):
                camera._take()
        finally:
            camera.stereo = None


if __name__ == "__main__":
    unittest.main()
