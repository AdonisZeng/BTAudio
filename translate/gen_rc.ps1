# Regenerates translate/generated/{zh_CN.ymo, zh_TW.ymo, translate.rc} from the
# .po sources. Windows equivalent of gen_rc.sh (which needs a POSIX shell and
# the translate-toolkit Python package) — this script only needs python3.
#
# Incremental: skips regeneration when the ymo files are newer than the .po
# sources, so it is safe to wire into the MSBuild PreBuildEvent.
#
# Run manually from anywhere:
#   powershell -ExecutionPolicy Bypass -File .\translate\gen_rc.ps1
$ErrorActionPreference = 'Stop'

$scriptDir = $PSScriptRoot
if (-not $scriptDir) { $scriptDir = (Get-Location).Path }

$cnPo = Join-Path $scriptDir 'source\zh_CN.po'
$cnYmo = Join-Path $scriptDir 'generated\zh_CN.ymo'
$twPo = Join-Path $scriptDir 'source\zh_TW.po'
$twYmo = Join-Path $scriptDir 'generated\zh_TW.ymo'
$rc = Join-Path $scriptDir 'generated\translate.rc'

function Test-Stale([string]$po, [string]$ymo) {
    if (-not (Test-Path $ymo)) { return $true }
    return (Get-Item $po).LastWriteTimeUtc -gt (Get-Item $ymo).LastWriteTimeUtc
}

$needRegen = (Test-Stale $cnPo $cnYmo) -or (Test-Stale $twPo $twYmo)
if (-not $needRegen) {
    Write-Host 'translate: ymo files are up to date.'
    exit 0
}

$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) {
    throw 'python3 is required to regenerate translation files (translate/po2ymo_stdlib.py)'
}

python (Join-Path $scriptDir 'po2ymo_stdlib.py') $cnPo $cnYmo
if ($LASTEXITCODE -ne 0) { throw 'po2ymo_stdlib.py failed for zh_CN.po' }
python (Join-Path $scriptDir 'po2ymo_stdlib.py') $twPo $twYmo
if ($LASTEXITCODE -ne 0) { throw 'po2ymo_stdlib.py failed for zh_TW.po' }

$rcLines = @(
    '#include "../../targetver.h"',
    '#include "windows.h"',
    'LANGUAGE LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED',
    '1 YMO "zh_CN.ymo"',
    'LANGUAGE LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL',
    '1 YMO "zh_TW.ymo"'
)
[System.IO.File]::WriteAllText(
    $rc,
    ($rcLines -join "`n") + "`n",
    [System.Text.UTF8Encoding]::new($false)
)
Write-Host 'translate: translate.rc and ymo files regenerated.'
