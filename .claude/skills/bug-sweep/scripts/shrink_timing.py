"""Shrink a failing fuzz_timing log to the fewest ops that still fail the same way (delta debugging), replaying subsets
through the app headless. Plain Python, outside the app:

    python shrink_timing.py <OpenAnimate.exe> <tfuzz_fail_N.json> [out.json] [--every K]

"The same way": the failure's kind (the text before its first colon). Replays run with OA_FUZZ_EVERY=K (default: the
log's length, so the periodic checks run once, after the last op).
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def kind(text):
    return text.split(":", 1)[0].strip()


def replay(exe, ops, work, every):
    path = os.path.join(work, "try.json")
    json.dump({"ops": ops}, open(path, "w", encoding="utf-8"))
    env = dict(os.environ, OA_FUZZ_REPLAY=path, OA_FUZZ_OUT=work, OA_FUZZ_EVERY=str(every or max(1, len(ops))),
               OA_FUZZ_HANG="60")
    try:
        r = subprocess.run([exe, "--background", "--python", os.path.join(HERE, "fuzz_timing.py")], env=env,
                           capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        return "crash-or-hang: timeout"
    fail = os.path.join(work, "tfuzz_replay_fail.json")
    if r.returncode == 0 or not os.path.exists(fail):
        return None
    got = json.load(open(fail, encoding="utf-8"))["failure"]
    os.remove(fail)
    return got


def main():
    args = [a for a in sys.argv[1:]]
    every = 10   # as the fuzzer runs (a reopen every 10 ops keeps the undo stack under its 64 steps)
    if "--every" in args:
        i = args.index("--every")
        every = int(args[i + 1])
        del args[i:i + 2]
    exe, log_path = args[0], args[1]
    out = args[2] if len(args) > 2 else log_path.replace(".json", "_min.json")
    log = json.load(open(log_path, encoding="utf-8"))
    want, ops = kind(log["failure"]), log["ops"]
    work = tempfile.mkdtemp(prefix="oa_tshrink_")
    first = replay(exe, ops, work, every)
    assert first and kind(first) == want, ("the log doesn't fail the same way on replay", first)
    n = 2
    while len(ops) >= 2:
        chunk = max(1, len(ops) // n)
        smaller = False
        for start in range(0, len(ops), chunk):
            rest = ops[:start] + ops[start + chunk:]
            got = replay(exe, rest, work, every)
            if got and kind(got) == want:
                ops, n, smaller = rest, max(n - 1, 2), True
                print(f"  {len(ops)} ops still fail: {got[:140]}", flush=True)
                break
        if not smaller:
            if chunk == 1:
                break
            n = min(len(ops), n * 2)
    final = replay(exe, ops, work, every)
    json.dump({"failure": final, "ops": ops}, open(out, "w", encoding="utf-8"), indent=1)
    print(f"shrunk to {len(ops)} ops -> {out}\n  {final}")


main()
