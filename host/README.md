# host/:电脑端工具

通过 BLE 实时接收 vuebuds 固件(设备名 `mustard`)发来的 HM01B0 图像,并在窗口里显示。

双目图片 / 拼接 / VLM 的两个交互 DEMO：运行 `demo.ps1` 或 `python demo.py`，详见 [DEMO.md](DEMO.md)。

## 安装

```bash
cd host
python3 -m venv .venv            # Python ≥ 3.10;Windows: python -m venv .venv
.venv/bin/python -m pip install -r requirements.txt    # Windows: .\.venv\Scripts\python.exe -m pip ...
```

- J-Link 工具(`--rtt`、`--reset`):读环境变量 `JLINK_DIR`,没有就用 `/Applications/SEGGER/JLink`(macOS)
  或 `C:\Program Files\SEGGER\JLink`(Windows);复位用 `nrfjprog`(有就用)或 `nrfutil device`。代码在 `../tools/devtools.py`。
- **macOS 直连要先认板子**:CoreBluetooth 不给 BLE 地址,bleak 只能用本机给每个外设编的 UUID。
  `python ble_identify.py` 依次通过 J-Link 暂停每块 5340 板的网络核几秒,看哪个 `mustard` 停止广播,
  结果写到 `boards.local.json`(gitignore,每台 Mac 自己的)。`--board` / `stereo.py` / `ble_bench.py` 直连时
  从这里查 UUID;Windows / Linux 照旧按地址连。经桥(`--bridge`)不受影响,桥按地址连板子。

## 运行

```powershell
.\.venv\Scripts\python.exe viewer.py                  # 实时预览
.\.venv\Scripts\python.exe viewer.py --rtt --reset    # 同时抓取固件 RTT 日志(先复位板子,从开机开始记录)
.\.venv\Scripts\python.exe viewer.py --duration 30 --snapshot shot.png   # 30 秒后自动退出,并保存最后一帧画面
```

- 窗口右上角显示实时 FPS(取最近 5 帧的平均值)和当前帧的尺寸与字节数。超过 5 秒没有收到新帧时,会显示 `(stalled)`。
- 窗口标题显示连接状态、累计帧数和累计丢包数。
- 按 **空格** 保存当前帧到 `captures/frame-<时间>.png`:原始 324×239 灰度图,不带叠加文字。保存后画面上会短暂提示 `saved ...`。
- 按 `q` 或 `Esc` 退出。固件断开连接后会自动复位,viewer 会自动重连。
- 使用 `--rtt` 前,请先关闭 JLinkRTTViewer,否则两者会抢同一个 RTT 通道。

## 日志与排查

每次运行都会把日志写到 `logs/session-<时间>.log`:`[host]` 是电脑端的日志,`[fw]` 是固件 RTT 日志,每行开头都是电脑端的接收时间。

| 现象 | `[fw]` | `[host]` | 问题在哪边 |
|---|---|---|---|
| 没有画面 | 没有 `image start` | 没有 `frame #` | 摄像头或接线 |
| 没有画面 | 有 `IMAGE READ DONE` 和 `sent NkB` | 没有 `frame #` | BLE 或电脑端 |
| 花屏 | 正常 | 有 `seq gap` 或 `lost>0` | BLE 链路丢包 |
| 花屏 | `IMAGE READ DONE` 字节数不对 | 正常 | 采集时序(SPIS/CS) |

注意:固件在流模式下,只有事件队列非空时才输出日志,而且每发 1 KB 就打一行 `sent NkB`。所以 `[fw]` 日志会有明显延迟,也可能丢失一部分。对照两边时间时,请以每条固件日志开头的**固件毫秒时间戳**为准,不要看它到达电脑的时间。
