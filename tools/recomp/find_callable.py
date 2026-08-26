"""Which ENC97 functions can be differentially tested if calls are allowed.

The leaf sweeps compare 974 functions. 5,316 of the 7,326 call something, and
extending to those is not a matter of safer inputs - it needs a decision about
what happens at the callee. This works out which functions that decision
actually reaches, and writes the list.

Measured first, because the answer changed the design. Each row counts the
functions that are loop-free with a fully known closure, and the flags say
which barrier is still treated as disqualifying:

    neither relaxed          1059    --strict-imports --strict-indirect
    imports stubbed only     1127    --strict-indirect          (+68)
    indirect tolerated only  1196    --strict-imports           (+137)
    both                     3292    (no flags)                 (+2233)

The barriers are entangled. Almost every function that calls an import also
makes an indirect call somewhere in its closure, so relaxing either alone gains
nothing much and relaxing both gains 3.1x. That is why the policy is a pair:

  IMPORTS are intercepted on BOTH sides by the same deterministic stub. Left
  real they would allocate, do I/O, or return a heap pointer that differs
  between two runs - none of which is a lift bug, all of which reads as one.

  INDIRECT CALLS are tolerated rather than disqualifying. Both sides compute
  the target from identical state, so both go to the same place: the lifted
  side through its dispatch table, the real side to real code. A target
  computed from a zeroed register is not a valid address and faults, which is
  caught like any other fault.

  LOOPS remain disqualifying anywhere in the closure. A backward branch can
  spin forever, and unlike a bad pointer there is nothing to catch.

    py find_callable.py            report the counts
    py find_callable.py --emit     write enc97_callable.h
"""
import argparse
import collections
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def load_analysis():
    """Reuse find_pure_chain's decoder rather than repeat it."""
    src = io.open(os.path.join(HERE, "find_pure_chain.py"), encoding="utf-8").read()
    ns = {"__name__": "m", "__file__": os.path.join(HERE, "find_pure_chain.py")}
    exec(compile(src[:src.index("# pure leaf: in funcs")], "fpc", "exec"), ns)
    return ns["analyze"], ns["funcs"]


def build(analyze, funcs):
    info = {}
    for a in funcs:
        r = analyze(a)
        if not r:
            continue
        has_call, calls, imp, ret_imm, ind, insns, writes, loop = r
        info[a] = dict(
            calls=[c for c in calls if c in funcs],
            # a direct call to something outside the function list is a target
            # nothing can be said about, so it disqualifies regardless
            ext=[c for c in calls if c not in funcs],
            imp=imp, ind=ind, loop=loop, writes=writes, ret=ret_imm,
            ninsn=len(insns))
    return info


# Which barriers are treated as disqualifying. Both off is the policy; turning
# them on one at a time is how the entanglement below was measured.
STRICT = {"imp": False, "ind": False}


def closure(a, info, memo, stack=()):
    """Everything `a` can reach, or None if it reaches something unusable.

    Recursion is not a disqualifier: a cycle adds no functions the walk has not
    already seen, so it contributes the empty set and the closure is still
    finite. It does mean the callee can recurse at run time, which is why the
    loop rule below is what bounds the work.
    """
    if a in memo:
        return memo[a]
    if a in stack:
        return set()
    d = info.get(a)
    if d is not None and ((STRICT["imp"] and d["imp"]) or (STRICT["ind"] and d["ind"])):
        memo[a] = None
        return None
    if d is None or d["ext"]:
        memo[a] = None
        return None
    out = {a}
    for c in d["calls"]:
        sub = closure(c, info, memo, stack + (a,))
        if sub is None:
            memo[a] = None
            return None
        out |= sub
    memo[a] = out
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit", action="store_true", help="write enc97_callable.h")
    ap.add_argument("--max-closure", type=int, default=8,
                    help="skip candidates that reach more functions than this")
    ap.add_argument("--strict-imports", action="store_true",
                    help="treat calling an import as disqualifying")
    ap.add_argument("--strict-indirect", action="store_true",
                    help="treat an indirect call as disqualifying")
    a = ap.parse_args()
    STRICT["imp"] = a.strict_imports
    STRICT["ind"] = a.strict_indirect

    analyze, funcs = load_analysis()
    info = build(analyze, funcs)
    memo = {}

    cand, stats = [], collections.Counter()
    for va in sorted(info):
        cl = closure(va, info, memo)
        if cl is None:
            stats["reaches an unknown target"] += 1
            continue
        if any(info[x]["loop"] for x in cl):
            stats["a loop somewhere in the closure"] += 1
            continue
        if len(cl) == 1:
            stats["leaf (the existing sweeps)"] += 1
            continue
        if len(cl) > a.max_closure:
            stats["closure too wide to attribute"] += 1
            continue
        cand.append((va, info[va]["ret"], sorted(cl)))
        stats["callable"] += 1

    for k, v in stats.most_common():
        print("  %-32s %5d" % (k, v))
    print("\n%d loop-free with a fully known closure; %d of them call" % (stats["leaf (the existing sweeps)"] + stats["callable"] + stats["closure too wide to attribute"], len(cand)))
    if cand:
        w = collections.Counter(len(c) for _v, _r, c in cand)
        print("closure sizes: %s" % ", ".join("%d fns:%d" % kv for kv in sorted(w.items())))

    if a.emit:
        p = os.path.join(HERE, "enc97_callable.h")
        with io.open(p, "w", encoding="utf-8", newline="\n") as h:
            h.write("/* AUTO-GENERATED by find_callable.py.\n"
                    "   Callers whose whole call graph is known code with no loop.\n"
                    "   Imports are stubbed identically on both sides; indirect calls\n"
                    "   are tolerated because both sides compute the same target. */\n")
            h.write("/* L(va, ret_imm_bytes, closure_size) */\n#define CALLABLE(L) \\\n")
            for va, ret, cl in cand:
                h.write("    L(0x%06x, %d, %d) \\\n" % (va, ret, len(cl)))
            h.write("\n")
        print("[*] wrote %s (%d callers)" % (p, len(cand)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
