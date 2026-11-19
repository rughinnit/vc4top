#!/usr/bin/env python3
"""Field-parity checker for vc4top releases.

Compares a baseline binary with a candidate binary and checks that the
headless interface stays byte-comparable in name/order:

  * every baseline `--once` keyed field is still present, spelled identically,
    and in the same relative order;
  * the only fields allowed to be added are the documented memory fields
    (memory is a new capability, so extra fields are expected and reported);
  * all 30 `--all-counters` rows keep their id, name and order;
  * all 78 `--regs` rows keep their name and order.

Volatile values (time, temperature, clocks, CMA free, live counters, ...) are
not compared; the contract is name/order/row-count.

Usage:
    sudo tools/field_parity.py --baseline /tmp/vc4top_101 --new build/vc4top

The V3D block is serialized with an flock on /tmp/vc4-gpu.lock so two runs can
never touch the GPU at the same time.
"""

import argparse
import fcntl
import os
import re
import subprocess
import sys

# Fields that this release is allowed to add to --once. They document the new
# live memory section; every other addition is a parity failure.
EXPECTED_NEW_FIELDS = ["arm mem used:", "gpu cma used:"]

BASELINE_FIELD_COUNT = 43
COUNTER_ROWS = 30
REGISTER_ROWS = 78

FIELD_RE = re.compile(r"^(\S[^:]*:)(?:\s+(.*))?$")
COUNTER_RE = re.compile(r"^\s+(\d{1,2})\s{2}(.*?)\s+(\d+)\s+(\d+\.\d+)$")
REGISTER_RE = re.compile(r"^\s{2}(\S+)\s+@0x([0-9a-fA-F]+)\s+0x[0-9a-fA-F]{8}\b")


def run(binary, *args):
    cmd = [binary, *args]
    if os.geteuid() != 0:
        cmd = ["sudo", "-n", *cmd]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit("vc4top %s failed (rc=%d): %s" %
                 (" ".join(args), res.returncode, res.stderr.strip()))
    return res.stdout


def fields(text):
    out = []
    for line in text.splitlines():
        if not line or line[0] in " \t":
            continue
        m = FIELD_RE.match(line.rstrip())
        if m:
            out.append(m.group(1))
    return out


def counter_rows(text):
    rows = []
    for line in text.splitlines():
        m = COUNTER_RE.match(line.rstrip())
        if m:
            rows.append((int(m.group(1)), m.group(2).rstrip()))
    return rows


def register_rows(text):
    rows = []
    for line in text.splitlines():
        m = REGISTER_RE.match(line)
        if m:
            rows.append((m.group(1), m.group(2).lower()))
    return rows


def is_subsequence(old, new):
    it = iter(new)
    return all(item in it for item in old)


def compare(label, old, new):
    if old == new:
        print("  %-22s %d -> %d  identical" % (label, len(old), len(new)))
        return True
    print("  %-22s %d -> %d  MISMATCH" % (label, len(old), len(new)))
    for i, (a, b) in enumerate(zip(old, new)):
        if a != b:
            print("    first difference at row %d: %r != %r" % (i + 1, a, b))
            break
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--baseline", required=True, help="previous release binary")
    ap.add_argument("--new", default="build/vc4top", help="candidate binary")
    args = ap.parse_args()

    try:
        lock = open("/tmp/vc4-gpu.lock", "a+")
    except PermissionError:
        # Another user created the lock root-owned; a read-only descriptor is
        # enough for flock(LOCK_EX) and still serializes GPU access.
        lock = open("/tmp/vc4-gpu.lock", "r")
    with lock:
        fcntl.flock(lock, fcntl.LOCK_EX)

        base_once = run(args.baseline, "--once")
        new_once = run(args.new, "--once")
        base_all = run(args.baseline, "--once", "--all-counters")
        new_all = run(args.new, "--once", "--all-counters")
        base_regs = run(args.baseline, "--once", "--regs")
        new_regs = run(args.new, "--once", "--regs")

    bf, nf = fields(base_once), fields(new_once)
    bc, nc = counter_rows(base_all), counter_rows(new_all)
    br, nr = register_rows(base_regs), register_rows(new_regs)

    ok = True
    print("vc4top field parity")
    print("  baseline: %s" % args.baseline)
    print("  new     : %s" % args.new)
    print()

    print("--once keyed fields")
    print("  baseline count         %d" % len(bf))
    print("  candidate count        %d" % len(nf))
    if len(bf) != BASELINE_FIELD_COUNT:
        print("  FAIL: baseline has %d fields, expected %d" % (len(bf), BASELINE_FIELD_COUNT))
        ok = False
    if not is_subsequence(bf, nf):
        print("  FAIL: baseline fields are not an ordered subsequence of the candidate")
        ok = False
    else:
        print("  old fields in order    yes (%d/%d)" % (len(bf), len(bf)))
    added = [f for f in nf if f not in bf]
    removed = [f for f in bf if f not in nf]
    print("  new fields             %s" % (", ".join(added) if added else "none"))
    if removed:
        print("  FAIL: removed fields    %s" % ", ".join(removed))
        ok = False
    unexpected = [f for f in added if f not in EXPECTED_NEW_FIELDS]
    missing_new = [f for f in EXPECTED_NEW_FIELDS if f not in added]
    if unexpected:
        print("  FAIL: unexpected new fields %s" % ", ".join(unexpected))
        ok = False
    if missing_new:
        print("  FAIL: documented memory fields missing: %s" % ", ".join(missing_new))
        ok = False
    print("  memory additions       %d (%s)" % (len(EXPECTED_NEW_FIELDS),
                                               " + ".join(EXPECTED_NEW_FIELDS)))
    print()

    print("counter / register rows")
    ok &= compare("all-counter rows", bc, nc)
    ok &= compare("register rows", br, nr)
    if len(bc) != COUNTER_ROWS:
        print("  FAIL: expected %d counter rows" % COUNTER_ROWS)
        ok = False
    if len(br) != REGISTER_ROWS:
        print("  FAIL: expected %d register rows" % REGISTER_ROWS)
        ok = False
    print()

    print("RESULT: %s  (43 old fields + %d new memory fields = %d fields; "
          "%d counters; %d registers)" %
          ("PASS" if ok else "FAIL", len(EXPECTED_NEW_FIELDS), len(bf) + len(EXPECTED_NEW_FIELDS),
           len(nc), len(nr)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
