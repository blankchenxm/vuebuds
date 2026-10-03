# nRF52840 DK helper: build, flash and capture RTT logs on Windows.
#   .\tools\dk.ps1 build              # compile banji_dev with RTT logging
#   .\tools\dk.ps1 flash-sd           # program SoftDevice S140 7.2.0 (once)
#   .\tools\dk.ps1 flash              # build + program app + reset
#   .\tools\dk.ps1 rtt -Seconds 20    # capture RTT channel 0 to _build_win\rtt.log
#   .\tools\dk.ps1 run -Seconds 20    # build + flash + capture RTT
#   .\tools\dk.ps1 camera -Frames 3   # reset, pull frames over BLE into _build_win\frames_<board>, show RTT log
#   .\tools\dk.ps1 run -Mode QQVGA -ColorBar   # camera mode / color bar test pattern (build, flash, run)
#   .\tools\dk.ps1 run -WiringTest            # HM0360 wiring / state check (wiring_test.c), prints PASS/FAIL
#   .\tools\dk.ps1 run -Monitor -Mode QQVGA   # Monitor build (CAM_APP=MONITOR), starts at boot
#   .\tools\dk.ps1 run -Monitor -PeriodMs 0   # Monitor storing every frame (default period 500 ms)
#   .\tools\dk.ps1 run -Board HM01B0          # pick the DK when both are plugged in (default HM0360)
#   .\tools\dk.ps1 run -Monitor -Cflags '-DMONITOR_FIRST_FRAMES_TEST -DMONITOR_SKIP_FRAMES=0'  # experiment defines
param(
  [Parameter(Position = 0)][ValidateSet('build', 'flash-sd', 'flash', 'rtt', 'run', 'camera', 'erase')]
  [string]$Command = 'run',
  [int]$Seconds = 15,
  [int]$Frames = 3,
  [ValidateSet('QVGA', 'QQVGA')][string]$Mode = 'QVGA',
  [switch]$ColorBar,
  [switch]$WiringTest,
  [switch]$Monitor,
  [int]$PeriodMs = -1,
  [switch]$NoLog,
  [ValidateSet('HM0360', 'HM01B0')][string]$Board = 'HM0360',
  # Extra C defines for experiments, e.g. -Cflags '-DMONITOR_FIRST_FRAMES_TEST -DMONITOR_SKIP_FRAMES=0'
  [string]$Cflags = ''
)

$ErrorActionPreference = 'Stop'
$FwDir = Split-Path -Parent $PSScriptRoot
# One output directory per build variant: the SDK makefiles do not rebuild when only flags change.
$OutDir = '_build_win'
if ($NoLog) { $OutDir += '_nolog' }
if ($Mode -ne 'QVGA') { $OutDir += '_' + $Mode.ToLower() }
if ($ColorBar) { $OutDir += '_colorbar' }
if ($WiringTest) { $OutDir += '_wiring' }
if ($Monitor) { $OutDir += '_monitor' }
if ($PeriodMs -ge 0) { $OutDir += "_p$PeriodMs" }
if ($Cflags) { $OutDir += '_' + (($Cflags -replace '-D', '' -replace '[^A-Za-z0-9]+', '_').Trim('_').ToLower()) }
$Hex = Join-Path $FwDir "$OutDir\banji_dev.hex"
$SdHex = Join-Path $FwDir 'sdk\components\softdevice\s140\hex\s140_nrf52_7.2.0_softdevice.hex'
# Both DKs run the same firmware (the sensor is detected at boot); they differ in probe and BLE address.
$Boards = @{
  'HM0360' = @{ Snr = '1050291681'; Address = 'C8:1A:80:9C:01:BC' }
  'HM01B0' = @{ Snr = '1050221517'; Address = 'D6:25:79:FD:6A:6B' }
}
$Snr = $Boards[$Board].Snr
$RttLog = Join-Path $FwDir "_build_win\rtt_$($Board.ToLower()).log"

$GccBin = if ($env:ARM_GCC_BIN) { $env:ARM_GCC_BIN } else { 'C:\Program Files (x86)\GNU Arm Embedded Toolchain\10 2021.10\bin' }
$JLinkDir = if ($env:JLINK_DIR) { $env:JLINK_DIR } else { 'C:\Program Files\SEGGER\JLink' }
$GitBash = if ($env:GIT_BASH) { $env:GIT_BASH } else {
  Join-Path (Split-Path -Parent (Split-Path -Parent (Get-Command git).Source)) 'bin\bash.exe'
}

function Get-ShortPath([string]$Path) {
  if (-not (Test-Path $Path)) { throw "Not found: $Path" }
  (New-Object -ComObject Scripting.FileSystemObject).GetFolder($Path).ShortPath
}

function Invoke-Build {
  # SDK makefiles need a POSIX shell and a toolchain path without spaces.
  $gcc = (Get-ShortPath $GccBin) -replace '\\', '/'
  $rtt = if ($NoLog) { '' } else { 'RTT_LOG=1' }
  $pattern = if ($ColorBar) { 'CAM_TEST_PATTERN=COLOR_BAR' } else { '' }
  $fw = $FwDir -replace '\\', '/'
  $defs = (@($(if ($WiringTest) { '-DWIRING_TEST' }), $Cflags) | Where-Object { $_ }) -join ' '
  $extra = if ($defs) { "CFLAGS='$defs' " } else { '' }
  $app = if ($Monitor) { 'CAM_APP=MONITOR' } else { 'CAM_APP=STREAM' }
  if ($PeriodMs -ge 0) { $app += " MONITOR_PERIOD_MS=$PeriodMs" }
  $cmd = "cd '$fw' && ${extra}make banji_dev OUTPUT_DIRECTORY=$OutDir GNU_INSTALL_ROOT=$gcc/ CAM_MODE=$Mode $app $pattern $rtt -j8"
  & $GitBash -lc $cmd
  if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
}

function Invoke-Nrfjprog {
  & nrfjprog -f nrf52 --snr $Snr @args
  if ($LASTEXITCODE -ne 0) { throw "nrfjprog $args failed ($LASTEXITCODE)" }
}

function Start-RttLogger {
  $logger = Join-Path $JLinkDir 'JLinkRTTLogger.exe'
  New-Item -ItemType Directory -Force (Split-Path $RttLog) | Out-Null
  if (Test-Path $RttLog) { Remove-Item $RttLog }
  Start-Process -FilePath $logger -PassThru -WindowStyle Hidden -ArgumentList @(
    '-USB', $Snr, '-Device', 'NRF52840_XXAA', '-If', 'SWD', '-Speed', '4000', '-RTTChannel', '0', "`"$RttLog`"")
}

function Stop-RttLogger($Proc, [string]$Label) {
  if (-not $Proc.HasExited) { Stop-Process -Id $Proc.Id -Force }
  Write-Host "===== RTT log ($Board, $Label) -> $RttLog ====="
  if (Test-Path $RttLog) { Get-Content $RttLog } else { Write-Host '(no RTT data captured)' }
}

function Invoke-Rtt([int]$Secs) {
  $p = Start-RttLogger
  Start-Sleep -Seconds $Secs
  Stop-RttLogger $p "$Secs s"
}

function Invoke-Camera {
  # Reset first so the RTT log starts at boot, then pull frames over BLE.
  Invoke-Nrfjprog --reset
  $p = Start-RttLogger
  Start-Sleep -Seconds 2
  $py = Join-Path $FwDir 'tools\.venv\Scripts\python.exe'
  & $py (Join-Path $FwDir 'tools\ble_receive.py') --address $Boards[$Board].Address --frames $Frames --timeout $Seconds --out (Join-Path $FwDir "_build_win\frames_$($Board.ToLower())")
  $rc = $LASTEXITCODE
  Start-Sleep -Seconds 1
  Stop-RttLogger $p 'camera'
  if ($rc -ne 0) { throw "BLE receive failed ($rc)" }
}

switch ($Command) {
  'build' { Invoke-Build }
  'flash-sd' { Invoke-Nrfjprog --program $SdHex --sectorerase --verify; Invoke-Nrfjprog --reset }
  'erase' { Invoke-Nrfjprog --eraseall }
  'flash' { Invoke-Build; Invoke-Nrfjprog --program $Hex --sectorerase --verify; Invoke-Nrfjprog --reset }
  'rtt' { Invoke-Rtt $Seconds }
  'camera' { Invoke-Camera }
  'run' {
    Invoke-Build
    Invoke-Nrfjprog --program $Hex --sectorerase --verify
    Invoke-Nrfjprog --reset
    Invoke-Rtt $Seconds
  }
}
