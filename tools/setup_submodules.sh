#!/usr/bin/env bash
# Adds the two source dependencies as pinned git submodules (run once, from the
# repository root). Works in Git Bash on Windows too.
set -euo pipefail

RIVE_COMMIT=eacd96a338b5c9c8a32dc6c413e325d9c2208f0a   # rive-runtime main, 2026-10-31
THORVG_TAG=v1.1.3

if [ ! -e third_party/rive-runtime/.git ]; then
  git submodule add --depth 1 https://github.com/rive-app/rive-runtime.git third_party/rive-runtime
fi
git -C third_party/rive-runtime fetch --depth 1 origin "$RIVE_COMMIT"
git -C third_party/rive-runtime checkout --quiet "$RIVE_COMMIT"

if [ ! -e third_party/thorvg/.git ]; then
  git submodule add --depth 1 https://github.com/thorvg/thorvg.git third_party/thorvg
fi
git -C third_party/thorvg fetch --depth 1 origin tag "$THORVG_TAG"
git -C third_party/thorvg checkout --quiet "$THORVG_TAG"

git add .gitmodules third_party/rive-runtime third_party/thorvg
echo "Submodules pinned: rive-runtime @ ${RIVE_COMMIT:0:9}, thorvg @ ${THORVG_TAG}"
