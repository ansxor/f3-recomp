import { existsSync, readFileSync } from "node:fs";
import { join, resolve } from "node:path";
import type { DifferentialAccounting, DifferentialOptions, DifferentialResult } from "./types.ts";
import { REPO_ROOT, capture } from "./util.ts";

/**
 * Run the instruction-level differential harness (`python -m tools.differential`, 68EC020 lowering vs Musashi)
 * and read its differential_accounting.json. Does not throw for a failing run; check `ok`.
 */
export async function runDifferential(opts: DifferentialOptions = {}): Promise<DifferentialResult> {
  const outputDir = resolve(REPO_ROOT, opts.output ?? join("build", "differential"));
  const [cmd, ...prefix] = opts.python ?? ["uv", "run", "python"];
  const args = [...prefix, "-m", "tools.differential", "--output", outputDir];
  if (opts.musashi) args.push("--musashi", resolve(REPO_ROOT, opts.musashi));
  if (opts.cases !== undefined) args.push("--cases", String(opts.cases));
  if (opts.seed !== undefined) args.push("--seed", String(opts.seed));
  if (opts.filter) args.push("--filter", opts.filter);
  if (opts.instructions) args.push("--instructions", resolve(REPO_ROOT, opts.instructions));
  if (opts.compileOnly) args.push("--compile-only");
  if (opts.verbose) args.push("-v");
  const done = await capture(cmd as string, args, { cwd: REPO_ROOT, timeoutMs: opts.timeoutMs });
  const report = join(outputDir, "differential_accounting.json");
  let accounting: DifferentialAccounting | null = null;
  if (existsSync(report)) {
    try {
      accounting = JSON.parse(readFileSync(report, "utf8")) as DifferentialAccounting;
    } catch {
      accounting = null;
    }
  }
  return {
    ok: done.code === 0 && !done.timedOut,
    exitCode: done.code,
    outputDir,
    output: done.stdout + done.stderr,
    accounting,
  };
}
