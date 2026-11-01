param([string]$DevRoot = 'C:\S2DevRoot', [switch]$Force)
$exe = "$DevRoot\Stalker2\Binaries\Win64\Stalker2-Win64-Shipping.exe"
if (-not (Test-Path $exe)) { "no dev exe at $exe; run src\make_devroot.ps1 first"; exit 3 }
$run = Get-CimInstance Win32_Process -Filter "Name='Stalker2-Win64-Shipping.exe'" | Select-Object ProcessId, ExecutablePath
$dev = $run | Where-Object { $_.ExecutablePath -like "$DevRoot\*" }
if ($dev) { "dev game already running pid=$($dev.ProcessId)"; exit 1 }
if ($run -and -not $Force) { "other STALKER 2 games running: $($run | Out-String) use -Force to launch alongside"; exit 2 }
if ((Get-Item $exe).LinkType) { throw 'dev exe is a link' }
if (-not (Test-Path "$PSScriptRoot\bin\S2Dev.dll")) { "build first: src\build.bat"; exit 4 }
Copy-Item "$PSScriptRoot\bin\S2Dev.dll" "$DevRoot\Stalker2\Binaries\Win64\S2Dev.dll" -Force
$env:ANOMALY_HOME = $PSScriptRoot
$p = Start-Process -FilePath $exe -ArgumentList '-windowed','-ResX=1280','-ResY=720','-NoWarmupFailMessage','-saveddirsuffix=dev' -PassThru
"launched pid=$($p.Id)"
