#!/usr/bin/env python3
"""
make_nosubxact_variant.py — emit a copy of kwabi_runtime_shim.c with the
subtransaction calls removed from shim_call_impl.

Used by subxact-probe.sh to build the negative control for fmgr-api check 13:
a bundle that catches a failed call but does NOT roll it back. If the probe
then reports the partial write surviving, check 13 is discriminating rather
than passing for free.

LONGEST NAME FIRST. RollbackAndReleaseCurrentSubTransaction() contains
ReleaseCurrentSubTransaction() as a substring, so replacing the short name
first leaves a dangling "RollbackAnd" and the file will not compile.

Usage: make_nosubxact_variant.py <input.c> <output.c>
"""

import sys

src_path, out_path = sys.argv[1], sys.argv[2]
src = open(src_path).read()

start = src.index("shim_call_impl(FmgrInfo *flinfo")
end = src.index("static KwabiStatus\nshim_call_function(", start)
body = src[start:end]

before = body
body = body.replace("RollbackAndReleaseCurrentSubTransaction();", "/* CONTROL: no rollback */")
body = body.replace("ReleaseCurrentSubTransaction();", "/* CONTROL: no commit */")
body = body.replace("BeginInternalSubTransaction(NULL);", "/* CONTROL: no subtransaction */")

assert body != before, "no replacements made — did shim_call_impl change?"
for bad in ("RollbackAnd", "ReleaseCurrentSubTransaction", "BeginInternalSubTransaction"):
    assert bad not in body, f"leftover {bad!r} — replacement order is wrong"

open(out_path, "w").write(src[:start] + body + src[end:])
print(f"wrote {out_path}: subtransaction removed from shim_call_impl")
