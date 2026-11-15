# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Run the input capture unit's own template against a simulated timer.

    python input_capture_model.py [--out REPORT.json] [--cases N]

The unit states one property: a measurement returns the true period and high time with
all three edges before the time limit, or a time-out that is true, or an error, however
long and wherever the caller is delayed. A board cannot place a delay between two chosen
statements, and reading the code missed such a path twice. This test can: it renders the
pinned template with test bindings, compiles it with a host C compiler next to
input_capture_model.c, and lets that model delay the caller at every access to the timer.

What is run:
  replay_codex, replay_claude   the two traces of the follow-up reviews, each
                                with its delay tried at every boundary around its moment;
  sweep                         one delay of 14 lengths at every boundary around the
                                first four marks of two signals;
  any                           seeded cases without restraint on edges and delays;
  unmaskable                    the same with delays that ignore the interrupt mask;
  strict                        seeded cases inside the unit's stated operating
                                condition, where an error is a violation too.
Version 3 of the unit must show no violation in any of them, on either capture channel.
Version 2, the unit the reviews rejected, is run through the same model as a positive
control and must fail on the traces, with the reviewers' exact wrong values, and on the
sweep. Its seeded run is reported; it meets the fault only by chance.

The model is not the hardware. It is the reference manual's description of the counter,
the two flags, capture and overcapture as input_capture_model.c states them.
Needs a host C compiler: the one CC names, or gcc. Outputs are temporary and removed.
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HERE = Path(__file__).resolve().parent
SEED = 20261114
BINDINGS = dict(unit="unit", pad_name="PAD", pull="none", peripheral_symbol="model_timer()", timer_clock="model_clock",
                timer_reset="model_reset", timer_instance="0", input_source="0", channel="0", capture_unit="0",
                pad_clock="model_clock", pull_symbol="0", pad_alternate="0", pad_port="0", pad_pin="0")


def template(version):
    """The pinned template: the package's copy, or the source root the generator uses."""
    relative = f"architecture/sources/bkpt-hal2-input-capture-v{version}/input-capture.c.in"
    for base in (ROOT / "sources", Path(os.environ.get("BKPT_KNOWLEDGE_SOURCE_ROOT", ROOT / "sources"))):
        if (base / relative).exists():
            return (base / relative).read_text(encoding="utf-8")
    raise SystemExit(f"template not found: {relative}")


def compiler():
    """The host C compiler and an environment in which it finds its own libraries first."""
    named = os.environ.get("CC", "")
    found = shutil.which(named) if named else None
    found = found or shutil.which("gcc") or shutil.which("cc")
    if not found:
        raise SystemExit("input capture host model: no host C compiler (set CC, or put gcc on the path)")
    return found, dict(os.environ, PATH=str(Path(found).parent) + os.pathsep + os.environ.get("PATH", ""))


def build(folder, version, channel):
    work = folder / f"v{version}-channel{channel}"
    work.mkdir()
    values = dict(BINDINGS, channel_number=str(channel))
    (work / "unit.c").write_text(re.sub(r"\{\{(\w+)\}\}", lambda match: values[match[1]], template(version)), encoding="utf-8")
    (work / "unit.h").write_text("#include \"stm32_hal.h\"\nhal_status_t unit_init(void);\n"
                                 "hal_status_t unit_measure(uint32_t timeout_ms, uint32_t *period_us, uint32_t *high_us);\n",
                                 encoding="utf-8")
    (work / "stm32_hal.h").write_text((HERE / "input_capture_model.h").read_text(encoding="utf-8"), encoding="utf-8")
    executable = work / "model.exe"
    program, environment = compiler()
    done = subprocess.run([program, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                           f"-DMODEL_CAPTURE_CHANNEL={channel}", f"-I{work}", str(work / "unit.c"),
                           str(HERE / "input_capture_model.c"), "-o", str(executable)],
                          capture_output=True, text=True, env=environment)
    if done.returncode != 0:
        raise SystemExit(f"input capture host model: version {version} did not compile (status {done.returncode})\n"
                         f"{done.stdout[-2000:]}{done.stderr[-4000:]}")
    return executable


def run(executable, *arguments):
    done = subprocess.run([str(executable), *[str(argument) for argument in arguments]], check=True, capture_output=True, text=True)
    return json.loads(done.stdout)


def main():
    arguments = sys.argv[1:]
    cases = int(arguments[arguments.index("--cases") + 1]) if "--cases" in arguments else 12000
    out = Path(arguments[arguments.index("--out") + 1]) if "--out" in arguments else None
    report = {"seed": SEED, "version_3": [], "version_2_positive_control": []}
    failures = []
    with tempfile.TemporaryDirectory(prefix="bkpt-capture-model-") as temporary:
        folder = Path(temporary)
        current = build(folder, 3, 1)
        other_channel = build(folder, 3, 2)
        rejected = build(folder, 2, 1)
        runs = [(current, ("replay_codex",)), (current, ("replay_claude",)), (current, ("sweep",)),
                (current, ("any", cases, SEED)), (current, ("unmaskable", cases // 2, SEED + 1)),
                (current, ("strict", cases, SEED + 2)),
                (other_channel, ("replay_codex",)), (other_channel, ("replay_claude",)),
                (other_channel, ("any", cases // 4, SEED + 3)), (other_channel, ("strict", cases // 4, SEED + 4))]
        control = [("replay_codex",), ("replay_claude",), ("sweep",), ("any", cases, SEED)]
        with ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
            results = list(pool.map(lambda entry: run(entry[0], *entry[1]), runs + [(rejected, command) for command in control]))
        for result in results[:len(runs)]:
            report["version_3"].append(result)
            if result["violations"] != 0:
                failures.append(f"version 3, channel {result['channel']}, {result['mode']}: {result['violations']} violations, "
                                f"first {json.dumps(result['first'][:1])}")
        # The model must exercise every outcome, or passing would mean nothing.
        free, strict = report["version_3"][3], report["version_3"][5]
        for name in ("exact", "timeout_true", "error"):
            if free[name] == 0:
                failures.append(f"the unrestrained cases never ended in {name}")
        if strict["exact"] == 0 or strict["timeout_true"] == 0 or strict["error"] != 0:
            failures.append("the strict cases must end exact or in a true time-out, and both must occur")
        # Positive control: the rejected version has to fail in this model, with the reviewers' values.
        expected = {"replay_codex": (113464, 83464, 179000, 149000), "replay_claude": (83464, 73464, 149000, 139000)}
        for result in results[len(runs):]:
            report["version_2_positive_control"].append(result)
            # A seeded run meets the fault only by chance, so it is reported and not required.
            if result["violations"] == 0 and result["mode"] != "any":
                failures.append(f"positive control: version 2 shows no violation in {result['mode']}")
            wanted = expected.get(result["mode"])
            seen = {(entry["period"], entry["high"], entry["true_period"], entry["true_high"]) for entry in result["wrong_results"]}
            if wanted and wanted not in seen:
                failures.append(f"positive control: version 2 did not return {wanted[:2]} for {wanted[2:]} in {result['mode']}")
    report["passed"] = not failures
    report["failures"] = failures
    if out:
        out.write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8", newline="\n")
    for name in ("version_3", "version_2_positive_control"):
        for result in report[name]:
            print(f"{name:27s} channel {result['channel']} {result['mode']:13s} cases {result['cases']:6d} "
                  f"exact {result['exact']:6d} timeout {result['timeout_true']:6d} error {result['error']:6d} "
                  f"violations {result['violations']:5d} "
                  f"(wrong value {result['wrong_value']}, late success {result['late_success']}, false time-out "
                  f"{result['false_timeout']}, no return {result['no_return']}, strict error {result['strict_error']})")
    for failure in failures:
        print("FAILED:", failure)
    if failures:
        raise SystemExit(1)
    total = sum(result["cases"] for result in report["version_3"])
    print(f"input capture host model: version 3 held its property in {total} cases; version 2 fails as the reviews found")


if __name__ == "__main__":
    main()
