# Adds the source dependencies as pinned git submodules (run once, from the
# repository root, in PowerShell). Safe to re-run: existing submodules are
# just re-pinned.
#
# HarfBuzz, SheenBidi and Yoga (text and layouts) are the versions
# rive-runtime pins in its dependencies/premake5_*_v2.lua (HarfBuzz's is a
# branch there, rive_13.1.1, so it is pinned here to that branch's commit).
$ErrorActionPreference = "Stop"

# path, url, ref (commit or tag)
$Deps = @(
    @("third_party/rive-runtime", "https://github.com/rive-app/rive-runtime.git", "eacd96a338b5c9c8a32dc6c413e325d9c2208f0a"),
    @("third_party/thorvg",       "https://github.com/thorvg/thorvg.git",        "v1.1.3"),
    @("third_party/harfbuzz",     "https://github.com/rive-app/harfbuzz.git",    "08d34675f64b1ac4880f4f10c9fd474a56dd4399"),  # branch rive_13.1.1
    @("third_party/sheenbidi",    "https://github.com/Tehreer/SheenBidi.git",    "v2.6"),
    @("third_party/yoga",         "https://github.com/rive-app/yoga.git",        "rive_yoga_dee88d2dfcad")
)

function Run($exe, [string[]]$argv) {
    & $exe @argv
    if ($LASTEXITCODE -ne 0) { throw "$exe $argv failed ($LASTEXITCODE)" }
}

$paths = @()
foreach ($dep in $Deps) {
    $path, $url, $ref = $dep
    if (-not (Test-Path "$path/.git")) {
        Run git @("submodule", "add", "--depth", "1", $url, $path)
    }
    if ($ref -match '^[0-9a-f]{40}$') {
        Run git @("-C", $path, "fetch", "--depth", "1", "origin", $ref)
    } else {
        Run git @("-C", $path, "fetch", "--depth", "1", "origin", "tag", $ref)
    }
    Run git @("-C", $path, "checkout", "--quiet", $ref)
    $paths += $path
    Write-Host "pinned $path @ $ref"
}

Run git (@("add", ".gitmodules") + $paths)
