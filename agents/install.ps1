# Installs the Game Crawler subagents (agents/*.md) into a project's .claude/agents/.
# {{CRAWLER}} in the files becomes this Game Crawler folder (notes, logs, marks live in <crawler>/agent/).
#   .\agents\install.ps1 -Project C:\Games\MyGame
param([Parameter(Mandatory = $true)][string]$Project)
$ErrorActionPreference = 'Stop'
$crawler = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path -replace '\\', '/'
$dst = Join-Path $Project '.claude\agents'
New-Item -ItemType Directory -Force $dst | Out-Null
New-Item -ItemType Directory -Force (Join-Path $PSScriptRoot '..\agent\notes') | Out-Null
foreach ($f in Get-ChildItem (Join-Path $PSScriptRoot '*.md')) {
    $text = [IO.File]::ReadAllText($f.FullName).Replace('{{CRAWLER}}', $crawler)
    [IO.File]::WriteAllText((Join-Path $dst $f.Name), $text, (New-Object Text.UTF8Encoding $false))
    Write-Output "installed $($f.Name) -> $dst"
}
