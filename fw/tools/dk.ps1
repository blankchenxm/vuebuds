# nRF52840 DK helper: build, flash and capture RTT logs on Windows.
#   .\tools\dk.ps1 build              # compile banji_dev with RTT logging
#   .\tools\dk.ps1 flash-sd           # program SoftDevice S140 7.2.0 (once)
#   .\tools\dk.ps1 flash              # build + program app + reset
#   .\tools\dk.ps1 rtt -Seconds 20    # capture RTT channel 0 to _build_win\rtt.log
#   .\tools\dk.ps1 run -Seconds 20    # build + flash + capture RTT
#   .\tools\dk.ps1 camera -Frames 3   # reset, pull frames over BLE into _build_win\frames, show RTT log
param(
  [Parameter(Position = 0)][ValidateSet('build', 'flash-sd', 'flash', 'rtt', 'run', 'camera', 'erase')]
  [string]$Command = 'run',
  [int]$Seconds = 15,
  [int]$Frames = 3,
  [switch]$NoLog
)

$ErrorActionPreference = 'Stop'
$FwDir = Split-Path -Parent $PSScriptRoot
$OutDir = if ($NoLog) { '_build_win_nolog' } else { '_build_win' }
$Hex = Join-Path $FwDir "$OutDir\banji_dev.hex"
$SdHex = Join-Path $FwDir 'sdk\components\softdevice\s140\hex\s140_nrf52_7.2.0_softdevice.hex'
$RttLog = Join-Path $FwDir '_build_win\rtt.log'

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
  $fw = $FwDir -replace '\\', '/'
  $cmd = "cd '$fw' && make banji_dev OUTPUT_DIRECTORY=$OutDir GNU_INSTALL_ROOT=$gcc/ $rtt -j8"
  & $GitBash -lc $cmd
  if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
}

function Invoke-Nrfjprog {
  & nrfjprog -f nrf52 @args
  if ($LASTEXITCODE -ne 0) { throw "nrfjprog $args failed ($LASTEXITCODE)" }
}

function Start-RttLogger {
  $logger = Join-Path $JLinkDir 'JLinkRTTLogger.exe'
  New-Item -ItemType Directory -Force (Split-Path $RttLog) | Out-Null
  if (Test-Path $RttLog) { Remove-Item $RttLog }
  Start-Process -FilePath $logger -PassThru -WindowStyle Hidden -ArgumentList @(
    '-Device', 'NRF52840_XXAA', '-If', 'SWD', '-Speed', '4000', '-RTTChannel', '0', "`"$RttLog`"")
}

function Stop-RttLogger($Proc, [string]$Label) {
  if (-not $Proc.HasExited) { Stop-Process -Id $Proc.Id -Force }
  Write-Host "===== RTT log ($Label) -> $RttLog ====="
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
  & $py (Join-Path $FwDir 'tools\ble_receive.py') --frames $Frames --timeout $Seconds --out (Join-Path $FwDir '_build_win\frames')
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
