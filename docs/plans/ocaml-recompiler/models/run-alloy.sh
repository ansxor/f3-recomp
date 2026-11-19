#!/usr/bin/env bash
# Run every command of an Alloy model with a native SAT solver and print one
# line per command: name, outcome (SAT = instance/counterexample found), time.
# Usage: ./run-alloy.sh model.als [command-glob]
set -euo pipefail
model=$1
glob=${2:-*}
jar=${ALLOY_JAR:-$(dirname "$(readlink -f "$(command -v alloy6)")")/../share/alloy/alloy6.jar}
out=${ALLOY_OUT:-$(mktemp -d)}
java -jar "$jar" exec -q -f -s minisat -t text -o "$out" -c "$glob" "$model" >/dev/null 2>&1 || true
python3 - "$out/receipt.json" <<'PY'
import json, sys
receipt = json.load(open(sys.argv[1]))
for name, command in receipt["commands"].items():
    solutions = command.get("solution", [])
    found = any(s.get("instances") for s in solutions)
    kind = "check" if "check" in str(command.get("type", "")).lower() else "run"
    duration = sum(s.get("duration", 0) for s in solutions)
    verdict = ("COUNTEREXAMPLE" if found else "no counterexample") if kind == "check" else ("SAT" if found else "UNSAT")
    print(f"{name:40} {kind:5} {verdict:18} {duration} ms  {command.get('scopes')}")
PY
echo "solutions in $out"
