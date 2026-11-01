param(
  [string]$Game = 'C:\Program Files (x86)\Steam\steamapps\common\S.T.A.L.K.E.R. 2 Heart of Chornobyl',
  [string]$DevRoot = 'C:\S2DevRoot'
)
$ErrorActionPreference = 'Stop'
$g = $Game; $r = $DevRoot
if (-not (Test-Path "$g\Stalker2\Binaries\Win64\Stalker2-Win64-Shipping.exe")) { throw "game not found at $g (use -Game)" }
if ($r -like "$g*") { throw 'DevRoot must be outside the game folder' }
function MkJ($p, $t) { if (-not (Test-Path $p)) { New-Item -ItemType Junction -Path $p -Target $t | Out-Null } }
function MkH($p, $t) { if (-not (Test-Path $p)) { New-Item -ItemType HardLink -Path $p -Target $t | Out-Null } }
New-Item -ItemType Directory -Force "$r\Stalker2\Binaries\Win64", "$r\Stalker2\Content\Paks" | Out-Null
MkJ "$r\Engine" "$g\Engine"
if (Test-Path "$g\DLC1") { MkJ "$r\DLC1" "$g\DLC1" }
foreach ($f in 'DLC_CostOfHope', 'InstallScript.vdf', 'Stalker2.exe') { if (Test-Path "$g\$f" -PathType Leaf) { MkH "$r\$f" "$g\$f" } elseif (Test-Path "$g\$f") { MkJ "$r\$f" "$g\$f" } }
MkJ "$r\Stalker2\Plugins" "$g\Stalker2\Plugins"
foreach ($d in 'Movies', 'Razer') { if (Test-Path "$g\Stalker2\Content\$d") { MkJ "$r\Stalker2\Content\$d" "$g\Stalker2\Content\$d" } }
Get-ChildItem "$g\Stalker2\Content\Paks" -File | ForEach-Object { MkH "$r\Stalker2\Content\Paks\$($_.Name)" $_.FullName }
$w = "$g\Stalker2\Binaries\Win64"
foreach ($d in 'D3D12', 'DML') { if (Test-Path "$w\$d") { MkJ "$r\Stalker2\Binaries\Win64\$d" "$w\$d" } }
Get-ChildItem $w -File | Where-Object { $_.Name -notin 'dwmapi.dll', 'Stalker2-Win64-Shipping.exe' } | ForEach-Object { MkH "$r\Stalker2\Binaries\Win64\$($_.Name)" $_.FullName }
$exe = "$r\Stalker2\Binaries\Win64\Stalker2-Win64-Shipping.exe"
if (Test-Path $exe) { if ((Get-Item $exe).LinkType) { throw 'dev exe is a link, refusing' } } else {
  node "$PSScriptRoot\patch_exe.js" "$w\Stalker2-Win64-Shipping.exe" $exe; if ($LASTEXITCODE) { throw 'patch failed' } }
"devroot ok: $r"
