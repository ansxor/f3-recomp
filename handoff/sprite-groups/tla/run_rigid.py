"""Full TLC matrix for RigidGroups.tla: grouping x unresolved x shadow x invariant.

Sequential runs, one -metadir per run. The module is copied to /tmp/sgmodel so
TLC's trace files stay out of the repository. Rows are appended to
rigid_results.txt in this directory. Raw TLC output: /tmp/sgmodel/<tag>.out.
"""
import os, re, shutil, subprocess, time

HERE = os.path.dirname(os.path.abspath(__file__))
D = "/tmp/sgmodel"
JAVA = "/opt/homebrew/opt/openjdk@21/bin/java"
JAR = os.path.expanduser("~/Downloads/tla2tools.jar")
INVS = ["RigidKey", "GroupSound", "GroupComplete", "Interpolates", "PairSound", "ShadowSmooth"]
GROUPINGS = ["class", "object"]
UNRESOLVED = ["pair", "snap"]
SHADOWS = ["forced", "flicker", "fill"]

CFG = """CONSTANTS
  Addr = {{1, 2}}
  Levels = {{0, 1}}
  Pos = {{0, 1}}
  Shadowed = {{1}}
  MaxUid = 9
  Grouping = "{g}"
  ShadowMode = "{sm}"
  Unresolved = "{un}"
SPECIFICATION Spec
CONSTRAINT Bound
INVARIANT {inv}
"""

os.makedirs(D, exist_ok=True)
shutil.copy(os.path.join(HERE, "RigidGroups.tla"), D)
version = None

with open(os.path.join(HERE, "rigid_results.txt"), "a") as log:
    log.write(f"# Matrix run {time.strftime('%Y-%m-%d %H:%M')} (module copy: {D}/RigidGroups.tla)\n")
    log.write("# ShadowSmooth: once a shadow is drawn for an incarnation, every later frame pairs it (drawnBefore history; frames before the first draw are exempt).\n")
    log.write("grouping\tunresolved\tshadow\tinvariant\tstatus\tstates\ttime\texit\n")
    for g in GROUPINGS:
        for un in UNRESOLVED:
            for sm in SHADOWS:
                for inv in INVS:
                    tag = f"{g}_{un}_{sm}_{inv}"
                    cfg = f"{D}/r_{tag}.cfg"
                    meta = f"{D}/meta/{tag}"
                    shutil.rmtree(meta, ignore_errors=True)
                    with open(cfg, "w") as f:
                        f.write(CFG.format(g=g, un=un, sm=sm, inv=inv))
                    t0 = time.time()
                    p = subprocess.run(
                        [JAVA, "-XX:+UseParallelGC", "-cp", JAR, "tlc2.TLC", "-workers", "auto",
                         "-deadlock", "-metadir", meta, "-config", cfg, "RigidGroups.tla"],
                        capture_output=True, text=True, cwd=D)
                    out = p.stdout + p.stderr
                    with open(f"{D}/{tag}.out", "w") as f:
                        f.write(out)
                    if version is None:
                        v = re.search(r"TLC2 Version ([^\n]*)", out)
                        version = v.group(1).strip() if v else "unknown"
                        log.write(f"# {version}\n")
                    status = ("PASS" if "No error has been found" in out
                              else "FAIL" if "is violated" in out else "ERR")
                    # Final summary line only: Progress(n) lines also contain "distinct states found".
                    st = re.findall(r"^([\d,]+) states generated, ([\d,]+) distinct states found, ", out, re.M)
                    states = f"{int(st[-1][1].replace(',', '')):,}" if st else "-"
                    log.write(f"{g}\t{un}\t{sm}\t{inv}\t{status}\t{states}\t"
                              f"{time.time() - t0:.0f}s\t{p.returncode}\n")
                    log.flush()
                    print(tag, status, states, p.returncode, flush=True)
