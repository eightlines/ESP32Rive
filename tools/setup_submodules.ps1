# Adds the two source dependencies as pinned git submodules (run once, from the
# repository root, in PowerShell).
$ErrorActionPreference = "Stop"

$RiveCommit = "eacd96a338b5c9c8a32dc6c413e325d9c2208f0a"  # rive-runtime main, 2026-10-31
$ThorvgTag  = "v1.1.3"

function Run($exe, [string[]]$argv) {
    & $exe @argv
    if ($LASTEXITCODE -ne 0) { throw "$exe $argv failed ($LASTEXITCODE)" }
}

if (-not (Test-Path "third_party/rive-runtime/.git")) {
    Run git @("submodule", "add", "--depth", "1", "https://github.com/rive-app/rive-runtime.git", "third_party/rive-runtime")
}
Run git @("-C", "third_party/rive-runtime", "fetch", "--depth", "1", "origin", $RiveCommit)
Run git @("-C", "third_party/rive-runtime", "checkout", "--quiet", $RiveCommit)

if (-not (Test-Path "third_party/thorvg/.git")) {
    Run git @("submodule", "add", "--depth", "1", "https://github.com/thorvg/thorvg.git", "third_party/thorvg")
}
Run git @("-C", "third_party/thorvg", "fetch", "--depth", "1", "origin", "tag", $ThorvgTag)
Run git @("-C", "third_party/thorvg", "checkout", "--quiet", $ThorvgTag)

Run git @("add", ".gitmodules", "third_party/rive-runtime", "third_party/thorvg")
Write-Host "Submodules pinned: rive-runtime @ $($RiveCommit.Substring(0,9)), thorvg @ $ThorvgTag"
