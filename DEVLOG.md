# 开发日志

每个阶段(每个合并进 `main` 的 PR)在这里追加一条,**最新的在最上面**。格式见 `CLAUDE.md` 的"开发日志"一节。
设计和 TODO 见 Obsidian 笔记 `Vuebuds Hm0360迁移对比.md`。

---

## 2026-09-30 · 重构 HM01B0 采集链路:传感器驱动分层 + 帧缓冲池 + 发送时裁剪 + 帧头带宽高(PR #9,Issue #8)

- **阶段**:TODO 第 1 步(重构 HM01B0)
- **改了什么**:
  - 删除 `fw/HM01B0/`,拆成三层:
    - `fw/sensors/camera_sensor.{h,c}`:公共接口(`camera_mode_info_t`、按型号 ID 选驱动)。
    - `fw/sensors/hm01b0/`:按 ESP32 驱动结构组织(`include/`、`private_include/hm01b0_regs.h`、寄存器表 `hm01b0_modes.c`、`hm01b0_mode_info.c`、`hm01b0_reg.c`、`hm01b0_sensor.c`)。寄存器值逐条照搬原 `hm01b0_init_optimized_vlm()`。
    - `fw/capture/`:MCLK(`capture_mclk.c`)、帧缓冲池(`frame_pool.c`)、SPIS + 片选 + DMA 自动分段(`capture.c`)。
  - `fw/camera.c`:只做编排(检测传感器 → 配置 → 采集),对外给出标准区域的帧描述;`CAMERA_MODE` / `CAMERA_TEST_PATTERN` 为编译时宏。
  - `fw/ble_manager.c`:删掉 77 KB 的 `ringBuffer` 拷贝,`bleSendFrame()` 直接从槽里按标准区域逐行发送;帧开始包带宽、高(各 uint16 LE)。
  - `fw/ble_app_template_gcc_nrf52.ld`:帧缓冲池 = 堆尾到栈底的剩余 RAM。
  - `fw/main.c`:`idle()` 推流时也处理日志;`fw/cli.c` 跟着新接口改。
  - `host/protocol.py` 按帧头取宽高;`fw/tools/ble_receive.py` 改为复用它。
- **为什么**:笔记 §6 的重构方案;为 HM0360 和 Monitor 帧环形缓存做准备。
- **和原代码的行为差异(逐条)**:
  1. BLE 发出的是裁剪后的 320×240(传感器列 2–321、行 2–241),原来是 324×239。
  2. DMA 分段按模式信息自动计算:收第 1–243 行,分成 122 + 121 行。原来是 118 + 121 行,第 240–243 行因缓冲区满被丢掉。**片选时序完全保持原设计**(FVLD 后 TIMER4 延时 312 µs,有意跳过第 0 行)。
  3. BLE 协议:帧开始包多 4 字节宽高,去掉 flags bit1(高分辨率标志);旧版 viewer 不兼容。
  4. 段边界的片选脉冲宽度:原来是 32 个 NOP(约 0.5 µs),现在是 `nrf_delay_us(1)`。
  5. 型号 ID 读高低两个字节,不是 0x01B0 就中止摄像头初始化(原来只打日志、照常继续)。
  6. 寄存器写入:进入待机只写一次 `MODE_SELECT=0`(原来写两次);测试图寄存器 `0x0601` 改在 `0x3067`、`0x3062` 之后写(写入的值不变)。
  7. 日志:推流时也输出 RTT;去掉每 KB 一行的 `sent NkB`;原来的 `image start/end`、`spiSlaveEventHandler 1/2`、`IMAGE READ DONE` 合并成每帧一行 `[cam] frame N: <耗时> us, dma <段字节数…>`(溢出时加 `OVERFLOW`)。
  8. 删掉没有调用者的接口:单帧拍摄 `cameraCaptureFrame()`(MODE_SELECT=0x03)、`cameraGetLines()` 等;CLI 删掉 `spis receive`,`camera print` 改为打印裁剪后的帧。
  9. 删掉 `hm_clk_out()` 里写 TIMER1 PRESCALER 的残留代码(TIMER1 本来就是 0,没有效果)。
  10. 摄像头未初始化时,`cameraEnableStandbyMode()` 不再访问 I2C。
- **验证**(nRF52840 DK + HM01B0,QVGA):
  - 重构前基线:viewer 20 s 收 17 帧,0 丢包,约 0.95–1.0 fps;`.bss` 180,244 B。
  - 重构后:viewer 25 s 收 22 帧 320×240,7133 包 0 丢包,约 0.95–1.01 fps;**每帧** RTT 都是 `dma 39528 39204`(= 122 + 121 行 × 324),无溢出,采集耗时 91.7 ms。
  - 彩条测试图:黑白传感器显示成棋盘格,奇偶行各自完全相同,包括段分界处,彩条边界上下笔直,没有错位。
  - `.bss` 25,460 B,少了 154,784 B(两份 77 KB 都移出了 `.bss`);帧缓冲池 0x2000B2C0–0x2003E000 = 208,192 B,QVGA 2 槽。
- **过程中的教训**:原来的 118/119 行切段不是 bug。第 0 行紧跟 FVLD 开始(FVLD 后约 327 µs 第 0 行就结束),312 µs 延时就是为了整行跳过它。实测在 FVLD 上升时就拉 CS(包括用 PPI 硬件拉)会丢第一个字节,整段错 1 像素。我中途曾改成别的片选方案,用户决定保持原设计。
- **遗留 / 下一步**:
  - TODO 第 2 步:Makefile `CAM_MODE`。QQVGA 用的 156 µs 延时比 QQVGA 一行(约 162 µs)短,有可能切进第 0 行中间,要用每段字节数确认。
  - `0x3052` 回读校验失败、`0x3401` 报错仍在(和原来一样)。

## 2026-09-30 · 文档:新增开发日志 DEVLOG.md(PR #7)

- **阶段**:基础设施
- **改了什么**:新增 `DEVLOG.md`,补记已完成的 PR #1 / #3 / #5 / #6;`CLAUDE.md` 新增"开发日志"规则(每个 PR 必须在同一个 PR 里追加一条)和条目格式。
- **为什么**:每个阶段改了什么、怎么验证的,在仓库里有据可查。
- **验证**:纯文档。

## 2026-09-30 · 文档:帧尺寸/裁剪决定 + 修订 TODO(PR #6)

- **阶段**:规划(TODO 修订)
- **改了什么**:`CLAUDE.md` 的"已定决定"和验收标准。
- **为什么**:确定模式在编译时选、不做运行时切换;DMA 收传感器完整输出(HM01B0 QVGA 324×244),固件发 BLE 时裁成标准 320×240 / 160×120;Monitor 只做 HM0360。
- **验证**:纯文档。
- **遗留 / 下一步**:开始 TODO 第 1 步(重构 HM01B0)。

## 2026-09-30 · 文档:新增 CLAUDE.md(PR #5,Issue #4)

- **阶段**:基础设施
- **改了什么**:新增 `CLAUDE.md`:设计文档入口、Windows 工具链与命令、已知坑、GitHub 流程、TODO 验收标准。
- **为什么**:新对话能直接接手,不用重新摸索环境。
- **验证**:纯文档。

## 2026-09-30 · 电脑端实时预览窗口(PR #3,Issue #2)

- **阶段**:工具
- **改了什么**:新增 `host/`:`viewer.py`(BLE 后台接收 + OpenCV 窗口,右上角 FPS / 帧尺寸,空格存图,断线自动重连,`--rtt --reset` 合并固件日志到 `logs/session-*.log`)、`protocol.py`(协议常量与拼帧)。
- **验证**:25 s 会话 17 帧、5440 包、0 丢包,FPS 约 0.72–0.86;空格存图读回与原始像素一致。
- **遗留**:推流时 RTT 日志延迟/丢失严重;FPS 被固件串行流程 + BLE 限制在约 0.8。

## 2026-09-23 · 在 nRF52840 DK(Windows)上跑通 banji_dev + 修 HM01B0 帧尺寸(PR #1)

- **阶段**:上板
- **改了什么**:
  - `fw/Makefile`:`-I./` → `-I.`(Windows GCC 不认结尾斜杠);新增 `RTT_LOG=1`;`banji_dev` 用开发板引脚映射 `-DFF=0`(PCLK P1.02、D0 P1.03)。
  - `fw/gpio.h`:`FF` 可在编译时覆盖。
  - `fw/main.c`:初始化 NRF_LOG。
  - `fw/HM01B0/HM01B0_FUNC.c`:全分辨率分支补写 `REG_GRP_PARAM_HOLD`(否则传感器停在 162×324,画面黑带)。
  - 新增 `fw/tools/dk.ps1`、`fw/tools/ble_receive.py`。
- **验证**:彩条测试图 324×239 完整对齐;真实画面多帧 0 丢包,每帧约 1.2 s。
- **遗留**:画面偏暗;`0x3052` 回读校验失败;连接初期 `0x3401` 报错。
