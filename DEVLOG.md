# 开发日志

每个阶段(每个合并进 `main` 的 PR)在这里追加一条,**最新的在最上面**。格式见 `CLAUDE.md` 的"开发日志"一节。
设计和 TODO 见 Obsidian 笔记 `Vuebuds Hm0360迁移对比.md`。

---

## 2026-10-02 · 问题 3:HM0360 唤醒后第一帧能直接用,pre-meter 不影响唤醒时间(PR #28,Issue #27)

- **阶段**:调查 / 实验工具
- **改了什么**(默认编译不变):
  - `fw/monitor.c`:`MONITOR_FIRST_FRAMES_TEST` 实验:每次 HM0360 唤醒连续存第 1、2、3 帧(第 3 帧覆盖第 1 帧的槽),打印三帧平均亮度、|第1−第2| 和 |第2−第3|(后者就是正常帧之间的噪声);帧边界睡眠抽成 `sleep_on_frame_boundary()`,实验和正常流程共用。
  - `fw/sensors/hm0360/hm0360_sensor.c`:`HM0360_PMU_CFG_5` / `HM0360_PMU_CFG_6` 可在编译时覆盖 pre-meter 寄存器 0x3026 / 0x3027。
  - `fw/tools/dk.ps1`:新增 `-Cflags '<defines>'`(每组 define 一个输出目录)。
- **为什么**:问题 3,Monitor 每次唤醒 MCLK 开 339 / 195 ms,只有约 91 / 46 ms 是有用的数据行。
- **和原代码的行为差异**:无(实验开关默认关;`MONITOR_SKIP_FRAMES` 默认仍是 1)。
- **验证**(HM0360 板,QVGA,室内静止场景,每组 15 s):

| 0x3026 / 0x3027 | 唤醒 → 第一个 FVLD | 三帧平均亮度 | 第1−第2 | 第2−第3 |
|---|---|---|---|---|
| 0x03 / 0x81(当前:每次唤醒 pre-meter,时间上限 8) | 131 ms | 93 93 93 | 1–2 | 2 |
| 0x01 / 0x81(唤醒不做 pre-meter) | 131 ms | 92 92 92 | 1–2 | 1–2 |
| 0x03 / 0x21 | 131 ms | 93 93 92 | 1–2 | 1–2 |
| 0x03 / 0x11 | 131 ms | 93 93 93 | 1–2 | 1–2 |

  - **第 1 帧和第 2、3 帧一样**(差异和正常帧间噪声相同),跳过的那一帧在这个场景下没有用处。
  - **pre-meter 设置不影响唤醒等待**,四种都是 131 ms;这约 1.3 帧是传感器从 S2 醒来本身的时间。
  - 不跳帧(`-Cflags '-DMONITOR_SKIP_FRAMES=0'`)的 Monitor:QVGA 唤醒 → 存完 **235 ms**(原 339),QQVGA **136 ms**(原 195),各 15 s 28 帧,0 坏帧、0 overrun。
- **遗留 / 下一步**:
  - **是否把 `MONITOR_SKIP_FRAMES` 默认改成 0,由用户决定**(10-01 定的是跳 1 帧)。只在静止的室内场景测过;睡眠期间光线突变时第 1 帧的曝光还没验证(默认配置下唤醒时会做 pre-meter,理论上就是为了这种情况)。
  - 剩下的 131 ms 唤醒等待:可以试 MODE_SELECT 的快照 / 自动唤醒模式(Streaming 2 / 3)代替 XSLEEP,看醒来是否更快。

## 2026-10-02 · 问题 1(方案 E):数到最后一行就结束采集,不等 FVLD↓(PR #26,Issue #25)

- **阶段**:修 bug(Issue #13 的第二种现象)
- **改了什么**:
  - `fw/capture/capture.c`:新增硬件行计数器 TIMER2(计数模式,16 位):PPI 把 LVLD 下降沿接到 COUNT、FVLD 每个边沿接到 CLEAR;CC[0] = `transport_height`,比较中断里拉高 CS 结束最后一段、关掉 FVLD 事件。FVLD↓ 仍是后备结束条件。LVLD 的 GPIOTE 事件从 arm 起一直开着(给计数器用),**中断**仍然只在分段边界附近打开(和原来一样)。
  - `fw/capture/capture.h`:统计里新增 `ended_by_line_count`;开头的时序说明更新。
  - `fw/camera.c`:由 FVLD↓ 结束的帧在 RTT 行末标 `(ended by FVLD)`。
  - `fw/monitor.c`:HM0360 采完后**等到 FVLD↓(帧边界)再拉低 XSLEEP**(最多等 30 ms),RTT 每帧多一行 `slept at FVLD fall, N us after the last line`。
- **为什么**:HM0360 的 FVLD 在最后一行后还要高 13 ms(36 行空白),两帧之间只低 54–65 µs,FVLD↓ 中断被 SoftDevice 推迟就会晚一个帧周期才结束。按行数结束后,中断有 13 ms 的余量。
- **和原代码的行为差异**:VueBuds 原来在 FVLD↓ 中断里结束采集;现在最后一行结束就结束,多用了 TIMER2 和 2 个 PPI 通道、每帧多一个 TIMER2 中断。TIMER2 原本留给 UART CLI(libuarte),`cliInit()` 在原代码里就是注释掉的;以后要开 CLI 得换一个定时器。Monitor 的睡眠时间点和原来一样(帧边界),只是现在是主循环里等 FVLD↓,不是 FVLD↓ 中断之后顺带发生。
- **验证**:
  - HM0360 QVGA 推流 60 s,和 main 在同一时间段对比:main 58 帧里 **11 帧耗时翻倍**(206.5 ms)、1 帧 OVERFLOW;本 PR 60 帧**全部 90.9–91.0 ms**(原 103.7),0 翻倍、0 坏帧、0 FVLD 结束,0.99 fps(main 0.94)。
  - HM0360 QQVGA 推流 40 s:134 帧,全部 45.6–45.8 ms(原 58.7),0 坏帧,3.28 fps。
  - HM0360 Monitor:QVGA 唤醒 → 第一个 FVLD 131 ms、→ 存完 339 ms;QQVGA 77 / 195 ms;各 20 s 38 帧,0 坏帧、0 overrun。和改动前一样。
  - HM01B0:QVGA 推流 60 s 61 帧,91695 µs(原 91705),0 丢包;1 帧由 FVLD 结束(数据正确:两个中断同时挂起时 GPIOTE 中断号更小先执行)。QQVGA 推流 161 帧、Monitor 正常。
- **发现**:**HM0360 进入 S2 的时间点决定下一次唤醒有多快**。QVGA 实测:在 FVLD 低的 65 µs 里拉低 XSLEEP → 下次唤醒 131 ms 出第一个 FVLD;在最后一行之后的空白里拉低 → 235 ms;在下一帧开始后拉低 → 235 ms(都是多一整帧)。所以 Monitor 必须在帧边界睡,原计划"Monitor 早 13 ms 关 MCLK"做不到(MCLK 开的时间和原来一样是 339 / 195 ms)。开始后的第一次睡眠会错过边界(等满 30 ms),之后都正好卡在边界上。
- **遗留 / 下一步**:等 FVLD↓ 是主循环里忙等约 13 ms;以后可以改成 FVLD↓ → PPI → GPIOTE 直接拉低 XSLEEP(纯硬件,不会错过 65 µs 的窗口)。问题 3。

## 2026-10-02 · 问题 4:BLE 帧率波动的来源 + 测量工具(PR #24,Issue #23)

- **阶段**:工具 / 调查
- **改了什么**:
  - 新增 `host/ble_bench.py`:按 `--board` 复位板子、同时抓 RTT,推流 `--duration` 秒,输出连接参数(固件日志里的连接间隔、PHY、数据长度、MTU)、帧数、平均帧率(从第 2 帧起)、每帧时间、包速率、每个连接事件的包数(按通知到达间隔 > 2 ms 分组估算)、固件丢掉的坏帧数;`--csv` 追加一行汇总。
  - `host/viewer.py`:结束时的 summary 除了 `last_fps`(最近 5 帧)再给 `session_fps`(整次会话平均)。
- **为什么**:用户提出的问题 4,同一份固件 0.7–1.0 fps,前后版本没法比。
- **和原代码的行为差异**:无(只是电脑端工具,固件不变)。
- **验证 / 结论**(HM01B0 板,QVGA,main 固件):
  - 同一时间段 9 次 40–60 s:**0.95–1.01 fps**,0 丢包。连接参数:2M PHY、数据长度 251、MTU 247,连接间隔有时 7.5 ms 有时 15 ms(Windows 每次连接自己选;前 5 s 是 Windows 默认间隔,所以第 1 帧慢)。
  - **瓶颈在电脑端蓝牙控制器**:7.5 ms 时每个连接事件平均 2.3 个包,15 ms 时约 4 个,两种情况都是约 **310 包/s**(≈ 75 KB/s)。按 2M PHY 计算,15 ms 里能发约 10 个 251 字节的包,所以不是空口的极限。
  - **SoftDevice 通知队列不是瓶颈**:实验把 `hvn_tx_queue_size` 从默认 1 加到 16(还要把应用 RAM 起点后移 4 KB,否则 `NRF_ERROR_NO_MEM`),A/B 交替各 4 次:16 → 平均 1.00 fps,默认 → 0.98 fps,差别在波动范围内。实验代码没有提交。
  - **之前看到的"0.7 fps"大多是 viewer 指标的噪声**:`last_fps` 只算最近 5 帧,一次 1.4–1.6 s 的长间隔(丢坏帧后重采、或者一次无线干扰)就会把它拉到 0.86 甚至 0.7;同一次会话的 `session_fps` 是 0.96。
  - 每帧约 1.0 s = BLE 发 318 包约 0.86 s + 发完后 arm、等下一个 FVLD↑(0–98 ms)+ 采集 91.7 ms。
- **对比方法**(写进 CLAUDE.md):用 `ble_bench.py`,两版在同一时间段交替跑(A B A B),每版至少 3 次取平均;不要和别的日子的数字比。
- **遗留 / 下一步**:要提高帧率只能隐藏采集时间(发送当前帧的同时往另一个槽采下一帧,约 +15%),这会改变 VueBuds 原来"发完再采"的流程,需用户决定;空口吞吐受电脑端限制,换手机 / 别的适配器可能不同。

## 2026-10-02 · HM01B0 Monitor:MCLK 常开,推流流程去掉 BLE(PR #22,Issue #21)

- **阶段**:新功能(用户 2026-10-02 提出,改变了之前"Monitor 只做 HM0360"的决定)
- **改了什么**:
  - `fw/monitor.c`:开机按读到的型号选流程。HM0360 不变(S2 睡眠 / 唤醒、跳 `MONITOR_SKIP_FRAMES` 帧);HM01B0 开机进流模式后 MCLK 和传感器一直开,每次 RTC2 到点只 arm 最旧的槽,不跳帧、不碰 XSLEEP。`MONITOR_PERIOD_MS=0` 时不开 RTC2,每采完一帧立刻 arm 下一个槽。坏帧日志加了耗时。去掉了槽里不再用的 `ok` 字段。
  - `fw/monitor.h`、`fw/Makefile` 注释;`fw/tools/dk.ps1` 新增 `-PeriodMs`(单独的输出目录 `_p<N>`),开头的用法说明补上 `-Board`。
- **为什么**:HM01B0 也要有只缓存的模式,和 HM0360 Monitor 对比(功耗基线:MCLK 常开 vs 睡眠 / 唤醒)。周期默认和 HM0360 一样 500 ms;想要"每帧都存"用 `-PeriodMs 0`。
- **和原代码的行为差异**:推流编译(`CAM_APP=STREAM`)不变;HM0360 Monitor 的流程不变。
- **验证**(HM01B0 板):
  - QVGA、500 ms:槽 0 / 1 交替,帧序号连续,间隔约 489 ms(系统时钟比 RTC 快约 0.5%,另外 HM01B0 在 arm 后要等下一个 FVLD↑,0–98 ms);20 s 39 帧全部正确。
  - QQVGA、500 ms:槽 0–9 循环,arm → 存完 34–54 ms。
  - QVGA、每帧都存:20 s 100 帧、0 坏帧(每 2 个帧周期存 1 帧,196 ms)。
  - QQVGA、每帧都存:20 s 存 346 帧、丢 2 帧坏帧;同样是每 2 个帧周期存 1 帧(57 ms),因为 HM01B0 QQVGA 帧间只有约 1.3 ms,主循环重新 arm 赶不上。
  - PR #20 的 Monitor 丢帧分支:坏帧打印 `BAD frame dropped`,下一次唤醒重写同一个槽,之后正常继续。
  - HM0360 Monitor 没法在板子上回归(HM0360 模块暂时不响应 I2C)。
- **发现**:HM01B0 模块没有 RST、DK 复位不断电;换模式的固件后第一次初始化有时不生效(QQVGA 固件下帧周期还是 QVGA 的 98 ms、每帧 `19602 OVERFLOW`,或者一帧都收不完),再复位一次就正常。VueBuds 原初始化也没有软件复位,推流时同样会这样(第 1 帧坏)。已记入 CLAUDE.md 已知坑。
- **遗留 / 下一步**:是否在 HM01B0 初始化时加软件复位(改原初始化流程,需用户决定);HM0360 恢复后回归 HM0360 Monitor。

## 2026-10-02 · 问题 2(方案 B):坏帧检测,推流不发、Monitor 不存(PR #20,Issue #19)

- **阶段**:修 bug(Issue #13 的第一种现象,先做方案 B)
- **改了什么**:
  - `fw/camera.c`:`cameraGetFrame()` 在采集层的 `ok` 为假(某段 DMA 字节数不等于该段行数 × 行宽,或 OVERFLOW)时返回 false,RTT 那一行末尾加 `, BAD, dropped`。
  - `fw/main.c`:推流时坏帧不调用 `bleSendFrame()`,直接放入 `EVENT_CAMERA_READY_NEXT_FRAME` 重新采下一帧。
  - `fw/monitor.c`:坏帧不标记就绪、不推进槽号、帧序号不加(下一次唤醒重写同一个槽),打印 `[mon] slot N: BAD frame dropped (dma …)` 和累计丢弃数;正常帧那一行去掉了 `BAD` 分支。
- **为什么**:用户选的方案 B,先保证坏帧不会发出去 / 存下来;根源(硬件切段 / 乒乓)以后再做。
- **和原代码的行为差异**:VueBuds 原来每一帧都发;现在字节数不对的帧不发,重采一帧(多等 1–2 个帧周期)。
- **验证**(HM01B0 板):
  - QVGA 推流 3 分钟:采 140 帧,坏帧 14 帧全部丢弃,viewer 收到 126 帧、0 丢包。
  - QQVGA 推流 2 分钟:采 366 帧,坏帧 38 帧全部丢弃,viewer 收到 327 帧、0 丢包。
  - 对照:PR #11 的版本(HM0360 改动之前)在同一块板上 QVGA 3 分钟 129 帧里 9 帧坏帧(只是没丢,直接发出去了),所以**不是 PR #14 / #16 引入的回退**。
  - Monitor 的丢帧分支:HM0360 板暂时不响应 I2C,在 HM01B0 Monitor 的 PR 里验证。
- **发现**:
  - HM01B0 上坏帧约占 **7–10%**,远多于之前以为的"偶发";而且大多不是分段切换(第 2 段),而是**第 1 段开头少 2–150 字节、没有 OVERFLOW**:帧开始时 CS 晚拉低,第 1 行开头被丢掉,整帧(QQVGA)或上半帧(QVGA)错位。
  - CS 晚拉低有两个来源:FVLD↑ 中断被推迟(TIMER4 晚启动,这时"耗时"也短几十 µs),以及 TIMER4 比较中断被推迟(耗时正常但字节数少)。HM01B0 第 1 行在 FVLD↑ 后约 376 µs(QVGA)/ 215 µs(QQVGA)开始,CS 定在 312 / 156 µs,余量只有几十 µs。
  - HM0360 第 0 行在 FVLD↑ 后 750 µs 才开始、CS 在 20 µs,有 700 多 µs 余量,所以 HM0360 上没出现这种坏帧。
  - 丢帧后的重采常常要等 2 个帧周期(重新 arm 时下一帧的 FVLD↑ 已经过去)。
- **遗留 / 下一步**:可选的根治办法(**改动 VueBuds 的片选方式,需要用户决定**):FVLD↑ 通过 PPI 直接启动 TIMER4,TIMER4 比较事件通过 PPI + GPIOTE 直接拉低 CS,时序不变(仍是 FVLD↑ 后 312 / 156 µs),只是不经过 CPU 中断。

## 2026-10-02 · 工具:两块 DK 同时接电脑时按板选择(PR #18,Issue #17)

- **阶段**:工具
- **改了什么**:
  - `fw/tools/dk.ps1`:新增 `-Board HM0360|HM01B0`(默认 HM0360),nrfjprog 用 `--snr`、JLinkRTTLogger 用 `-USB`;RTT 日志写到 `_build_win\rtt_<板>.log`,`camera` 按 BLE 地址连接、图存到 `_build_win\frames_<板>\`。
  - `host/protocol.py`:新增 `BOARDS`(两块板的 J-Link 序列号和 BLE 地址)。
  - `host/viewer.py`、`fw/tools/ble_receive.py`:新增 `--board hm0360|hm01b0`,设定 BLE 地址;viewer 的 `--rtt --reset` 用对应的 J-Link。
- **为什么**:两块板同时插着时,原来的工具不知道操作哪一块(两块都叫 `mustard`)。固件不变,两块板烧同一个 hex,开机自动识别传感器。
- **和原代码的行为差异**:无(只是工具)。不加 `-Board` 时默认操作 HM0360 板,只插一块 HM01B0 板时要加 `-Board HM01B0`。
- **验证**:
  - HM01B0 板:`dk.ps1 flash/camera -Board HM01B0` 收 3 帧 320×240、0 丢包;`viewer.py --board hm01b0 --rtt --reset` 20 s 收 8 帧、0 丢包,RTT 来自 HM01B0 板。
  - HM0360 板:`dk.ps1 flash/run -Board HM0360` 烧录、复位、RTT 都是这块板;但传感器不响应 I2C(接线检测 [4] MCLK 引脚翻转 PASS、[5] 0/5 读到 ID),和 10-01 的 MCLK 焊点问题症状一样,是硬件问题,等检查。
- **发现**:HM01B0 推流时出现一帧 `dma 39503 39204`(第 1 段少 25 字节、没有 OVERFLOW、耗时短 73 µs):FVLD↑ 中断被推迟,TIMER4 晚启动,CS 晚拉低,第 1 行开头少收 25 字节,上半帧错位。这是笔记 §4.4 的风险第一次实测出现。
- **遗留 / 下一步**:HM0360 模块硬件检查;问题 1–4 + HM01B0 Monitor。

## 2026-10-02 · HM0360 Monitor:RTC2 周期 S2 唤醒,帧写进环形缓存(PR #16,Issue #15)

- **阶段**:TODO 第 4 步(HM0360 Monitor,只做缓存)
- **改了什么**:
  - 新增 `fw/monitor.{c,h}`:
    - RTC2(nrfx 驱动,32.768 kHz)每 `MONITOR_PERIOD_MS`(默认 500)ms 产生一次中断,经事件队列交给主循环:开 MCLK → XSLEEP 高 → 采集层写入最旧的槽,先跳过 `MONITOR_SKIP_FRAMES`(默认 1)帧 → 采集完成后 XSLEEP 低 → 关 MCLK → 标记这个槽就绪(帧序号、时间戳、DMA 是否正确)。
    - 不是 HM0360 就不启动;上一帧还没完成时再次唤醒,记一次 overrun 并跳过。
  - `fw/capture/`:新增 `capture_arm_slot(slot, skip_frames)`,`capture_arm()` 改为调用 `capture_arm_slot(0, 0)`;统计里新增 `ok`(各段字节数正确且无溢出)和 `arm_to_fvld_us`;新增 `capture_mclk_running()`。
  - `fw/camera.c`:新增 `cameraSensorStream()`、`cameraSleep()`(XSLEEP 低 → 100 µs → 关 MCLK / 开 MCLK → 100 µs → XSLEEP 高)、`cameraArmSlot()`、`cameraSlotFrame()`、`cameraModelId()`、`cameraSlotCount()` 等。
  - `fw/main.c`:`CAMERA_APP_MONITOR` 时开机放入 `EVENT_MONITOR_START`;`EVENT_MONITOR_WAKE` → `monitorWake()`;采集完成 → `monitorFrameDone()`(不发 BLE);收到 0xB1 只打日志。`fw/event.h` 新增两个事件。
  - `fw/Makefile`:`CAM_APP=STREAM|MONITOR`,可选 `MONITOR_PERIOD_MS`、`MONITOR_SKIP_FRAMES`;`tools/dk.ps1` 新增 `-Monitor`。
- **为什么**:笔记 §5.2 / §8 第 4 步。用户确认的选择:编译时开关、开机自动开始、周期 500 ms、每次唤醒固定跳过 1 帧。
- **和原代码的行为差异**:默认的 `CAM_APP=STREAM` 编译下,推流行为不变(`capture_arm()` 仍然写槽 0、不跳帧;中断里只多了一次计时和一个字节数检查)。Monitor 是新功能。
- **验证**(DK SN 1050291681 + HM0360):
  - **QQVGA**:槽号 0→9→0 循环,帧序号连续,间隔约 497.5 ms,每帧 DMA 正确,每次唤醒前 XSLEEP 为低、MCLK 关闭;唤醒 → 第一个 FVLD 77 ms,唤醒 → 存完 195 ms。
  - **QVGA**:槽号 0↔1 交替,帧序号连续(1–27),间隔约 498 ms,每帧两段 DMA 正确,XSLEEP 低 / MCLK 关;唤醒 → 第一个 FVLD 131 ms,唤醒 → 存完 339 ms。
  - 用 J-Link 把帧缓冲池读出来看画面:QQVGA 10 个槽、QVGA 槽 0 都是完整的正常画面。运行中直接读会读到"清零后还没收完"的槽,要先暂停 CPU。
  - 推流回归(`CAM_APP=STREAM` QVGA):两次 20 帧 0 错位、0 丢包。帧率 0.70 fps,但同时间段 `main` 也是 0.69–0.70,属于 BLE 环境波动,不是这次改动造成的。
- **发现**:
  - **XSLEEP 唤醒后约 1.3 个帧周期才出第一个 FVLD**(QQVGA 77 ms、QVGA 131 ms),看起来传感器醒来后内部先跑约一帧。加上跳过的 1 帧和存下的 1 帧,每次唤醒 MCLK 要开 195 / 339 ms,在 500 ms 周期里占 39%(QQVGA)/ 68%(QVGA)。
  - QVGA 刚启动时第一次唤醒用了 542 ms,超过 500 ms 周期,第二次唤醒记了 1 次 overrun,之后正常。
  - 日志时间戳用的是系统定时器(内部 RC 振荡器),RTC2 用的是 32.768 kHz 晶振,两者差约 0.5%,所以间隔显示为 497–498 ms。
- **遗留 / 下一步**:
  - 降低 MCLK 开启时间的方向:不跳帧(要先验证唤醒后第一帧的曝光)、缩短唤醒等待、用 RTC → PPI → GPIOTE 由硬件翻转 XSLEEP。
  - QVGA 段间错位(Issue #13)在 Monitor 里同样可能发生,槽的 `ok` 标志会把它标出来。
  - 触发和回传、读出缓存的帧:按计划以后再做。

## 2026-10-01 · HM0360 驱动 + 上板:QVGA / QQVGA 出图,S2 唤醒预验证(PR #14,Issue #12)

- **阶段**:TODO 第 3 步(HM0360 驱动 + 上板)
- **改了什么**:
  - 新增 `fw/sensors/hm0360/`,结构和 `hm01b0/` 一样:`include/`、`private_include/hm0360_regs.h`(按 datasheet V04)、`hm0360_modes.c`(寄存器表)、`hm0360_mode_info.c`、`hm0360_reg.c`、`hm0360_sensor.c`。
  - 寄存器配置:
    - 1-bit 串行、MSB 先发、PCLKO 按行门控(datasheet 表 6.5)。
    - `PLL1CFG = 0x07`:Sensor_Core = 8 MHz ÷ 8 = 1 MHz,PCLKO = 8 MHz。
    - 窗口模式 640×480;QVGA = Sub2、QQVGA = Sub4,**不开合并**。
    - 行长 376;帧长取传感器在这个模式下的最小值 **QVGA 276、QQVGA 156**(= 输出行数 + 36),帧周期 103.8 ms / 58.7 ms(9.6 / 17 fps)。
    - 关闭上下文切换和运动检测。
    - 画质调校(黑电平、色调映射、AE、厂商模拟寄存器)沿用 OpenMV 的表,但覆盖了其中不适合 1-bit / SPIS 的几项(见下面的"行为差异")。
  - `fw/sensors/camera_sensor.c`:注册 HM0360;`camera_sensor_detect()` 改成轮询读 ID(带超时)。
  - `fw/camera.c`:开 MCLK 之后做硬件复位(XSHUTDOWN 低 → 高),XSLEEP 保持高。`fw/gpio.h`:新增 `CAM_XSLEEP` P1.04、`CAM_XSHUTDOWN` P1.05。
  - `fw/wiring_test.c` + `main.c` 里的 `#ifdef WIRING_TEST`:HM0360 接线 / 状态检测工具,11 项 PASS/FAIL(第 11 项用 CPU 周期计数器测帧时序,核对帧周期 = 寄存器帧长 × 行长);`tools/dk.ps1` 新增 `-WiringTest` 开关(可配 `-Mode`)。
  - `fw/Makefile`:加入 `sensors/hm0360`。
- **为什么**:笔记 §8 第 3 步。
- **和原代码的行为差异**(HM01B0 的寄存器和采集流程都不变):
  1. `cameraInit()` 开 MCLK 后多了一次硬件复位(P1.05 拉低 10 ms + 等 1 ms;HM01B0 模块没有 RST,这个引脚不连接,不起作用),检测传感器改成轮询读 ID(超时 1 s)。开机时**不再**拉低 P1.05(调试中加过,按用户意见去掉)。
  2. 检测到 HM0360 时走新的驱动;BLE 协议和采集层都不变。HM0360 的 `mode_info`:transport = standard = 320×240 / 160×120,片选延时 20 µs,从第 0 行开始收。
- **验证**(新 DK:SN 1050291681,nRF52840 REV3;模块 Arducam UC-806,MCLK = P1.08,RST = P1.05,XSLEEP = P1.04):
  - 接线检测 10 项全部 PASS:模块上拉、片选跳线、无 MCLK 时不应答、MCLK 输出、5/5 次复位都读到 0x0360、RST 能复位寄存器、XSLEEP 能断开 / 恢复 I2C、停 MCLK 后寄存器保留、**S2 唤醒流程寄存器保留且立刻可用**、FVLD / HVLD / PCLKO / D0 都有信号。
  - 彩条测试图:QQVGA 每帧 19,200 B,QVGA 每帧 38,400 + 38,400 B(= 宽 × 高,**验证了 PCLKO = 8 MHz**);奇偶行各自完全相同,条边笔直。
  - 接线检测第 [11] 项:实测帧周期 QVGA 103,776 µs = 276 × 376 µs、QQVGA 58,656 µs = 156 × 376 µs,和寄存器一致(PASS)。
  - 真实画面,30 s(修正帧长后):QQVGA 92 帧 160×120,0 丢包,约 3.3 fps,每帧耗时 58.6 ms;QVGA 24 帧 320×240,0 丢包,约 1.0 fps,每帧耗时 103.7 ms,25 帧 DMA 全部正确。第 1 帧(刚开始推流)耗时约两倍;QQVGA 92 帧里另有 3 帧耗时翻倍(帧结束中断赶不上 54 µs 的 FVLD 低电平,数据正确,只晚一帧,见 Issue #13)。
  - `.bss` 25,468 B。
- **过程中的教训**:
  - 一开始用的是 OpenMV 的表,出现了三个问题:`0x30A5=0x04` 让 PCLKO 连续输出,按行门控失效,每行收到 376 字节;`0x30A1–0x30A4` 让每行多出 40 字节;`AE_CTRL=0x5F` 打开了自动降帧率,帧被拉长到约 600 ms。三项都已经在驱动里覆盖。
  - 开合并(Bin2 / Bin4)时,两种模式都出现大量黑 / 白竖条,关掉就正常;用户选了"只子采样"(方案 A)。
  - "每帧耗时比计算值长"(QVGA 104 ms vs 99.6 ms):OpenMV 的帧长 265 / 132 **低于传感器在这个模式下的最小值**(输出行数 + 36),传感器自动改用 276 / 156;和自动曝光无关(关 AE、积分 32 行时序不变)。另外 HM0360 的 FVLD 是整帧周期信号:FVLD 上升 2 行后出第 0 行,36 行空白都在高电平里,帧间只低 54–65 µs,所以日志里的"耗时"≈ 帧周期。
  - 中间很长一段时间 HM0360 时好时坏、最后完全不响应 I2C,换引脚、断电都没用,**最后查出是模块 MCLK 的焊点不良**。期间先后怀疑过上电顺序、CPU 休眠影响 MCLK、XSHUTDOWN 断线,这些都被实验排除了。为了恢复调试,MCLK / RST 曾临时改到 P1.07 / P1.10 / P1.11,最后又改回了 P1.08 / P1.05。
- **遗留 / 下一步**:
  - **Issue #13**:QVGA 偶尔有一帧下半部错位(两段 DMA 之间的 CS 脉冲被 SoftDevice 推迟,超出约 56 µs 的行间消隐);帧结束的 FVLD 中断赶不上 54–65 µs 的帧间低电平时,采集晚一帧(数据正确)。暂未修,Issue 里列了方案 A–D,以及"关 VSYNC 模式让 FVLD 只覆盖数据行"的实验方向。
  - HM01B0 的板子这次没有回归测试(DK 上接的是 HM0360);HM01B0 的路径代码没有改,只多了 `cameraInit` 里对 P1.05 的复位脉冲(该引脚不连接)和 ID 轮询。
  - TODO 第 4 步:HM0360 Monitor(S2 唤醒已经提前验证可行)。

## 2026-09-30 · 编译时选模式 CAM_MODE=QVGA/QQVGA,QQVGA 首次上板(PR #11,Issue #10)

- **阶段**:TODO 第 2 步(编译时选模式,HM01B0)
- **改了什么**:
  - `fw/Makefile`:`CAM_MODE=QVGA`(默认)/ `QQVGA` → `-DCAMERA_MODE=…`,其他值直接报错;可选 `CAM_TEST_PATTERN=COLOR_BAR / WALKING_1`;`make help` 列出这些选项。
  - `fw/tools/dk.ps1`:新增 `-Mode QVGA|QQVGA`、`-ColorBar`;每种组合用单独的输出目录(如 `_build_win_qqvga_colorbar`),因为 SDK Makefile 只改编译参数时不会重新编译。
  - `fw/.gitignore`:改成 `_build_win*/`,覆盖所有编译目录。
- **为什么**:已定的决定,模式在编译时选,不做运行时切换。驱动和采集层在 PR #9 里已经按 `mode_info` 工作,这一步只需要接上编译选项并上板验证。
- **和原代码的行为差异**:默认编译(QVGA)和 PR #9 完全一样。QQVGA 在原代码里只能改头文件打开,收的是 162×119;现在按 `mode_info` 收第 1–121 行,发出 160×120。原来的 QQVGA 从没在 DK 上跑过。
- **验证**(nRF52840 DK + HM01B0):
  - QQVGA 真实画面:viewer 20 s 收 87 帧 160×120,7009 包 0 丢包,约 3.6–5 fps;RTT 抓到的 51 行逐帧日志全部是 `dma 19602`(= 121 行 × 162),无溢出,采集耗时 26.2 ms。这说明 156 µs 的片选延时**没有**切进第 0 行(PR #9 遗留问题排除)。
  - QQVGA 帧缓冲池 208,192 B = 10 槽 × 19,764 B,和笔记 §5.2 一致。
  - 彩条测试图:QQVGA 和 QVGA 都是奇偶行各自完全相同、条边笔直、0 丢包。QQVGA 只显示得下 3 条(条宽固定约 52 个输出像素),属于测试图本身的特点。
  - QVGA 回归:每帧 `dma 39528 39204`。
  - `CAM_MODE=VGA` 时 make 报错退出。
- **遗留 / 下一步**:
  - TODO 第 3 步:HM0360 驱动 + 上板。
  - 有一次 Windows 在 BLE 刚连上时取消了连接;固件断连就复位,RTT logger 因此断开,重跑即可(已记进 CLAUDE.md 已知坑)。

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
