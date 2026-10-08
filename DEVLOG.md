# 开发日志

每个阶段(每个合并进 `main` 的 PR)在这里追加一条,**最新的在最上面**。格式见 `CLAUDE.md` 的"开发日志"一节。
设计和 TODO 见 Obsidian 笔记 `Vuebuds Hm0360迁移对比.md`。

---

## 2026-10-08 · 双板最小流程:"拍一张"命令 0xB2 + 左右同时拍、旋转、拼接(PR #?,Issue #35)

- **阶段**:新功能(双耳最小流程,见 Obsidian `02-daily_raw/10.5~10.11/硬件流程+后续优化.md`)
- **改了什么**:
  - `fw_nrf5340/src/snapshot.c/.h`(新):控制命令 **0xB2 = 拍一张**(用户 10-07 同意加)。第一次请求时初始化相机、让传感器开始出帧并采第 1 帧;之后 HM0360 平时 S2 睡眠,收到请求 → 唤醒 → 采唤醒后第 1 帧到槽 0 → 帧边界睡回去 → 用原来的帧格式通过 BLE 发出。复用 Monitor 的唤醒 / 睡眠函数。**坏帧(字节数不对 / OVERFLOW)时传感器不睡、直接采下一帧,最多重拍 2 次**。HM01B0 没有 S2,MCLK 常开,请求时采下一帧。
  - `fw_nrf5340/src/main.c`、`ble.c`、`event.h`:新事件 `EVENT_CAMERA_SNAPSHOT`;一次开机只能用 0xB1(推流)或 0xB2(拍照)其中一种,另一种被忽略并打日志;Monitor 编译里 0xB2 被忽略。
  - `host/stereo.py`(新):Windows 同时连两块板(按地址,先连左再连右),**空格**同时给两块板写 0xB2,收齐左右两张 → 按 `BOARDS` 的 `rotate` 旋转 → 两种拼接 → 存到 `host/captures/stereo/<时间>/`:`left.png`、`right.png`、`stitch_opencv.png` / `stitch_orb.png`(成功才有)、`info.json`(写命令完成时间、每张收完时间、丢包、拼接结果)。板子刚连上时自动拍一对"热身"(含相机初始化,不存)。`--auto N` 不用键盘拍 N 对;一对 20 s 收不齐就放弃。
  - `host/stitch.py`(新):`stitch_opencv`(`cv2.Stitcher` PANORAMA,作参照)和 `stitch_orb`(论文 §3.2.3 的轻量流程:ORB → BFMatcher 汉明距离 + 比值测试 → `findHomography` RANSAC → `warpPerspective`,不裁边,失败就保留两张)。ORB 的合理性检查:内点 ≥ 15、变形后的右图是凸四边形、每条边长度 0.5–2 倍。`python stitch.py <目录>` 对存下的一对重新拼接。
  - `host/protocol.py`:`CMD_SNAPSHOT = 0xB2`;两块 5340 板 `rotate: 90`。
- **为什么**:双耳流程先跑通(用户 10-07 要求),不追求性能;同时拍靠"主机同时下命令"(方案 A,和论文一样)。
- **和原代码的行为差异**:新增 0xB2 命令(新协议命令,`fw/` 没有);0xB1 推流、Monitor 行为不变。坏帧重拍只在 0xB2 里(推流仍是丢掉不发)。
- **验证**(两块 5340 + HM0360,Windows 直连):
  | | QVGA | VGA |
  |---|---|---|
  | 拍到的对数 | 10 / 10 | 10 / 10(加重拍前:14 对里右板 4 对超时,见下) |
  | 唤醒 → 第一个 FVLD | 129 ms(每次相同) | 287 ms(每次相同) |
  | 请求 → 采完 | 221 ms | 662 ms |
  | 两块板写命令完成时间差 | 0–2 ms | 0–1 ms |
  | 右板收完 / 左板收完(请求后) | 1.3 s / 2.6 s | 5.5 s / 9.2 s |
  | 丢包 | 0 | 0 |
  - 时间差:两条写命令在 2–9 ms 内都完成,唤醒到出帧固定,所以估计两张图拍摄时刻差在 10 ms 量级(没有实测拍摄时刻,只量了写命令完成时间)。
  - 两块板同时传时,先连上的左板比右板慢一倍多;两块合计 VGA 600 KB / 9.2 s ≈ 65 KB/s,低于单板推流的 84 KB/s —— **Windows 的总带宽两块板分着用**,和讨论时的估计一致。
  - 旋转:两块板实拍画面都是"上"朝向图像左边,顺时针转 90° 后正常;ORB 能直接对齐,说明两块方向一致。
  - 拼接(两块 DK 放在桌上、指向不一样,近处有人,视差大):QVGA 10 对两种方法都"成功",VGA 10 对 OpenCV 9 次、ORB 10 次;但 ORB 有时对齐在近处物体上,背景错开;OpenCV PANORAMA 输出的是大幅弯曲的全景(重叠少时焦距估不准)。两种都只是能跑,效果要等板子按真实佩戴方式固定后再比较。ORB 早期有一次把右图拉成细三角也算成功,加了边长检查后排除。
- **遗留 / 下一步**:
  - **HM0360B 在 VGA 下偶发 `OVERFLOW, BAD`(每段字节数都对)**:3 次运行共 25 张里 5 张(5 张中 1、9 张中 3、11 张中 1);左板(HM0360)和 QVGA 都没出现。重拍可以兜住,但这一对左右会差约 0.8 s。根因没查(采集层,属于原设计,需另开 Issue)。
  - VGA 每张采完后都打 `no frame boundary within 30 ms, XSLEEP low anyway`(帧边界在约 28 ms 后,30 ms 超时有时等不到);不影响下次唤醒时间(都是 287 ms)。
  - 实测左右拍摄时刻差(手机毫秒秒表);同步方案 C / C' + D、桥、压缩等见笔记 TODO。

## 2026-10-07 · nRF5340:HM0360 VGA(推流 + Monitor)、帧边界超时 30 → 16 ms、第二块板换成 HM0360(PR #34,Issue #33)

- **阶段**:新功能(VGA)/ 修 bug(Monitor 帧边界)
- **改了什么**:
  - `fw/sensors/`(两个平台共用):`camera_sensor.h` 新增 `CAMERA_MODE_VGA`;`hm0360_modes.c` 新增 VGA 表(不子采样,行长 768、帧长 516、MAX_INTG 512);`hm0360_mode_info.c` / `hm0360_sensor.c` 支持 VGA。HM01B0 没有 VGA(返回不支持)。`fw/`(52840)不编 VGA(一帧 300 KB),编译确认不受影响。
  - `fw_nrf5340/src/camera.c`:帧边界超时(采完后)**30 → 16 ms**(用户 10-07 选方案 B),VGA 用 30 ms;其它时候的超时 VGA 用 850 ms(两帧);传感器没有所选模式时不再 panic,只打日志、不开相机。
  - `fw_nrf5340/src/monitor.c`:VGA 默认周期 **1000 ms**(QQVGA / QVGA 仍 500 ms,`-PeriodMs` 可覆盖)。
  - `fw_nrf5340/src/capture/capture.h`:`CAPTURE_MAX_SEGMENTS` 4 → 8(VGA 5 段)。
  - `fw_nrf5340/CMakeLists.txt`:`CAM_MODE=VGA`;额外定义 `CAM_MODE_<模式>` 宏给 `#ifdef` 用(`CAMERA_MODE_*` 是枚举,`#if` 里看不到,会被当成 0)。
  - `fw_nrf5340/tools/dk5340.ps1`:`-Mode VGA`;板名 `HM01B0` → **`HM0360B`**(SN 1050017384,10-07 起接 HM0360);`-Cflags` 的编译目录名改成短哈希(目录名太长超过 Windows 路径上限会编译失败)。`host/protocol.py`:`hm01b0-5340` → `hm0360b-5340`。
- **为什么**:
  - **帧边界有时等不到的原因**(10-07 查清):**某一帧里 HM0360 的 AE 调了增益,这一帧末尾就没有 FVLD 低脉冲**,FVLD 一直高到下一帧结束(晚 58.68 ms)。DWT 精确计时 + 每帧读 AE 寄存器,59 次唤醒:增益变了 30/30 次没有帧边界,没变 28/28 次都有;曝光一直顶在 152 行、帧长寄存器 156 不变。场景暗或闪时 AE 调得多,超时就多。之前"500 ms 周期锁在坏相位"的推测是错的。52840 用同样的传感器设置,应该也有,未验证。
  - 那一帧只能等超时强制睡眠;实测强制睡眠不会让下次唤醒多等一帧(第一个 FVLD 仍是 74.35 ms),所以把超时缩到刚过正常帧边界(13.5 ms)的 16 ms。
  - VGA:5340 的 RAM 够放一帧(用户 10-07 要求);Monitor 只存 1 个槽(用户同意)。
- **和原代码的行为差异**:超时 30 → 16 ms(仅 5340);新增 VGA 模式;VGA Monitor 周期 1000 ms、1 个槽。
- **验证**(两块板都是 HM0360:`HM0360` = 1050035314、`HM0360B` = 1050017384):

| 测试 | HM0360 | HM0360B |
|---|---|---|
| 推流 QVGA | 19 帧 0 坏帧,1.01 fps | 19 帧 0 坏帧,1.34 fps |
| 推流 QQVGA | 36 帧 0 坏帧,3.71 fps | 62 帧 0 坏帧,3.83 fps |
| 推流 VGA | 7 帧,每帧 `61440` × 5 段,370 ms,0 丢包,0.25 fps(1270 包 / 帧,约 4 s) | 7 帧,同左,画面清楚 |
| Monitor QQVGA | 38 帧 / 16 槽,MCLK 开 135–138 ms | 38 帧,137–140 ms |
| Monitor QVGA | 38 帧 / 5 槽,232–235 ms | 38 帧,234–238 ms |
| Monitor VGA(1000 ms) | 18 帧 / 1 槽,0 overrun,唤醒 → FVLD 285 ms → 存完 656 ms,MCLK 开 681–686 ms | 18 帧,689 ms |
| 压力测试 | VGA 300 µs:7 帧 0 坏帧 | QVGA 2000 µs:17 帧 0 坏帧 |

  - 超时改 16 ms 前,缺帧边界的帧 MCLK 开 152–154 ms;改后同样场景 135–140 ms。全部 Monitor 0 坏帧、0 overrun,唤醒前 XSLEEP 低 / MCLK 关。
  - VGA 500 ms 周期实测:每次唤醒 682 ms > 500 ms,20 s 里 19 次 overrun,所以默认 1000 ms。
  - VGA 帧长没有被传感器抬高:最后一行到 FVLD 下降约 26 ms ≈ 36 行 × 768 µs。
  - 52840 `fw/` 用改过的 `fw/sensors/` 编译通过。
- **遗留 / 下一步**:52840 上是否也有"AE 调增益时没有帧边界"未验证;`wiring_test.c` 仍未移植。

---

## 2026-10-06 · 移植到 nRF5340 DK:新建 fw_nrf5340/(NCS),两颗传感器推流 + Monitor 全部跑通(PR #34,Issue #33)

- **阶段**:新平台移植(nRF5340 DK,nRF Connect SDK v3.2.1)
- **改了什么**:
  - 新目录 `fw_nrf5340/`(和 `fw/` 平级,`fw/` 不动):
    - `CMakeLists.txt`:直接编译 `../fw/sensors/`(驱动和寄存器表只有一份);`CAM_MODE` / `CAM_TEST_PATTERN` / `CAM_APP` / `EXTRA_DEFINES` 编译时选择,名字和 `fw/Makefile` 一样。
    - `src/compat/`:传感器驱动用到的 nRF5 SDK 头文件(`i2c.h`、`timers.h`、`nrf_log.h`、`nrf_delay.h`、`sdk_errors.h`)的 Zephyr 实现。
    - `src/capture/`:PR #32 的硬件片选方案改用 DPPI 实现(通道表在 `capture.c` 开头);MCLK = TIMER0、数行 = TIMER1、2 µs CS 脉冲 = TIMER2;SPIS2 直接操作寄存器。
    - `src/camera.c`、`src/monitor.c`:从 `fw/` 移植,流程和常数不变。
    - `src/ble.c`:Zephyr GATT,服务 / 特征 UUID、`0xB1`、包格式和 `fw/` 相同;网络核跑 Nordic `ipc_radio`(`sysbuild/ipc_radio/prj.conf`)。
    - `src/stress_test.c`:`CAPTURE_STRESS_US` 压力测试。
    - `tools/dk5340.ps1`(build / flash / rtt / run,参数和 `dk.ps1` 对应)、`tools/ncs_env.sh`。
  - `host/protocol.py`:新增 `hm0360-5340`、`hm01b0-5340` 两块板(J-Link SN、BLE 地址、J-Link 器件名);`viewer.py`、`ble_bench.py` 按板选 RTT 器件名和 nrfjprog family。
  - `CLAUDE.md`:更正 Obsidian 笔记路径(`D:\Obsidian\Obsidian Vault\…`),加入 nRF5340 一节。
- **为什么**:换到 nRF5340 DK;nRF5340 不支持 nRF5 SDK / SoftDevice。
- **接线**:按 Obsidian `nrf5340迁移.md` §2,只改 FVLD → P1.09、LVLD → P1.10、片选回环 P1.06 → P1.07。两块板所有信号实测正常(DMA 字节数、彩条),**没有发现接线问题**。
- **和 `fw/`(nRF52840)的行为差异**:
  - **DPPI 通道结构**:DPPI 一个事件只能发布到一个通道、一个任务只能订阅一个通道,所以 FVLD 经 EGU0 分到"开始 / 睡眠"两个受控通道;CS 的三个任务(SET / CLR / OUT)分别用于拉高、帧开始拉低、脉冲结束拉低;数行计数器的"最后一行"和"分段边界"比较各复制一份(CC4 = CC0、CC5 = CC2)。**拉高 CS 的通道一直开着**(`fw/` 放在 run 组里):不采集时 CS 本来就是高,多出来的 SET 没有影响。时序和 PR #32 相同。
  - **SPIS DMA 起始地址必须 4 字节对齐**(nRF5340 实测,52840 不需要):HM01B0 QQVGA 跳过第 0 行后段 0 从槽 + 162 B 开始,DMA 每帧收到 0 字节;槽地址前移 0–3 字节让 DMA 起点对齐后正常。帧在槽里的布局、发送内容不变。
  - **SPIS 改成模式 1(PCLK 下降沿采样 D0)**,两颗传感器都用(用户 10-06 选方案 A;`fw/` 是模式 0、上升沿)。证据:模式 0 下 HM0360 实景画面全是随机噪点、几乎看不清,而彩条干净。原因是 5340 的上升沿采样点正好落在 HM0360 切换 D0 的时刻,彩条位翻转很少所以看不出。改下降沿(离 D0 变化半个时钟)后 HM0360 QVGA / QQVGA 画面清楚;HM01B0 两种边沿都正常。
  - **帧缓冲池 = 剩余全部 RAM**(用户 10-06 决定,和 `fw/` 的思路一样):从镜像结尾 `_end` 到 RAM 区结尾 `__kernel_ram_end`。Zephyr 的栈、堆都静态分配在镜像里;libc `malloc` 关掉(`CONFIG_COMMON_LIBC_MALLOC=n`,原本它会把这段当堆;没有代码用 malloc)。应用核 RAM 448 KB(512 KB 里最后 64 KB 是和网络核共享的 IPC 区;网络核自己的 64 KB 应用核访问不到),镜像占 45.7 KB,**帧池 413,000 B**:HM0360 QVGA 5 槽(52840:2)、QQVGA 21 槽(Monitor 最多用 16;52840:10)、HM01B0 QVGA 5 槽。
  - **时间基准**:`systemTimeGetMs/Us` 用 Zephyr 内核时钟(RTC1,32.768 kHz 晶振,30.5 µs 分辨率),不是 HFCLK 上的 TIMER1;Monitor 周期显示 500 ms(52840 显示 497–498 ms)。Monitor 唤醒用周期 `k_timer`(`fw/` 用 RTC2 比较),同样不累计漂移。睡眠超时用 `k_timer`(`fw/` 用 app_timer)。
  - **日志**:`printk` 直接写 RTT(`fw/` 是 NRF_LOG 延迟输出),RTT 上行缓冲 4 KB(`fw/` 512 B + 16 KB 日志缓冲)。采集中断里不打日志。
  - **BLE 发送**:主线程一次发完一帧(`bt_gatt_notify_cb`,最多 10 个通知在栈里),发完再采下一帧;`fw/` 是主循环轮询 `bleService()` 往 SoftDevice 队列里填(队列 1)。采集和发送的先后关系不变。连接参数请求(7.5–15 ms、5 s 后请求)、MTU 247、数据长度 251 和 `fw/` 一致;本板不主动请求 PHY(电脑端请求了 2M)。连接参数更新时写 `0xFF` 到控制特征改在系统工作队列里通知(在 BT RX 线程里直接通知会在发送队列满时死锁,实测卡死过一次)。
  - **压力测试**:RTC0、中断优先级 0(高于采集用的 TIMER1 / GPIOTE 中断,优先级 1);`fw/` 是 RTC2、优先级 2。
  - **没有移植**:`wiring_test.c`;VueBuds 自制板的 PMU、IMU、按键、LED、CLI、关机流程;`cameraDeInit` / `cameraEnableStandbyMode`;音频 / 时间同步控制命令(`fw/` 里也没有实现)。
  - BLE 地址是随机静态地址:HM0360 板 `DA:E7:DE:69:E3:5E`、HM01B0 板 `EA:F1:6D:2A:C0:BB`。
- **验证**(两块 nRF5340 DK,REV1,出厂开着 APPROTECT,先 `nrfjprog --recover`):
  - **型号**:J-Link `1050035314` = HM0360,`1050017384` = HM01B0(开机读 ID 0x0360 / 0x01B0)。
  - **推流 QVGA**:HM0360 每帧 `dma 38400 38400`、91.1 ms;HM01B0 `39528 39204`、92.6 ms。viewer 26 帧 / 25 s,0 丢包,**session_fps 1.21–1.38**(52840 约 1.0)。
  - **推流 QQVGA**:HM0360 `19200`,61 帧 0 坏帧,3.77 fps;HM01B0 `19602`,74 帧 0 坏帧,4.16 fps(第一帧 `OVERFLOW, BAD` 在 52840 上也有,10-02 日志)。
  - **彩条**:HM0360 QVGA 竖条笔直、无噪点。
  - **压力测试**:HM0360 QVGA `CAPTURE_STRESS_US=300` 42 帧 0 坏帧;HM01B0 QVGA `2000` 24 帧 0 坏帧。
  - **Monitor**:HM0360 QQVGA 28 帧序号连续,16 槽循环,周期 500 ms,唤醒 → 第一个 FVLD 76 ms → 存完 122 ms,MCLK 每次开 135–136 ms,唤醒前 XSLEEP 低 / MCLK 关;HM0360 QVGA 22 帧 5 槽循环,FVLD 128–130 ms,MCLK 开 233–235 ms;HM01B0 QVGA / QQVGA 各 22 帧,4 / 16 槽循环,MCLK 常开;`-PeriodMs 0` 每帧都存。全部 0 坏帧、0 overrun;开机前几次睡眠超时强制(QQVGA 2 次,QVGA 4 次)。
  - **改成模式 1 + 帧池用满剩余 RAM 后的回归**(上面的数字是改之前测的):
    - 推流:HM0360 QVGA 16 帧 0 坏帧(5 槽);HM0360 QQVGA 板子静止 40 s 139 帧 0 坏帧、0 丢包、3.75 fps(21 槽);HM01B0 QVGA 14 帧,只有第 1 帧 `OVERFLOW`(5 槽);HM01B0 QQVGA 49 帧,只有第 1 帧 `OVERFLOW`。实景画面两颗传感器都清楚。
    - 压力测试 `CAPTURE_STRESS_US=2000`:HM0360 QVGA 22 帧、HM01B0 QVGA 22 帧,都是 0 坏帧(HM0360 第一次跑 2000 µs)。
    - Monitor:HM0360 QQVGA 28 帧序号连续、16 槽循环、周期 500 ms、MCLK 开 135–137 ms;HM0360 QVGA 28 帧、5 槽循环、MCLK 开 232–235 ms;HM01B0 QVGA 29 帧、5 槽循环。全部 0 坏帧、0 overrun,唤醒前 XSLEEP 低 / MCLK 关。有一次 HM0360 QQVGA 每次睡眠都超时(MCLK 开 153 ms),马上重跑两次都正常,和下面的 FVLD 接触问题一致。
- **遗留 / 下一步**:
  - ~~HM0360 噪点是场景太暗~~:这个判断是错的。真正原因是 SPIS 采样边沿,已改成模式 1(见上)。
  - HM0360 板移动时偶发一帧采集跨好几帧(QQVGA 一帧最长 4.5 s,`OVERFLOW`):数行计数在帧中间被清零,像是 FVLD / LVLD 跳线接触不良。板子静止 40 s 139 帧 0 坏帧。建议检查 P1.09 / P1.10 两根线。
  - 模式 1 下 HM01B0 QVGA 推流第 1 帧也会 `OVERFLOW, BAD`(只有第 1 帧,丢弃不发;QQVGA 第 1 帧在 52840 上也有)。
  - 一次刚烧录后紧接着复位,HM0360 整次运行都不应答 I2C,之后重试 6 次都正常,没能复现;和 CLAUDE.md 记录的模块 MCLK 焊点接触不良的现象一致,需要检查接线。
  - `wiring_test.c` 还没移植。

---

## 2026-10-02 · 问题 3:HM0360 Monitor 改成不跳帧,每次唤醒 MCLK 少开约 30%(PR #28,Issue #27)

- **阶段**:调查 / 实验工具
- **改了什么**:
  - `fw/monitor.c`:**`MONITOR_SKIP_FRAMES` 默认 1 → 0**(用户 10-02 决定);`fw/Makefile` 注释 / 帮助同步。
  - `fw/monitor.c`:`MONITOR_FIRST_FRAMES_TEST` 实验:每次 HM0360 唤醒连续存第 1、2、3 帧(第 3 帧覆盖第 1 帧的槽),打印三帧平均亮度、|第1−第2| 和 |第2−第3|(后者就是正常帧之间的噪声);帧边界睡眠抽成 `sleep_on_frame_boundary()`,实验和正常流程共用。
  - `fw/sensors/hm0360/hm0360_sensor.c`:`HM0360_PMU_CFG_5` / `HM0360_PMU_CFG_6` 可在编译时覆盖 pre-meter 寄存器 0x3026 / 0x3027。
  - `fw/tools/dk.ps1`:`-Cflags '<defines>'`(每组 define 一个输出目录;已随 PR #32 先合并)。
- **为什么**:问题 3,Monitor 每次唤醒 MCLK 开 339 / 195 ms,只有约 91 / 46 ms 是有用的数据行。
- **和原代码的行为差异**:HM0360 Monitor 每次唤醒存第 1 帧,不再跳过 1 帧(10-01 定的是跳 1 帧)。实验开关默认关。
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
- **默认改成不跳帧后的验证**(合并 PR #32 之后,PPI 帧边界睡眠):QVGA 20 s 38 帧,唤醒 → 存完 223 ms,**MCLK 开 235–236 ms**(原 339);QQVGA 38 帧,123–124 ms,**MCLK 开 136–137 ms**(原 195);0 坏帧、0 overrun,平均亮度 112–113 和跳帧时相同。开机头两次睡眠由超时强制(PR #32 记录的启动期现象)。
- **遗留 / 下一步**:
  - 只在静止的室内场景测过;睡眠期间光线突变时第 1 帧的曝光还没验证(唤醒时会做 pre-meter,理论上就是为了这种情况)。如果发现不行,`MONITOR_SKIP_FRAMES=1` 改回即可。
  - 剩下的 131 ms 唤醒等待主要是曝光时间;可以限制自动曝光上限来缩短(暂不做)。

## 2026-10-02 · 方案 A:CS 和 DMA 换段全部交给硬件(PPI),PPI 拉低 XSLEEP(PR #32,Issue #31)

- **阶段**:修 bug(Issue #13 两种现象 + HM01B0 坏帧;用户 10-02 选定方案 A)
- **改了什么**:
  - `fw/capture/capture.c`(重写):CS(P0.12)和 XSLEEP(P1.04)改成 GPIOTE 任务引脚;TIMER2 计数 LVLD↓(FVLD 边沿清零),四个比较通道:CC1 = `first_line`(HM01B0 帧开始)、CC2 = 分段边界、CC0 = 最后一行、CC3 = `first_line + 1`(通知 CPU 预写下一段)。三个 PPI 通道组由硬件事件开关:arm(帧开始触发 → CS 低,关掉自己、打开 run)、run(边界 → CS 高 + 启动 TIMER4,TIMER4 2 µs 后 → CS 低;最后一行 → CS 高、关掉 run)、sleep(FVLD↓ → XSLEEP 低,关掉自己)。TIMER4 从"312 µs 延时"改成 2 µs 脉冲。HM0360 帧开始用 FVLD 边沿触发(第 0 行在 750 µs 后)。SPIS 去掉 END→ACQUIRE、关掉 END 中断,CPU 在上一段期间直接写下一段的 RXD.PTR / MAXCNT;跳帧改成按每帧最后一行(CC0)计数。
  - `fw/capture/capture.h`:说明更新;去掉 `ended_by_line_count`;新增 `capture_xsleep()`、`capture_xsleep_on_frame_boundary()`。
  - `fw/camera.c/.h`:`cameraSleep()` 换成 `cameraWake()`、`cameraSleepOnFrameBoundary()`(PPI 在帧边界拉低 XSLEEP,app_timer 超时保护:采完后 30 ms、其它时候 250 ms)、`cameraAsleep()`(`EVENT_CAMERA_ASLEEP` 后关 MCLK)。
  - `fw/monitor.c/.h`、`fw/main.c`、`fw/event.h`:Monitor 不再忙等 FVLD↓;新增 `EVENT_CAMERA_ASLEEP` → `monitorAsleep()`,每次睡下打印 MCLK 开了多久;开机后的第一次睡眠也在帧边界。
  - 新增 `fw/stress_test.c`(`-DCAPTURE_STRESS_US=N` 才编译):RTC2 每约 7 ms 一次优先级 2 的中断,空转 N µs,用来验证采集不依赖中断时机。`fw/tools/dk.ps1` 新增 `-Cflags`(和 PR #28 相同的改动)。
  - `fw/sensors/camera_sensor.h`:注明 `fvld_to_cs_us` 不再使用。
- **为什么**:原来每个 CS 边沿(帧开始、分段边界、最后一行)都在中断里拨,SoftDevice 能把中断推迟几十到几百 µs,而 HM01B0 帧开始和分段边界的窗口只有约 52 µs。
- **和原代码的行为差异**:
  - VueBuds 原设计是 FVLD↑ 中断启动 TIMER4、312 µs 后 TIMER4 中断拉低 CS,分段边界在 LVLD 中断里拉 1 µs 脉冲,换段在 SPIS 中断里。现在全部是 PPI。HM01B0 的 CS 拉低时刻从"FVLD↑ 后 312 µs"变成"第 0 行刚结束"(仍然跳过第 0 行,DMA 字节数不变);HM0360 从"FVLD↑ 后 20 µs"变成"FVLD 边沿"。
  - LVLD 不再有中断;中断只剩 TIMER2 的三个比较(每帧 2–4 次)和 FVLD(记时间、睡眠)。
  - 多用了 2 个 GPIOTE 通道、6 个 PPI 通道、3 个 PPI 通道组;TIMER4 用途改变。
- **验证**:
  - 推流(无压力):HM01B0 QVGA 90 s **101 帧 0 坏帧**、1.11 fps(之前同一块板 7–10% 坏帧、0.85–0.98 fps);HM01B0 QQVGA 40 s 155 帧 0 坏帧(之前 10%);HM0360 QVGA / QQVGA 0 坏帧,`38400 38400` / `19200`,90.9 / 45.7 ms。
  - **压力测试**(每约 7 ms 一次 300 µs 高优先级中断):

| 板 | main(中断拨 CS) | 本 PR |
|---|---|---|
| HM01B0 QVGA 60 s | 69 帧里 **9 帧坏帧**(帧开始晚、分段错位都有) | 62 帧 **0** |
| HM0360 QVGA 60 s | 66 帧里 **7 帧坏帧** | 60 帧 **0** |
| HM01B0 QVGA,2000 µs 压力 | — | 44 帧 **0** |

  - SPIS 换段前提实验:在第 1 段接收中途写 RXD.PTR,第 1 段不受影响(第 20 行后 0 个零字节),第 2 段完整落到新地址,两块板都是。
  - HM0360 Monitor:QVGA 唤醒 → 第一个 FVLD 131 ms、MCLK 开 339–340 ms;QQVGA 77 / 195 ms;各 20 s 38 帧,0 坏帧、0 overrun。HM01B0 Monitor QVGA 38 帧 0 坏帧。接线检测版本能编译。
- **发现**:**HM0360 开始出帧后的一段时间(> 250 ms)和开机后第一次唤醒的那一帧,FVLD 在帧与帧之间不下降**。所以开机后前两次睡眠由超时强制、第一次唤醒多采一帧(存完 429 ms,正常 326 ms),从第二次唤醒起正常。PR #26 的忙等版本同样碰到过(第一次睡眠等满 30 ms),原来的版本则是开机第一次唤醒 540 ms + 1 次 overrun。
- **遗留 / 下一步**:PR #28(唤醒实验工具)要和这个 PR 合并后的 `monitor.c` 对齐。

## 2026-10-02 · HM01B0 初始化加软件复位,换模式后第一次初始化就生效(PR #30,Issue #29)

- **阶段**:修 bug(用户 10-02 同意)
- **改了什么**:
  - `fw/sensors/hm01b0/hm01b0_sensor.c`:新增 `hm01b0_reset()`:先写待机(`0x0100 = 0`),等 110 ms(比最长的 QVGA 帧 97.8 ms 长)让正在出的那一帧结束,再软件复位(`0x0103 = 0x01`,和 ESP32 驱动一致),等 10 ms。`hm01b0_init()` 在读到 ID 之后、写寄存器表之前调用它。
  - `fw/i2c.c/.h`:新增 `i2cWrite16NoVerify()`(写完不回读),给软件复位这种只写 / 自动清零的寄存器用。
  - `fw/sensors/hm01b0/private_include/hm01b0_regs.h`:`HM01B0_SOFTWARE_RESET`、`HM01B0_RESET_RECOVERY_MS`、`HM01B0_FRAME_DRAIN_MS`。
- **为什么**:HM01B0 模块没有 RST,DK 复位不断电;换了模式的固件后第一次初始化有时不生效(帧周期还是旧模式、每帧 `19602 OVERFLOW`)。
- **和原代码的行为差异**:VueBuds 原初始化没有软件复位;现在初始化多了约 120 ms,且自动曝光每次都从默认值开始(和冷启动一样)。
- **验证**(HM01B0 板):
  - 只加软件复位(等 1 ms 或 10 ms):QVGA → QQVGA 3 次里仍有 1 次不生效(每帧 45 ms、`19602 OVERFLOW`)。推测传感器正在出帧时收到复位,要等这一帧结束才执行,把复位后马上写进去的寄存器表清掉了。等 1 ms 时还偶尔多出 `0x3057` / `0x0343` 回读失败。
  - 加上"先待机、等 110 ms":QVGA / QQVGA 交替 **10 次全部第一次就正常**,0 坏帧,没有新的写入失败(只剩原来就有的 `0x3052`)。
  - 副作用:QVGA 存下的第 1 帧平均亮度约 135,第 2 帧起约 100(自动曝光从默认值收敛);QQVGA 第 1 帧就正常。
  - 推流回归:和 main 交替各 3 次 × 40 s,0.84–0.85 vs 0.84–0.88 fps,坏帧数相当(这个时段整体比上午慢)。
- **遗留 / 下一步**:无。

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
