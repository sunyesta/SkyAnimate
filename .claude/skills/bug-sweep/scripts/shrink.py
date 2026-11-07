"""Shrink a failing fuzz log to the fewest ops that still fail the same way (delta debugging). Plain Python, outside
the app: it replays subsets through the app headless.

    python shrink.py <OpenAnimate.exe> <fuzz_fail_N.json> [out.json]

"The same way" is the failure's kind: the text before its first colon (exception, no-step change, undo-chain, order,
roundtrip, non-finite, cancel-changed, action-peek, follow, crash-or-hang). The result is written as a fuzz log; replay it with OA_FUZZ_REPLAY, or turn it into a test.
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def kind(text):
    return text.split(":", 1)[0].strip()


def replay(exe, sweep_file, ops, work):
    path = os.path.join(work, "try.json")
    json.dump({"ops": ops}, open(path, "w", encoding="utf-8"))
    env = dict(os.environ, OA_SWEEP_FILE=sweep_file, OA_FUZZ_REPLAY=path, OA_FUZZ_OUT=work, OA_SWEEP_TMP=work)
    r = subprocess.run([exe, "--background", "--python", os.path.join(HERE, "fuzz.py")], env=env,
                       capture_output=True, text=True, timeout=900)
    fail = os.path.join(work, "fuzz_replay_fail.json")
    if r.returncode == 0 or not os.path.exists(fail):
        return None
    got = json.load(open(fail, encoding="utf-8"))["failure"]
    os.remove(fail)
    return got


def main():
    exe, log_path = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else log_path.replace(".json", "_min.json")
    log = json.load(open(log_path, encoding="utf-8"))
    want, ops, sweep_file = kind(log["failure"]), log["ops"], log["file"]
    work = tempfile.mkdtemp(prefix="oa_shrink_")
    assert kind(replay(exe, sweep_file, ops, work) or "none") == want, "the log doesn't fail the same way on replay"
    n = 2
    while len(ops) >= 2:
        chunk = max(1, len(ops) // n)
        smaller = False
        for start in range(0, len(ops), chunk):
            rest = ops[:start] + ops[start + chunk:]
            got = replay(exe, sweep_file, rest, work)
            if got and kind(got) == want:
                ops, n, smaller = rest, max(n - 1, 2), True
                print(f"  {len(ops)} ops still fail: {got[:120]}", flush=True)
                break
        if not smaller:
            if chunk == 1:
                break
            n = min(len(ops), n * 2)
    final = replay(exe, sweep_file, ops, work)
    json.dump({"file": sweep_file, "failure": final, "ops": ops}, open(out, "w", encoding="utf-8"), indent=1)
    print(f"shrunk to {len(ops)} ops -> {out}\n  {final}")


main()
