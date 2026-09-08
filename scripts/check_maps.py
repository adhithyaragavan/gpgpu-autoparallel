#!/usr/bin/env python3
"""Tier 3 check for Days 15-16: decode the OpenMP map-clause lowering that
Clang actually emitted (the @.offload_sizes / @.offload_maptypes globals in
--offload-host-only -emit-llvm output) and cross-check it against the
map(...) clauses the tool itself reported for the same pragma.

This is a static check of what Clang's host-side runtime call will *tell the
offload runtime to move* -- it does not run a device and cannot prove the
runtime moves the bytes correctly. See NOTES.md for what each verification
tier does and does not prove.

Every mapped array in the current benchmark set is `double` (8 bytes); this
script does not infer element size, it is told via --elem-size.

A file with more than one offload region gets one @.offload_sizes[.N] /
@.offload_maptypes[.N] pair per call site, and Clang's numeric suffixes are
NOT in lockstep between the two globals (e.g. sizes get ".1" while the
matching maptypes get ".2") -- there is no way to pair them up generically.
For a single-region file (the only shape scripts/build_and_run.sh drives),
the unsuffixed default names are always the right ones. For a multi-region
file, find the correct pair yourself (`grep offload_sizes file.ll`) and pass
them with --sizes-global/--maptypes-global.

Usage:
    check_maps.py --ll <file.ll> --pragma "<pragma text>" [--elem-size 8] \
        [--sizes-global @.offload_sizes] [--maptypes-global @.offload_maptypes]
Exit code 0 if every assertion below passes or is explicitly skipped
(symbolic extent), 1 if any assertion fails.
"""
import argparse
import re
import sys

# llvm/Frontend/OpenMP/OMPConstants.h
OMP_MAP_TO = 0x01
OMP_MAP_FROM = 0x02
OMP_MAP_TARGET_PARAM = 0x20
OMP_MAP_ATTACH = 0x4000

DIRECTION_BITS = {
    "to": OMP_MAP_TO,
    "from": OMP_MAP_FROM,
    "tofrom": OMP_MAP_TO | OMP_MAP_FROM,
}


def decode_direction(maptype: int) -> str:
    bits = maptype & (OMP_MAP_TO | OMP_MAP_FROM)
    if bits == (OMP_MAP_TO | OMP_MAP_FROM):
        return "tofrom"
    if bits == OMP_MAP_TO:
        return "to"
    if bits == OMP_MAP_FROM:
        return "from"
    return "none"


def parse_global_i64_array(ir_text: str, global_name: str):
    # e.g. @.offload_sizes = private unnamed_addr constant [4 x i64] [i64 524288, i64 8, ...]
    pat = re.compile(
        re.escape(global_name) + r"\s*=.*?\[(\d+)\s*x\s*i64\]\s*\[(.*?)\]", re.S
    )
    m = pat.search(ir_text)
    if not m:
        return None
    count = int(m.group(1))
    body = m.group(2)
    values = [int(v) for v in re.findall(r"i64\s+(-?\d+)", body)]
    if len(values) != count:
        print(
            f"warning: {global_name} declared [{count} x i64] but parsed "
            f"{len(values)} values",
            file=sys.stderr,
        )
    return values


def parse_map_clauses(pragma: str):
    # map(from: out[0:65536])  /  map(to: in[0:n])  /  map(tofrom: a[0:8])
    clauses = []
    for m in re.finditer(
        r"map\((\w+):\s*(\w+)\[(\w+):(\w+)\]\)", pragma
    ):
        direction, var, lo, extent = m.groups()
        clauses.append({"direction": direction, "var": var, "lo": lo, "extent": extent})
    return clauses


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ll", required=True, help="path to --offload-host-only -emit-llvm output")
    ap.add_argument("--pragma", required=True, help="verbatim pragma text reported by p05tool")
    ap.add_argument("--elem-size", type=int, default=8, help="bytes per mapped element (default: 8, double)")
    ap.add_argument("--sizes-global", default="@.offload_sizes",
                     help="exact global name to read map sizes from (see multi-region note above)")
    ap.add_argument("--maptypes-global", default="@.offload_maptypes",
                     help="exact global name to read map types from (see multi-region note above)")
    ap.add_argument("--skip-leading-data", type=int, default=0,
                     help="skip this many leading non-ATTACH IR entries before pairing with "
                          "pragma clauses -- needed when a scalar used inside the target region "
                          "(e.g. a variable-extent guard) gets an implicit map/firstprivate entry "
                          "ahead of the explicit map() clauses; observed for the guarded case in "
                          "tests/profitability_cases.c, see NOTES.md")
    args = ap.parse_args()

    ir_text = open(args.ll).read()
    sizes = parse_global_i64_array(ir_text, args.sizes_global)
    maptypes = parse_global_i64_array(ir_text, args.maptypes_global)

    if sizes is None or maptypes is None:
        print("FAIL: could not find @.offload_sizes / @.offload_maptypes in", args.ll)
        return 1
    if len(sizes) != len(maptypes):
        print(f"FAIL: {len(sizes)} sizes vs {len(maptypes)} maptypes")
        return 1

    # Each map(...) clause lowers to one TARGET_PARAM data entry plus (for a
    # pointer/array argument) one ATTACH entry carrying the pointer itself.
    # Drop ATTACH entries; what's left should line up 1:1, in order, with the
    # clauses as written in the pragma.
    data_entries = [
        (sz, mt) for sz, mt in zip(sizes, maptypes) if not (mt & OMP_MAP_ATTACH)
    ]
    if args.skip_leading_data:
        skipped, data_entries = data_entries[:args.skip_leading_data], data_entries[args.skip_leading_data:]
        for i, (sz, mt) in enumerate(skipped):
            print(f"  skipping implicit data[{i}]: size={sz} maptype=0x{mt:x} "
                  f"(not one of the pragma's map() clauses)")

    clauses = parse_map_clauses(args.pragma)

    print(f"decoded {len(sizes)} IR map entries ({len(data_entries)} data, "
          f"{len(sizes) - len(data_entries)} attach) vs {len(clauses)} pragma clause(s)")
    for i, (sz, mt) in enumerate(zip(sizes, maptypes)):
        kind = "ATTACH" if (mt & OMP_MAP_ATTACH) else decode_direction(mt)
        tp = "+TARGET_PARAM" if (mt & OMP_MAP_TARGET_PARAM) else ""
        print(f"  ir[{i}]: size={sz:>8}  maptype=0x{mt:x} ({kind}{tp})")

    ok = True

    if len(data_entries) != len(clauses):
        print(f"FAIL: {len(data_entries)} data entries != {len(clauses)} map() clauses")
        ok = False

    for i, ((sz, mt), clause) in enumerate(zip(data_entries, clauses)):
        decoded_dir = decode_direction(mt)
        want_dir = clause["direction"]
        tag = f"clause {i} ({clause['var']})"
        if decoded_dir != want_dir:
            print(f"FAIL: {tag}: IR says '{decoded_dir}', pragma says '{want_dir}'")
            ok = False
        else:
            print(f"PASS: {tag}: direction '{decoded_dir}' matches pragma")

        if not (mt & OMP_MAP_TARGET_PARAM):
            print(f"FAIL: {tag}: IR entry missing TARGET_PARAM")
            ok = False

        extent = clause["extent"]
        if extent.lstrip("-").isdigit():
            want_bytes = int(extent) * args.elem_size
            if sz != want_bytes:
                print(f"FAIL: {tag}: IR size {sz} B != extent {extent} * elem-size "
                      f"{args.elem_size} = {want_bytes} B")
                ok = False
            else:
                print(f"PASS: {tag}: size {sz} B == {extent} * {args.elem_size}")
        else:
            print(f"SKIP: {tag}: extent '{extent}' is symbolic, not a literal -- "
                  f"size check requires evaluating it at the call site (see NOTES.md)")

    if ok:
        print("RESULT: PASS")
        return 0
    print("RESULT: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
