#!/usr/bin/env bash
# Adds the source dependencies as pinned git submodules (run once, from the
# repository root). Safe to re-run: existing submodules are just re-pinned.
# Works in Git Bash on Windows too.
#
# HarfBuzz, SheenBidi, Yoga (text and layouts) and miniaudio (audio) are the
# versions rive-runtime pins in its dependencies/premake5_*_v2.lua
# (HarfBuzz's and miniaudio's are branches there, rive_13.1.1 and
# rive_changes_11, so they are pinned here to those branches' commits).
set -euo pipefail

# path | url | ref (commit or tag)
DEPS=(
  "third_party/rive-runtime|https://github.com/rive-app/rive-runtime.git|eacd96a338b5c9c8a32dc6c413e325d9c2208f0a"
  "third_party/thorvg|https://github.com/thorvg/thorvg.git|v1.1.3"
  "third_party/harfbuzz|https://github.com/rive-app/harfbuzz.git|08d34675f64b1ac4880f4f10c9fd474a56dd4399"   # branch rive_13.1.1
  "third_party/sheenbidi|https://github.com/Tehreer/SheenBidi.git|v2.6"
  "third_party/yoga|https://github.com/rive-app/yoga.git|rive_yoga_dee88d2dfcad"
  "third_party/miniaudio|https://github.com/rive-app/miniaudio.git|c0f61f670d986c367fa861aeadf953a933ed26ac"   # branch rive_changes_11
)

paths=()
for dep in "${DEPS[@]}"; do
  IFS='|' read -r path url ref <<< "$dep"
  if [ ! -e "$path/.git" ]; then
    git submodule add --depth 1 "$url" "$path"
  fi
  if [[ "$ref" =~ ^[0-9a-f]{40}$ ]]; then
    git -C "$path" fetch --depth 1 origin "$ref"
  else
    git -C "$path" fetch --depth 1 origin tag "$ref"
  fi
  git -C "$path" checkout --quiet "$ref"
  paths+=("$path")
  echo "pinned $path @ $ref"
done

git add .gitmodules "${paths[@]}"
