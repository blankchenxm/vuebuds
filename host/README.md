# host/:电脑端工具

通过 BLE 实时接收 vuebuds 固件(设备名 `mustard`)发来的 HM01B0 图像,并在窗口里显示。

## 安装

```powershell
cd host
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
```

## 运行

```powershell
.\.venv\Scripts\python.exe viewer.py                  # 实时预览
.\.venv\Scripts\python.exe viewer.py --rtt --reset    # 同时抓取固件 RTT 日志(先复位板子,从开机开始记录)
.\.venv\Scripts\python.exe viewer.py --duration 30 --snapshot shot.png   # 30 秒后自动退出,并保存最后一帧画面
```

- 窗口右上角显示实时 FPS(取最近 5 帧的平均值)和当前帧的尺寸与字节数。超过 5 秒没有收到新帧时,会显示 `(stalled)`。
- 窗口标题显示连接状态、累计帧数和累计丢包数。
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
