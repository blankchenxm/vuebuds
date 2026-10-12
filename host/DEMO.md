# 双目视觉 DEMO

一个本地 Gradio 网页，两个标签页：离线比较，以及双板拍摄 → 自动拼接 → VLM。

## 启动

在 Windows PowerShell 中执行（可从任意目录运行）：

```powershell
& 'D:\Projects\vuebuds_wt\host\demo.ps1'
```

浏览器打开 <http://127.0.0.1:7860>。启动器优先使用当前 worktree 的 `host/.venv`，否则使用同仓库其他 worktree 的环境。
启动后保持终端运行；`Ctrl+C` 关闭服务和桥连接。

新环境：

```powershell
cd host
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe demo.py
```

可选参数：

```powershell
.\demo.ps1 -Captures 'D:\my-data\stereo' -EnvFile 'D:\my-config\.env' -BridgePort COM25 -Port 7860
# 或直接 Python
python demo.py --captures 'D:\my-data\stereo' --env-file 'D:\my-config\.env' --bridge-port COM25 --no-browser
```

未指定数据目录时，自动寻找当前 Git worktree 或同仓库其他 worktree 中的现有 QVGA 数据集。
未指定配置文件时，使用当前 worktree 或同仓库其他 worktree 的第一个 `host/.env`。只读取环境变量配置，不复制或显示密钥。

## DEMO 1：离线比较

1. 选择数据集，再选图片对。现有数据是 **QVGA 和 VGA**，不是 QQVGA。两张图已按相机方向旋转，不会再次旋转。
2. 选择拼接方法。成功的历史结果会直接加载并标明“缓存 / 原计算耗时”；没有成功缓存时现场计算。可点击“重新计算当前方法”。“查看匹配连线”展开匹配图。
3. 选择 **左右两张原图** 或 **拼接结果**、VLM 模型、任务 A/B。上面的提示词随输入方式和任务切换，点击“运行 VLM”发起新请求，回答会逐步显示。

拼接失败时，离线页不会偷偷用上一对图或另一种方法的结果。可选其他方法，或显式选择原图。
重算结果保存在当前网页会话中，不覆盖以前的评估文件。

数据格式：

```text
captures/stereo/
  20261009_QVGA/
    pair-001/left.png
    pair-001/right.png
    pair-001/stitch_results.json    # 可选，原 stitch.py 格式
    pair-001/stitch_sift.png        # 可选
    pair-001/matches_sift.png       # 可选
  20261009_VGA/...
```

直接位于根目录下的图片对归入“未分组拍摄”。只有左右图都存在的目录才会显示。

## DEMO 2：实时链路

- 使用 **BLE-USB 桥**（52840 DK 的 nRF USB 口），双板为 `hm0360-5340` 和 `hm0360b-5340`。自动查找桥串口，也可传 `--bridge-port`。
- 先选择 QVGA/VGA。改变分辨率后展开“切换分辨率 · 双板烧录”，点击烧录按钮；界面显示两块板的编译/烧录进度。复用 `dk5340.ps1`，需要已有 NCS/J-Link 环境和两块板的调试 USB 连接。
- 压缩支持 raw、PNG、JPEG q95/90/85/75/60，使用现有板端编码器；曝光为自动或右板跟随左板上一对曝光。
- 选择 VLM、任务、图像输入，点击“拍摄 → 自动拼接 → VLM”。三栏依次更新，左侧先显示实拍，中间拼接完成后立即显示，右侧逐字输出回答。
- 首次连接会释放桥原有 BLE 连接并重新连接双板，清理以前程序遗留的推流/拍照请求；之后保持连接。首次连接/设置变更先预热；同步曝光用预热左图的值设置右板。预热不算入“正式拍照请求→收齐”，但计入“点击→收齐”和总时间。
- 自动拼接顺序 **SIFT → ASIFT → ORB**，采用首个通过现有几何和内点检查的结果。这是已约定的实时启发式，并非客观画质排名；全部失败则自动送左右原图。可勾选在 CPU 方法失败后继续尝试 LightGlue/LoFTR。
- 每次成功收图保存到 `demo_YYYYMMDD_QVGA|VGA/<时间>/`，含 `left.png`、`right.png`、`info.json`，以及成功拼接和 `demo_result.json`。后者记录提示词、回答、模型和各段耗时。离线页刷新即可看到。

## 耗时口径

| 指标 | 含义 |
|---|---|
| 请求→收齐（每板） | 正式拍照命令发出前，到该板完整图像在电脑端组装完毕 |
| 点击→收齐（每板） | 点击处理开始后到收图；含连接、配置、预热 |
| 收齐时间差 | 左右图在电脑端完成接收的时间差，不是拍摄同步误差 |
| 实际拍摄时间差 | 只有两帧都携带共同时间基准标志才计算，并处理 32 位回绕；当前方案 A 显示未测 |
| 拼接 | 所有自动候选的累计实际耗时，包含失败尝试 |
| VLM 首字（TTFT） | 从准备 PNG/发请求，到收到第一个非空回答片段；不包括模型内部思考内容 |
| VLM 总耗时 | 相同起点到流结束；模型冷加载、排队、网络时间都包含在内 |
| 点击总时间 | 此次处理开始到 VLM 完成，含连接、预热、图片保存和各阶段开销 |

Gradio 队列等待发生在处理开始前，不包含在上述点击计时中。界面串行处理推理、拼接和硬件操作，避免同时占用 GPU 或双板。

## 模型与可选拼接

| 显示名称 | 后端 / 模型 |
|---|---|
| qwen2.5-vl-7b | 本地 Ollama / `qwen2.5vl:7b` |
| minicpm-v4.5 | 本地 Ollama / `minicpm-v4.5:8b` |
| gemma4-e4b | 本地 Ollama / `gemma4:e4b` |
| qwen3-vl-8b | OpenRouter / `qwen/qwen3-vl-8b-instruct` |
| qwen3.5-9b | OpenRouter / `qwen/qwen3.5-9b` |

本地默认 `http://localhost:11434`，可用 `OLLAMA_HOST` 修改。远端模型使用环境变量或 `.env` 中的 `OPENROUTER_API_KEY`。选 OpenRouter 时图片会发往该服务；本地 Ollama 使用配置的 Ollama 地址。
提示词复用已有 A/B 评估定义；拼接图使用单图版本。图片以原生分辨率 PNG 编码发送。回答受 2000-token 上限限制，达到上限会标注；不自动重试付费模型调用。

LightGlue/LoFTR 是可选依赖，安装命令见 `requirements.txt`，首次使用可能下载模型权重。未安装时界面显示原因，不影响其他方法。

## 故障恢复

- 找不到桥 / 端口被占用：检查 nRF USB 线，退出其他 `stereo.py` / `bridge.py` / DEMO 进程。
- 收图超时 / 丢包 / 断连：本次请求失败，关闭连接，保留已显示的错误；解决连接后再次拍摄。若固件不一致，重新烧录双板。
- 实际尺寸或格式与所选不同：不会将错误设置标成成功；分辨率需烧录，压缩失败应检查 PSRAM/固件。
- 烧录一板成功、一板失败：继续阻止拍摄，直到两板重新烧录成功。烧录开始后不要断电，浏览器断开不会主动中止正在进行的烧录命令。
- VLM 服务未启动/模型未安装/密钥缺失/中途断流：显示错误；已收到的部分文本及实时图片仍保留，可修复后重试。

## 软件回归

```powershell
cd host
python -m unittest -v test_demo
```

测试不连接硬件、不调用真实模型、不使用密钥。覆盖 SSE/NDJSON、提示词与图片数量、流中错误/截断、旧拼接结果隔离、自动回退、数据发现、共同时间基准和保存结果。

流式/队列接口依据 [Gradio Blocks](https://gradio.app/docs/gradio/blocks) 和 [Ollama API](https://github.com/ollama/ollama/blob/main/docs/api.md)。

## 本机验收（2026-10-12，PR #47）

- 13 项软件回归通过；浏览器验证数据集切换、缓存/重算、输入/任务提示词切换及真实流式回答。
- 5 个 VLM 均完成真实调用；9 种拼接方法在现有 QVGA/VGA 照片上运行，成功和失败状态均正常呈现；LightGlue/LoFTR 使用 CUDA。
- 双板 QVGA/VGA 烧录通过；右板 raw、PNG、JPEG q95/90/85/75/60 全部正确返回，0 丢包。
- 以下两次在浏览器点击按钮完成整条链路，图像及结果保存成功，能在离线页打开：

| 实拍配置 | 左 / 右收齐 | 拼接 | VLM 首字 / 完成 |
|---|---|---|---|
| QVGA JPEG q85，自动曝光 | 0.376 / 0.349 s，0 丢包 | 场景特征不足，回退双原图 | Qwen 2.764 / 6.584 s |
| VGA JPEG q85，右板跟随曝光 | 1.183 / 1.505 s，0 丢包 | SIFT 71 ms，70 内点；发送一张拼接图 | Qwen 7.429 / 39.950 s |

这是功能验收的单次耗时，受场景、冷加载和回答长度影响，不代表性能基准。VGA 帧头验证左右曝光均为 169/0/256，右板 manual 标志有效。时钟同步方案 E 未合入，实际拍摄时差仍显示未测。仓库未配置 CI。
