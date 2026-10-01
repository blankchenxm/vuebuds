# vuebuds(blankchenxm fork)工作说明

用户的 fork,目标:在 **nRF52840 DK** 上复现 VueBuds 的摄像头 + BLE 传图,并把传感器从 HM01B0 迁移到 HM0360。
**和用户交流用中文。**
**凡是改动 VueBuds 原有设计或行为(采集时序、片选/DMA 方式、中断用法、日志、BLE 协议等),哪怕看起来是修 bug 或加诊断,都要先告诉用户、说明证据和可选方案,由用户决定;"重构"阶段默认保持原设计。PR 和 DEVLOG 里列出所有行为差异。**

## 设计文档(先读)

- 迁移设计与 TODO:`C:\Users\blank\Documents\Obsidian Vault\Research\Proactive Wearable Agent\02-daily_raw\9.28~10.4\Vuebuds Hm0360迁移对比.md`
  - §1 现有实现 · §1.2 / §3.1 接线表 · §1.6 RAM 预算 · §4 时钟结论 · §5 模式与帧环形缓存 · §6 重构结构 · §8 TODO
- 背景:同目录 `01-synthesis/Hardware/HM01B0 vs HM0360.md`、`Camera持续采集Buffer问题.md`
- 已定的决定,不要再推翻:
  - MCLK 由 nRF 提供 8 MHz(HM0360 不能用内部时钟跑 1-bit)。
  - 模式在编译时选(`CAM_MODE=QVGA/QQVGA`),**不做运行时切换**。
  - 帧槽按传感器完整输出分配(HM01B0 QVGA 324×244、QQVGA 162×122),**固件发 BLE 时裁成标准 320×240 / 160×120**;BLE 帧头带宽高。HM0360 用窗口模式 `0x3030[0]=1` 直接输出标准尺寸(笔记 §5.3)。
  - **片选时序保持 VueBuds 原设计**:FVLD 上升 → TIMER4 延时(QVGA 312 µs)→ 拉低 CS。HM01B0 第 0 行紧跟 FVLD 开始,这个延时**有意跳过第 0 行**,所以 DMA 收第 1–243 行(`mode_info.first_line = 1`),槽里第 0 行为空,标准裁剪 y=2 不受影响。在 FVLD 上升时就拉 CS(哪怕用 PPI)会丢第一个字节、整段错 1 像素(PR #9 实测)。
  - **Monitor 只做 HM0360**,用 S2;不开运动检测;不做触发/回传;不做 VGA。
  - RAM 保持现状(RTT 保留,日志缓冲 16 KB、堆 8 KB 不缩小)。

## 仓库结构

- `fw/`:nRF5 SDK 17.1.0 裸机固件(Makefile,**不是** Zephyr / NCS)。DK 目标是 `banji_dev`(`-DBOARD_PCA10056 -DFF=0`);`banji` 是作者的自制板,不要在 DK 上烧。
- 固件里的摄像头分三层:`fw/sensors/`(公共接口 `camera_sensor.h` + `hm01b0/`,结构照 ESP32 驱动:`include/`、`private_include/`、寄存器表 `hm01b0_modes.c`、`mode_info`)→ `fw/capture/`(nRF 专用:MCLK、帧缓冲池、SPIS + 片选 + DMA 自动分段)→ `fw/camera.c`(编排,`CAMERA_MODE` / `CAMERA_TEST_PATTERN` 编译时宏)。BLE 发送在 `ble_manager.c` 的 `bleSendFrame()`,直接从槽里按标准区域逐行读。
- 帧缓冲池 = 链接脚本里 `__frame_pool_start`(堆尾)到 `__frame_pool_end`(栈底)之间的全部 RAM,约 208 KB。
- `fw/sdk/`:整份 SDK(已打好 `sdk_patch/nrfx_spis*` 补丁);`fw/_build/` 是作者在 macOS 上的编译产物,不要改;Windows 编译输出在 `fw/_build_win/`(已 gitignore)。
- `host/`:电脑端 `viewer.py`(BLE 实时预览,空格存图,`--rtt --reset` 合并固件日志)。
- `hw/`:KiCad 硬件。**用户可能有未提交的 KiCad 改动和锁文件,不要提交、不要还原。**

## 环境与命令(Windows)

| 工具 | 位置 |
|---|---|
| Arm GCC 10.3-2021.10 | `C:\Program Files (x86)\GNU Arm Embedded Toolchain\10 2021.10\bin`(make 里用短路径 `C:/PROGRA~2/GNUARM~1/102021~1.10/bin/`) |
| make | `C:\MinGW\bin\make.exe`;**SDK Makefile 依赖 POSIX shell,必须在 Git Bash 里跑**(Git 在 `D:\Git\Git`) |
| nrfjprog | PATH 中 |
| SEGGER J-Link | `C:\Program Files\SEGGER\JLink`(PATH 里的 `jlink` 是 Java 的,不是它) |
| Python venv | `fw/tools/.venv`(bleak、pillow)、`host/.venv`(bleak、opencv、numpy) |

常用(在 `fw/` 下,PowerShell):
```powershell
.\tools\dk.ps1 build          # 编译 banji_dev(带 RTT_LOG=1)
.\tools\dk.ps1 flash-sd       # 只在擦除芯片后需要:烧 S140 7.2.0
.\tools\dk.ps1 run -Seconds 20 # 编译 + 烧录 + 抓 RTT 日志
.\tools\dk.ps1 camera -Frames 3 # 复位 + 通过 BLE 收图存到 _build_win\frames\ + 打印 RTT
.\tools\dk.ps1 run -Mode QQVGA -ColorBar  # 选模式(默认 QVGA)/ 彩条测试图;每种组合单独输出目录 _build_win_qqvga_colorbar
..\host\.venv\Scripts\python.exe ..\host\viewer.py --rtt --reset --duration 20 --snapshot ..\host\logs\shot.png
```
- 直接用 make:`make banji_dev CAM_MODE=QQVGA CAM_TEST_PATTERN=COLOR_BAR OUTPUT_DIRECTORY=_build_win_qqvga_colorbar ...`。**只改这些参数时 SDK Makefile 不会重新编译**,所以每种组合要用不同的 `OUTPUT_DIRECTORY`(`dk.ps1` 已自动处理)。
- 在 bash 里直接调 make 要额外 CFLAGS 时,用环境变量:`CFLAGS=-DXXX make banji_dev ...`;**不要**在命令行写 `CFLAGS+=`,会覆盖 Makefile 里的全部参数。
- 设备:J-Link SN `1050221517`;BLE 名 `mustard`,地址 `D6:25:79:FD:6A:6B`;数据特征 `47ea1402-a0e4-554e-5282-0afcd3246970`(notify),控制 `47ea1403-…`,写 `0xB1` 开始推流(`ble_cus.h` 注释里的 UUID 字节序是错的)。
- 诊断:RTT 是主要手段;读 RAM 变量/寄存器可以用 `JLink.exe -CommandFile`(一次会话批量读,比逐条 `nrfjprog --memrd` 快很多);`arm-none-eabi-nm` 查变量地址。

## 已知坑

- **J-Link 同时只能一个工具稳定使用**:用户的 JLinkRTTViewer 开着时会冲突;`nrfjprog --reset` 必须在启动 JLinkRTTLogger **之前**做,否则报 `Cannot connect to J-Link`。
- **PowerShell 5.1 传给原生程序的参数里有引号/特殊字符会被拆乱**:commit message 用 `git commit -F <文件>`,PR 正文用 `gh pr create --body-file <文件>`(文件写到 scratchpad)。
- `NRF_LOG_DEFERRED=0`(立即输出日志)会拖慢中断,导致帧上下半错位;调试时临时用可以,别留下。
- 推流时 RTT 日志:PR #9 起 `idle()` 推流时也处理日志、去掉了 `sent NkB` 刷屏,每帧一行 `[cam] frame N: … dma <段1字节> <段2字节>`;看不到时先查这里。
- HM01B0 尺寸寄存器(`sensors/hm01b0/hm01b0_modes.c` 的模式表)末尾必须写 `GROUP_PARAMETER_HOLD 0x0104 = 1`(HM0360 对应 `COMMAND_UPDATE 0x0104`),否则参数不生效。
- 每帧 DMA 字节数是对齐的最好证据:HM01B0 QVGA 应为 `39528 39204`(122 + 121 行 × 324),QQVGA 应为 `19602`(121 行 × 162);不是行宽整数倍就说明丢字节/错位。
- HM01B0 黑白传感器的"彩条"测试图显示成棋盘格,奇偶行不同是正常的;看彩条边界是否上下笔直来判断对齐。条宽固定约 52 个输出像素,所以 QVGA 看到 6 条、QQVGA 只看到 3 条,正常。
- PowerShell 里不要用 `viewer.py … | Select-Object -First N`:管道提前结束会把 viewer 一起杀掉,时长和截图都不完整。输出重定向到 `$null`,再从 `host/logs/session-*.log` 统计。
- Windows 偶尔在 BLE 刚连上时取消连接(`操作已被用户取消`),viewer 会自动重连;但固件断连会 `NVIC_SystemReset`,而 JLinkRTTLogger 在芯片重启后不会重新连上 RTT,这一次会话就没有固件日志了,重跑即可。
- 在 bash heredoc 里用 Python 改 C 文件时,`\\n` 会被转义成真换行;含反斜杠的改动用 Edit 工具。
- HM0360 datasheet:`D:\Projects\Proactive camera agent\HM0360.pdf`。Read 工具会误报"加密",用 pypdf 提取文字即可(图 6.4 时钟分频图在笔记里有截图)。
- 传感器驱动结构参照 ESP32 驱动:GitHub `blankchenxm/hm01b0-esp-idf-driver` → `components/hm01b0/`(未克隆到本地,用 `gh api` 读)。

## GitHub 流程

- 每个任务:开 Issue → 建分支(`feat/…`、`fix/…`、`refactor/…`)→ 实测 → 开 PR → 合并。
- **所有 `gh` 命令都显式加 `--repo blankchenxm/vuebuds`,PR 用 `--base main`**:仓库是 fork,默认会指向上游 `uw-x/vuebuds`,绝不能提到上游。
- PR 正文写清楚:改了什么、怎么在板子上验证的(RTT 片段、帧数/丢包/FPS、截图结论)、已知问题、`Closes #N`。
- 每个 PR 都要包含 `DEVLOG.md` 的新条目(见下方"开发日志")。
- 用 `--merge` 保留提交历史;合并后同步本地 `main`,删除已合并分支(本地 + 远端)。
- 合并前默认先问用户,除非用户在当次对话里说了直接合并。
- 不要提交拍到用户本人的图片:`host/logs/`、`host/captures/`、`fw/_build_win/` 都已 gitignore,保持这样。

## 开发日志(每个阶段都要写)

每个合并进 `main` 的 PR,都要在仓库根目录 `DEVLOG.md` **最上面**追加一条,**和代码放在同一个 PR 里提交**(不是合并后再补)。格式:

```markdown
## YYYY-MM-DD · 一句话标题(PR #N,Issue #M)

- **阶段**:TODO 第几步 / 工具 / 文档 / 修 bug
- **改了什么**:按文件列出主要改动
- **为什么**:动机或对应的决定
- **验证**:在板子上怎么测的、关键数据(帧数、丢包、FPS、RTT 关键行、.bss 大小等)
- **遗留 / 下一步**:没解决的问题、发现的新问题
```

- 日期用用户本地日期。PR 编号在开 PR 前不知道时,先写 `PR #?`,开完 PR 后改成实际编号再合并。
- 写人能看懂的结论,不要粘大段日志。
- 设计上的新决定除了写进日志,也要同步到 Obsidian 笔记和本文件的"已定的决定"。

## TODO 各步的验收标准(笔记 §8)

1. ✅(PR #9)**重构(HM01B0)**:viewer 看到**标准 320×240** 画面(324×244 的槽,DMA 按原时序收第 1–243 行,发送时裁剪),无黑带/错位;FPS 约 0.7–0.9;0 丢包;BLE 帧头带宽高,viewer 按帧头适配;`arm-none-eabi-size` 确认 .bss 少了约 77 KB;帧缓冲池由链接器分配到剩余 RAM。注意画面和重构前不完全一样(左右少 2 列边框、底部不再缺 5 行),这是预期的。
2. ✅(PR #11)**编译时选模式(HM01B0)**:`CAM_MODE=QVGA` 和 `CAM_MODE=QQVGA` 两种编译结果都正确显示(320×240 / 160×120),彩条测试图正确,0 丢包。
3. **HM0360 驱动 + 上板**:I2C 读到 ID 0x0360;彩条测试图下 SPIS 收到的字节数 = 宽×高(验证 PCLKO = 8 MHz);QQVGA 再 QVGA 真实画面正常;viewer 看到的尺寸和 HM01B0 一样。
4. **HM0360 Monitor(只做缓存)**:RTT 日志显示槽号循环、帧序号连续、时间戳间隔符合 RTC 周期;QQVGA 10 槽、QVGA 2 槽;帧间 XSLEEP 为低、MCLK 关闭。
