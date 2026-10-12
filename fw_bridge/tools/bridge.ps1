# BLE-USB bridge helper (52840 DK now; -Target 5340 later): build, flash and capture RTT logs.
#   .\tools\bridge.ps1 build                # west build (nrf52840dk/nrf52840)
#   .\tools\bridge.ps1 flash                # build + program + reset
#   .\tools\bridge.ps1 rtt -Seconds 20      # attach the RTT logger without a reset (the bridge keeps its links)
#   .\tools\bridge.ps1 flash -UsbTest       # USB link test build (host\usb_test.py)
# Build directories: fw_bridge\build (bridge), fw_bridge\build_usbtest. Paths with spaces are
# built through drive V: (subst), like fw_nrf5340\tools\dk5340.ps1.
param(
  [Parameter(Position = 0)][ValidateSet('build', 'flash', 'rtt')]
  [string]$Command = 'flash',
  [int]$Seconds = 15,
  [switch]$UsbTest,
  [switch]$Pristine,
  [string]$Snr = '1050221517'   # the 52840 DK that was the HM01B0 board until 2026-10-10
)

$ErrorActionPreference = 'Continue'
$Repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Drive = if ($Repo -match ' ') { 'V:' } else { $Repo.TrimEnd('\') }
$Ncs = if ($env:NCS_DIR) { $env:NCS_DIR } else { 'D:\NRFSDK' }
$Tc = Join-Path $Ncs 'toolchains\66cdf9b75e'
$JLinkDir = if ($env:JLINK_DIR) { $env:JLINK_DIR } else { 'C:\Program Files\SEGGER\JLink' }
$BuildDir = if ($UsbTest) { "$Drive\fw_bridge\build_usbtest" } else { "$Drive\fw_bridge\build" }
$RttLog = Join-Path $Repo 'fw_bridge\build_logs\rtt_bridge.log'

function Use-Ncs {
  if ($Drive -eq 'V:' -and -not (Test-Path "$Drive\fw_bridge")) { cmd /c "subst $Drive `"$Repo`"" | Out-Null }
  $env:ZEPHYR_BASE = Join-Path $Ncs 'v3.2.1\zephyr'
  $env:ZEPHYR_TOOLCHAIN_VARIANT = 'zephyr'
  $env:ZEPHYR_SDK_INSTALL_DIR = Join-Path $Tc 'opt\zephyr-sdk'
  $env:NRFUTIL_HOME = Join-Path $Tc 'nrfutil\home'
  $env:PYTHONPATH = (@('opt\bin', 'opt\bin\Lib', 'opt\bin\Lib\site-packages') | ForEach-Object { Join-Path $Tc $_ }) -join ';'
  $paths = @('', 'mingw64\bin', 'bin', 'opt\bin', 'opt\bin\Scripts', 'nrfutil\bin', 'opt\zephyr-sdk\arm-zephyr-eabi\bin') |
    ForEach-Object { Join-Path $Tc $_ }
  if (-not $env:PATH.StartsWith($paths[0])) { $env:PATH = ($paths -join ';') + ';' + $env:PATH }
}

function Invoke-Build {
  Use-Ncs
  Push-Location "$Drive\fw_bridge"
  try {
    $p = if ($Pristine) { 'always' } else { 'auto' }
    $extra = if ($UsbTest) { @('--', '-Dfw_bridge_USB_TEST=1') } else { @() }
    & west build -p $p -b nrf52840dk/nrf52840 -d $BuildDir . @extra
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
  } finally { Pop-Location }
}

function Invoke-Flash {
  & nrfjprog --snr $Snr --program "$BuildDir\merged.hex" --sectorerase --verify --reset
  if ($LASTEXITCODE -ne 0) { throw "Flash failed ($LASTEXITCODE)" }
}

function Invoke-Rtt([int]$Secs) {
  $logger = Join-Path $JLinkDir 'JLinkRTTLogger.exe'
  New-Item -ItemType Directory -Force (Split-Path $RttLog) | Out-Null
  if (Test-Path $RttLog) { Remove-Item $RttLog }
  $p = Start-Process -FilePath $logger -PassThru -WindowStyle Hidden -ArgumentList @(
    '-USB', $Snr, '-Device', 'NRF52840_XXAA', '-If', 'SWD', '-Speed', '4000', '-RTTChannel', '0', "`"$RttLog`"")
  Start-Sleep -Seconds $Secs
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
  Write-Host "===== bridge RTT log ($Secs s) -> $RttLog ====="
  if (Test-Path $RttLog) { Get-Content $RttLog } else { Write-Host '(no RTT data captured)' }
}

$SavedEnv = @{}
foreach ($name in 'PATH', 'PYTHONPATH', 'ZEPHYR_BASE', 'ZEPHYR_TOOLCHAIN_VARIANT', 'ZEPHYR_SDK_INSTALL_DIR', 'NRFUTIL_HOME') {
  $SavedEnv[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
  switch ($Command) {
    'build' { Invoke-Build }
    'flash' { Invoke-Build; Invoke-Flash }
    'rtt' { Invoke-Rtt $Seconds }
  }
} finally {
  foreach ($name in $SavedEnv.Keys) { [Environment]::SetEnvironmentVariable($name, $SavedEnv[$name], 'Process') }
}
