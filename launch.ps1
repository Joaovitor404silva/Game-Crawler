param(
  [Parameter(Mandatory = $true)][string]$DevRoot,
  [string[]]$GameArgs = @('-windowed', '-ResX=1280', '-ResY=720', '-saveddirsuffix=dev'),
  [switch]$Force
)
$cfg = Join-Path $DevRoot 'uescan_exe.txt'
if (-not (Test-Path $cfg)) { "no dev root at $DevRoot; run src\make_devroot.ps1 -Exe <game exe> first"; exit 3 }
$exe = (Get-Content $cfg -TotalCount 1).Trim()
if (-not (Test-Path $exe)) { "dev exe missing: $exe"; exit 3 }
if (-not (Test-Path "$PSScriptRoot\bin\UEScanner.dll")) { "build first: src\build.bat"; exit 4 }
$exeName = Split-Path $exe -Leaf
$run = Get-CimInstance Win32_Process -Filter "Name='$exeName'" | Select-Object ProcessId, ExecutablePath
$dev = $run | Where-Object { $_.ExecutablePath -like "$DevRoot\*" }
if ($dev) { "dev game already running pid=$($dev.ProcessId)"; exit 1 }
if ($run -and -not $Force) { "$exeName already running outside the dev root; use -Force to launch alongside"; exit 2 }
Copy-Item "$PSScriptRoot\bin\proxy\version.dll" (Join-Path (Split-Path $exe) 'version.dll') -Force
$env:UESCAN_HOME = $PSScriptRoot
$p = Start-Process -FilePath $exe -ArgumentList $GameArgs -PassThru
"launched pid=$($p.Id)"
