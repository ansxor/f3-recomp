#!/usr/bin/env bash
# Run every model in this directory and print one line per check.
# Needs alloy6, tlc (tla2tools) and java on PATH, e.g.:
#   nix shell nixpkgs#alloy6 nixpkgs#tlaplus -c ./run-all.sh
set -uo pipefail
cd "$(dirname "$0")"
work=${MODEL_WORK:-$(mktemp -d)}
echo "# Alloy (minisat)"
for model in liveness.als coverage.als pipeline.als; do
  echo "## $model"
  ALLOY_OUT="$work/${model%.als}" ./run-alloy.sh "$model" | grep -v '^solutions in'
done
echo "# TLA+ (TLC)"
for cfg in LazyFlags_*.cfg Dispatch_*.cfg; do
  spec=${cfg%%_*}.tla
  [ "$cfg" = Dispatch_witnesses.cfg ] && continue
  expected=$(head -1 "$cfg" | sed 's/^\\\* Expected: //')
  log="$work/${cfg%.cfg}.log"
  tlc -workers auto -deadlock -metadir "$work/${cfg%.cfg}" -config "$cfg" "$spec" >"$log" 2>&1
  result=$(grep -oE 'Invariant [A-Za-z]+ is violated|Temporal properties were violated|No error has been found' "$log" | head -1)
  states=$(grep -oE '[0-9]+ distinct states found' "$log" | tail -1)
  printf '%-44s %-44s %s\n  expected: %s\n' "$cfg" "${result:-ERROR (see $log)}" "$states" "$expected"
done
for inv in IdleNeverYields RamNeverRunsNatively HandlerNeverRuns; do
  sed "s/^INVARIANT .*/INVARIANT $inv/" Dispatch_witnesses.cfg >"$work/witness.cfg"
  result=$(tlc -deadlock -metadir "$work/w-$inv" -config "$work/witness.cfg" Dispatch.tla 2>&1 |
           grep -oE 'is violated|No error has been found' | head -1)
  printf '%-44s %s (expected: is violated = path reachable)\n' "witness $inv" "$result"
done
echo "logs and solutions in $work"
