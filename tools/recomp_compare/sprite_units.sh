#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."
generator=${1:-recomp/_build/default/bin/main.exe}
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

configs=(games/*/config.toml)
for config in "${configs[@]}"; do
    game=$(basename "$(dirname "$config")")
    uv run python recomp/compile/sprite_units.py --config "$config" --output-dir "$scratch/$game/python"
    "$generator" sprite-units --config "$config" --output-dir "$scratch/$game/ocaml"
    for header in sprite_units.h sprite_units.hpp; do
        if ! cmp "$scratch/$game/python/$header" "$scratch/$game/ocaml/$header"; then
            echo "$config: $header differs" >&2
            exit 1
        fi
    done
done
echo "Both headers match for all ${#configs[@]} configs."
