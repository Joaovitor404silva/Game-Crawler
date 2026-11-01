param(
  [Parameter(Mandatory = $true)][string]$Exe,
  [string]$DevRoot
)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
if ($Exe -notlike '*.exe') { throw "not an exe: $Exe" }
$win64 = Split-Path $Exe
$root = $win64
while ($root -and -not (Test-Path (Join-Path $root 'Engine'))) { $root = Split-Path $root }
if (-not $root) { throw "no Engine folder above $Exe; is this an Unreal Engine game?" }
$name = Split-Path $root -Leaf
if (-not $DevRoot) { $DevRoot = Join-Path 'C:\GameCrawlerRoot' ($name -replace '[^A-Za-z0-9._-]', '_') }
$r = $DevRoot.TrimEnd('\')
if ($r -like "$root*") { throw 'DevRoot must be outside the game folder' }
$skipFiles = 'dwmapi.dll', 'version.dll', 'dxgi.dll', 'winmm.dll', 'xinput1_3.dll', 'dinput8.dll', 'd3d11.dll', 'UE4SS.dll', 'UE4SS-settings.ini'
$skipDirs = 'ue4ss', 'Mods', '~mods', 'LogicMods', 'Saved'
function MkJ($p, $t) { if (-not (Test-Path $p)) { New-Item -ItemType Junction -Path $p -Target $t | Out-Null } }
function MkH($p, $t) { if (-not (Test-Path $p)) { try { New-Item -ItemType HardLink -Path $p -Target $t -ErrorAction Stop | Out-Null } catch { Copy-Item $t $p } } }
$descend = @($win64)
Get-ChildItem $root -Directory | ForEach-Object { $pk = Join-Path $_.FullName 'Content\Paks'; if (Test-Path $pk) { $descend += $pk } }
function Mirror($src, $dst) {
  New-Item -ItemType Directory -Force $dst | Out-Null
  foreach ($c in Get-ChildItem $src -Force) {
    $t = Join-Path $dst $c.Name
    if ($c.PSIsContainer) {
      if ($skipDirs -contains $c.Name) { continue }
      $onPath = $descend | Where-Object { $_ -eq $c.FullName -or $_ -like "$($c.FullName)\*" }
      if ($onPath) { Mirror $c.FullName $t } else { MkJ $t $c.FullName }
    } else {
      if ($src -eq $win64 -and $skipFiles -contains $c.Name) { continue }
      MkH $t $c.FullName
    }
  }
}
Mirror $root $r
$devWin64 = $win64.Replace($root, $r)
Copy-Item "$PSScriptRoot\..\bin\proxy\version.dll" (Join-Path $devWin64 'version.dll') -Force
Set-Content -Encoding ascii (Join-Path $devWin64 'crawler_home.txt') (Resolve-Path "$PSScriptRoot\..").Path
$common = Split-Path $root
if ((Split-Path $common -Leaf) -eq 'common' -and -not (Test-Path (Join-Path $win64 'steam_appid.txt'))) {
  $inst = Split-Path $root -Leaf
  foreach ($m in Get-ChildItem (Split-Path $common) -Filter 'appmanifest_*.acf') {
    $t = Get-Content $m.FullName -Raw
    if ($t -match '"installdir"\s+"([^"]+)"' -and $Matches[1] -eq $inst -and $t -match '"appid"\s+"(\d+)"') { Set-Content -Encoding ascii (Join-Path $devWin64 'steam_appid.txt') $Matches[1]; "steam appid: $($Matches[1])"; break }
  }
}
Set-Content -Encoding ascii (Join-Path $r 'crawler_exe.txt') $Exe.Replace($root, $r)
"devroot ok: $r"
"exe: $($Exe.Replace($root, $r))"
