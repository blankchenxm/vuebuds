# nRF5340 DK helper (counterpart of fw/tools/dk.ps1): build, flash and capture RTT logs.
#   .\tools\dk5340.ps1 build                     # west build (app core + ipc_radio net core)
#   .\tools\dk5340.ps1 flash                     # build + program both cores + reset
#   .\tools\dk5340.ps1 rtt -Seconds 20           # reset + capture RTT channel 0
#   .\tools\dk5340.ps1 run -Seconds 20           # build + flash + capture RTT
#   .\tools\dk5340.ps1 run -Board HM0360B        # pick the DK (default HM0360)
#   .\tools\dk5340.ps1 run -Mode QQVGA -ColorBar # camera mode / color bar test pattern
#   .\tools\dk5340.ps1 run -Monitor -PeriodMs 0  # Monitor build (CAM_APP=MONITOR)
#   .\tools\dk5340.ps1 run -Cflags 'CAPTURE_STRESS_US=300'   # extra defines, ';'-separated, no -D
# Zephyr's devicetree step breaks on the spaces in the repo path, so the repo is mapped to
# drive V: (subst) and built from there. Build directories: fw_nrf5340\build_<variant>.
param(
  [Parameter(Position = 0)][ValidateSet('build', 'flash', 'rtt', 'run')]
  [string]$Command = 'run',
  [int]$Seconds = 15,
  [ValidateSet('QVGA', 'QQVGA', 'VGA')][string]$Mode = 'QVGA',
  [switch]$ColorBar,
  [switch]$Monitor,
  [int]$PeriodMs = -1,
  [ValidateSet('HM0360', 'HM0360B')][string]$Board = 'HM0360',
  [string]$Cflags = '',
  [switch]$Pristine   # rebuild from scratch (needed after adding a .conf / overlay file)
)

# west / nrfjprog print progress on stderr: rely on exit codes instead of 'Stop'.
$ErrorActionPreference = 'Continue'
$Repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Drive = 'V:'
$Ncs = if ($env:NCS_DIR) { $env:NCS_DIR } else { 'D:\NRFSDK' }
$Tc = Join-Path $Ncs 'toolchains\66cdf9b75e'
$JLinkDir = if ($env:JLINK_DIR) { $env:JLINK_DIR } else { 'C:\Program Files\SEGGER\JLink' }
$Boards = @{
  'HM0360' = @{ Snr = '1050035314' }
  'HM0360B' = @{ Snr = '1050017384' }  # had the HM01B0 until 2026-10-06
}
$Snr = $Boards[$Board].Snr

$Variant = $Mode.ToLower()
if ($ColorBar) { $Variant += '_colorbar' }
if ($Monitor) { $Variant += '_monitor' }
if ($PeriodMs -ge 0) { $Variant += "_p$PeriodMs" }
# Cflags as a short hash: long build paths break the Windows 260-character limit in ninja.
if ($Cflags) {
  $md5 = [System.Security.Cryptography.MD5]::Create().ComputeHash([System.Text.Encoding]::UTF8.GetBytes($Cflags))
  $Variant += '_x' + (($md5[0..3] | ForEach-Object { $_.ToString('x2') }) -join '')
}
$BuildDir = "$Drive\fw_nrf5340\build_$Variant"
$RttLog = Join-Path $Repo "fw_nrf5340\build_logs\rtt_$($Board.ToLower()).log"

function Use-Drive {
  if (-not (Test-Path "$Drive\fw_nrf5340")) { cmd /c "subst $Drive `"$Repo`"" | Out-Null }
}

function Use-Ncs {
  $env:ZEPHYR_BASE = Join-Path $Ncs 'v3.2.1\zephyr'
  $env:ZEPHYR_TOOLCHAIN_VARIANT = 'zephyr'
  $env:ZEPHYR_SDK_INSTALL_DIR = Join-Path $Tc 'opt\zephyr-sdk'
  $env:NRFUTIL_HOME = Join-Path $Tc 'nrfutil\home'
  $env:PYTHONPATH = (@('opt\bin', 'opt\bin\Lib', 'opt\bin\Lib\site-packages') | ForEach-Object { Join-Path $Tc $_ }) -join ';'
  $paths = @('', 'mingw64\bin', 'bin', 'opt\bin', 'opt\bin\Scripts', 'opt\nanopb\generator-bin', 'nrfutil\bin',
    'opt\zephyr-sdk\arm-zephyr-eabi\bin') | ForEach-Object { Join-Path $Tc $_ }
  if (-not $env:PATH.StartsWith($paths[0])) { $env:PATH = ($paths -join ';') + ';' + $env:PATH }
}

function Invoke-Build {
  Use-Drive
  Use-Ncs
  $pattern = if ($ColorBar) { 'COLOR_BAR' } else { 'OFF' }
  $app = if ($Monitor) { 'MONITOR' } else { 'STREAM' }
  $defs = @()
  if ($PeriodMs -ge 0) { $defs += "MONITOR_PERIOD_MS=$PeriodMs" }
  if ($Cflags) { $defs += $Cflags }
  $extra = $defs -join ';'
  Push-Location "$Drive\fw_nrf5340"
  try {
    $p = if ($Pristine) { 'always' } else { 'auto' }
    # Sysbuild passes -D<image>_<var> to that image only; the app image is named fw_nrf5340.
    & west build -p $p -b nrf5340dk/nrf5340/cpuapp -d $BuildDir . -- "-Dfw_nrf5340_CAM_MODE=$Mode" `
      "-Dfw_nrf5340_CAM_TEST_PATTERN=$pattern" "-Dfw_nrf5340_CAM_APP=$app" "-Dfw_nrf5340_EXTRA_DEFINES=$extra"
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
  } finally { Pop-Location }
}

function Invoke-Flash {
  Use-Drive
  Use-Ncs
  & west flash -d $BuildDir --runner nrfjprog --dev-id $Snr
  if ($LASTEXITCODE -ne 0) { throw "Flash failed ($LASTEXITCODE)" }
}

function Invoke-Rtt([int]$Secs) {
  # Reset first (nrfjprog cannot connect while the RTT logger holds the probe).
  & nrfjprog --snr $Snr --reset | Out-Null
  $logger = Join-Path $JLinkDir 'JLinkRTTLogger.exe'
  New-Item -ItemType Directory -Force (Split-Path $RttLog) | Out-Null
  if (Test-Path $RttLog) { Remove-Item $RttLog }
  $p = Start-Process -FilePath $logger -PassThru -WindowStyle Hidden -ArgumentList @(
    '-USB', $Snr, '-Device', 'nRF5340_xxAA_APP', '-If', 'SWD', '-Speed', '4000', '-RTTChannel', '0', "`"$RttLog`"")
  Start-Sleep -Seconds $Secs
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
  Write-Host "===== RTT log ($Board, $Secs s) -> $RttLog ====="
  if (Test-Path $RttLog) { Get-Content $RttLog } else { Write-Host '(no RTT data captured)' }
}

# The NCS variables (PYTHONPATH especially) would break other Pythons run later in the same
# PowerShell session (host\.venv), so they are put back afterwards.
$SavedEnv = @{}
foreach ($name in 'PATH', 'PYTHONPATH', 'ZEPHYR_BASE', 'ZEPHYR_TOOLCHAIN_VARIANT', 'ZEPHYR_SDK_INSTALL_DIR', 'NRFUTIL_HOME') {
  $SavedEnv[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
  switch ($Command) {
    'build' { Invoke-Build }
    'flash' { Invoke-Build; Invoke-Flash }
    'rtt' { Invoke-Rtt $Seconds }
    'run' { Invoke-Build; Invoke-Flash; Invoke-Rtt $Seconds }
  }
} finally {
  foreach ($name in $SavedEnv.Keys) { [Environment]::SetEnvironmentVariable($name, $SavedEnv[$name], 'Process') }
}
