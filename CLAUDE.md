# vuebuds(blankchenxm fork)工作说明

用户的 fork,目标:在 **nRF52840 DK** 上复现 VueBuds 的摄像头 + BLE 传图,并把传感器从 HM01B0 迁移到 HM0360。
**和用户交流用中文。**

## 设计文档(先读)

- 迁移设计与 TODO:`C:\Users\blank\Documents\Obsidian Vault\Research\Proactive Wearable Agent\02-daily_raw\9.28~10.4\Vuebuds Hm0360迁移对比.md`
  - §1 现有实现 · §1.2 / §3.1 接线表 · §1.6 RAM 预算 · §4 时钟结论 · §5 模式与帧环形缓存 · §6 重构结构 · §8 TODO
- 背景:同目录 `01-synthesis/Hardware/HM01B0 vs HM0360.md`、`Camera持续采集Buffer问题.md`
- 已定的决定,不要再推翻:
  - MCLK 由 nRF 提供 8 MHz(HM0360 不能用内部时钟跑 1-bit)。
  - 模式在编译时选(`CAM_MODE=QVGA/QQVGA`),**不做运行时切换**。
  - DMA 收传感器完整输出(HM01B0 QVGA 324×244、QQVGA 162×122),**固件发 BLE 时裁成标准 320×240 / 160×120**;BLE 帧头带宽高。HM0360 用窗口模式 `0x3030[0]=1` 直接输出标准尺寸(笔记 §5.3)。
  - **Monitor 只做 HM0360**,用 S2;不开运动检测;不做触发/回传;不做 VGA。
  - RAM 保持现状(RTT 保留,日志缓冲 16 KB、堆 8 KB 不缩小)。

## 仓库结构

- `fw/`:nRF5 SDK 17.1.0 裸机固件(Makefile,**不是** Zephyr / NCS)。DK 目标是 `banji_dev`(`-DBOARD_PCA10056 -DFF=0`);`banji` 是作者的自制板,不要在 DK 上烧。
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
..\host\.venv\Scripts\python.exe ..\host\viewer.py --rtt --reset --duration 20 --snapshot ..\host\logs\shot.png
```
- 在 bash 里直接调 make 要额外 CFLAGS 时,用环境变量:`CFLAGS=-DXXX make banji_dev ...`;**不要**在命令行写 `CFLAGS+=`,会覆盖 Makefile 里的全部参数。
- 设备:J-Link SN `1050221517`;BLE 名 `mustard`,地址 `D6:25:79:FD:6A:6B`;数据特征 `47ea1402-a0e4-554e-5282-0afcd3246970`(notify),控制 `47ea1403-…`,写 `0xB1` 开始推流(`ble_cus.h` 注释里的 UUID 字节序是错的)。
- 诊断:RTT 是主要手段;读 RAM 变量/寄存器可以用 `JLink.exe -CommandFile`(一次会话批量读,比逐条 `nrfjprog --memrd` 快很多);`arm-none-eabi-nm` 查变量地址。

## 已知坑

- **J-Link 同时只能一个工具稳定使用**:用户的 JLinkRTTViewer 开着时会冲突;`nrfjprog --reset` 必须在启动 JLinkRTTLogger **之前**做,否则报 `Cannot connect to J-Link`。
- **PowerShell 5.1 传给原生程序的参数里有引号/特殊字符会被拆乱**:commit message 用 `git commit -F <文件>`,PR 正文用 `gh pr create --body-file <文件>`(文件写到 scratchpad)。
- `NRF_LOG_DEFERRED=0`(立即输出日志)会拖慢中断,导致帧上下半错位;调试时临时用可以,别留下。
- 推流时 RTT 日志会延迟/丢失(`sent NkB` 刷屏 + `idle()` 只在有事件时处理日志),对照时以固件毫秒时间戳为准。
- `fw/HM01B0/HM01B0_FUNC.c` 修改尺寸寄存器后必须写 `REG_GRP_PARAM_HOLD`(HM0360 对应 `COMMAND_UPDATE 0x0104`),否则参数不生效。
- HM0360 datasheet:`D:\Projects\Proactive camera agent\HM0360.pdf`。Read 工具会误报"加密",用 pypdf 提取文字即可(图 6.4 时钟分频图在笔记里有截图)。
- 传感器驱动结构参照 ESP32 驱动:GitHub `blankchenxm/hm01b0-esp-idf-driver` → `components/hm01b0/`(未克隆到本地,用 `gh api` 读)。

## GitHub 流程

- 每个任务:开 Issue → 建分支(`feat/…`、`fix/…`、`refactor/…`)→ 实测 → 开 PR → 合并。
- **所有 `gh` 命令都显式加 `--repo blankchenxm/vuebuds`,PR 用 `--base main`**:仓库是 fork,默认会指向上游 `uw-x/vuebuds`,绝不能提到上游。
- PR 正文写清楚:改了什么、怎么在板子上验证的(RTT 片段、帧数/丢包/FPS、截图结论)、已知问题、`Closes #N`。
- 用 `--merge` 保留提交历史;合并后同步本地 `main`,删除已合并分支(本地 + 远端)。
- 合并前默认先问用户,除非用户在当次对话里说了直接合并。
- 不要提交拍到用户本人的图片:`host/logs/`、`host/captures/`、`fw/_build_win/` 都已 gitignore,保持这样。

## TODO 各步的验收标准(笔记 §8)

1. **重构(HM01B0)**:viewer 看到**标准 320×240** 画面(DMA 收完整 324×244,发送时裁剪),无黑带/错位;FPS 约 0.7–0.9;0 丢包;BLE 帧头带宽高,viewer 按帧头适配;`arm-none-eabi-size` 确认 .bss 少了约 77 KB;帧缓冲池由链接器分配到剩余 RAM。注意画面和重构前不完全一样(左右少 2 列边框、底部不再缺 5 行),这是预期的。
2. **编译时选模式(HM01B0)**:`CAM_MODE=QVGA` 和 `CAM_MODE=QQVGA` 两种编译结果都正确显示(320×240 / 160×120),彩条测试图正确,0 丢包。
3. **HM0360 驱动 + 上板**:I2C 读到 ID 0x0360;彩条测试图下 SPIS 收到的字节数 = 宽×高(验证 PCLKO = 8 MHz);QQVGA 再 QVGA 真实画面正常;viewer 看到的尺寸和 HM01B0 一样。
4. **HM0360 Monitor(只做缓存)**:RTT 日志显示槽号循环、帧序号连续、时间戳间隔符合 RTC 周期;QQVGA 10 槽、QVGA 2 槽;帧间 XSLEEP 为低、MCLK 关闭。
