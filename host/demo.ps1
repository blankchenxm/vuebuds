# Run from any directory. Reuse this worktree's venv or another worktree's host venv.
param(
    [string]$Captures = '',
    [string]$EnvFile = '',
    [string]$BridgePort = '',
    [int]$Port = 7860,
    [switch]$NoBrowser
)
$ErrorActionPreference = 'Stop'
$roots = @($PSScriptRoot)
$trees = & git -C $PSScriptRoot worktree list --porcelain
foreach ($line in $trees) {
    if ($line.StartsWith('worktree ')) { $roots += Join-Path $line.Substring(9) 'host' }
}
$python = $null
foreach ($root in $roots) {
    $candidate = Join-Path $root '.venv\Scripts\python.exe'
    if (Test-Path -LiteralPath $candidate) { $python = $candidate; break }
}
if (-not $python) { throw 'Create host/.venv and install host/requirements.txt first.' }
$demoArgs = @((Join-Path $PSScriptRoot 'demo.py'), '--port', $Port)
if ($Captures) { $demoArgs += @('--captures', $Captures) }
if ($EnvFile) { $demoArgs += @('--env-file', $EnvFile) }
if ($BridgePort) { $demoArgs += @('--bridge-port', $BridgePort) }
if ($NoBrowser) { $demoArgs += '--no-browser' }
$env:PYTHONIOENCODING = 'utf-8'
& $python @demoArgs
exit $LASTEXITCODE
