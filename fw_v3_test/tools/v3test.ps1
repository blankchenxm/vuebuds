# balsamic3 hardware test: build, flash (application core only) and read the RTT log.
# Same NCS setup as fw_nrf5340/tools/dk5340.ps1 (NCS v3.2.1 in D:\NRFSDK, repo path mapped
# to V: because the devicetree preprocessor fails on paths with spaces).
#   .\tools\v3test.ps1 build | flash | rtt | run   [-Seconds 70] [-Snr 53001982] [-Pristine]
# The PSRAM pass takes ~20 s; with shorter captures the log was cut there (the logger is
# killed before its last data reaches the file), so keep -Seconds at 70 or more.
param(
  [Parameter(Position = 0)][ValidateSet('build', 'flash', 'rtt', 'run')]
  [string]$Command = 'run',
  [int]$Seconds = 70,
  [string]$Snr = '53001982',   # J-Link EDU Mini used with the debug FPC
  [switch]$Pristine
)
$ErrorActionPreference = 'Continue'
$Repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Drive = if ($Repo -match ' ') { 'V:' } else { $Repo.TrimEnd('\') }
$Ncs = if ($env:NCS_DIR) { $env:NCS_DIR } else { 'D:\NRFSDK' }
$Tc = Join-Path $Ncs 'toolchains\66cdf9b75e'
$JLinkDir = if ($env:JLINK_DIR) { $env:JLINK_DIR } else { 'C:\Program Files\SEGGER\JLink' }
$BuildDir = "$Drive\fw_v3_test\build"
$RttLog = Join-Path $Repo 'fw_v3_test\build_logs\rtt.log'

function Use-Drive {
  if ($Drive -eq 'V:' -and -not (Test-Path "$Drive\fw_v3_test")) { cmd /c "subst $Drive `"$Repo`"" | Out-Null }
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
  Push-Location "$Drive\fw_v3_test"
  try {
    $p = if ($Pristine) { 'always' } else { 'auto' }
    & west build -p $p -b nrf5340dk/nrf5340/cpuapp -d $BuildDir .
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
  $logger = Join-Path $JLinkDir 'JLinkRTTLogger.exe'
  New-Item -ItemType Directory -Force (Split-Path $RttLog) | Out-Null
  if (Test-Path $RttLog) { Remove-Item $RttLog }
  $p = Start-Process -FilePath $logger -PassThru -WindowStyle Hidden -ArgumentList @(
    '-USB', $Snr, '-Device', 'nRF5340_xxAA_APP', '-If', 'SWD', '-Speed', '1000', '-RTTChannel', '0', "`"$RttLog`"")
  Start-Sleep -Seconds 2
  & nrfjprog --snr $Snr --reset | Out-Null
  Start-Sleep -Seconds $Secs
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
  Write-Host "===== RTT log ($Secs s) -> $RttLog ====="
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
    'run' { Invoke-Build; Invoke-Flash; Invoke-Rtt $Seconds }
  }
} finally {
  foreach ($name in $SavedEnv.Keys) { [Environment]::SetEnvironmentVariable($name, $SavedEnv[$name], 'Process') }
}
