#!/usr/bin/env python3
"""PE structural-equivalence differ for the putup release migration.

The putup (clang-cl + lld-link) build and the legacy MSVC build compile the same
sources with different toolchains, so their PE binaries are never byte-identical
(different codegen, timestamps, the MSVC 'Rich' header, section sizes). This tool
compares the parts that MUST still match for the two to be the same program --
the source- and spec-driven facets -- while ignoring the parts that legitimately
differ between compilers.

Two uses:
  * diff  A B         -- transitional gate: one putup binary vs its MSVC twin.
  * diff-tree PA PB   -- gate a whole build tree, matching binaries by basename.
  * extract PE        -- dump one binary's facets as JSON (debugging / self-check).

Asserted facets (a mismatch fails the gate): machine, subsystem, exe-vs-dll,
section-name set, imported DLL + symbol sets, exported symbol set, the non-
manifest resource-type set, the presence of version info, and the VERSIONINFO
fixed fields. Everything else -- timestamps, section sizes, DllCharacteristics
(CF-Guard/CET/ASLR bits), the manifest resource, checksum, debug directory -- is
reported for the record but never fails the gate.

Requires pefile (`pip install pefile`).
"""
from __future__ import annotations

import argparse
import json
import os
import sys

import pefile


# --- flag decoding -----------------------------------------------------------

def _flag_names(value, flagdict):
    """Decode a bitmask to sorted flag names using a pefile two-way flag dict
    (which holds both name->value and value->name entries)."""
    return sorted(name for name, bit in flagdict.items()
                  if isinstance(name, str) and isinstance(bit, int)
                  and bit and (value & bit) == bit)


def _machine(pe):
    return pefile.MACHINE_TYPE.get(pe.FILE_HEADER.Machine, hex(pe.FILE_HEADER.Machine))


def _subsystem(pe):
    return pefile.SUBSYSTEM_TYPE.get(pe.OPTIONAL_HEADER.Subsystem, pe.OPTIONAL_HEADER.Subsystem)


# --- facet extraction --------------------------------------------------------

def _imports(pe):
    out = {}
    for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        dll = entry.dll.decode("latin1").lower()
        syms = out.setdefault(dll, set())
        for imp in entry.imports:
            if imp.name:
                syms.add(imp.name.decode("latin1"))
            elif imp.ordinal is not None:
                syms.add(f"#ordinal:{imp.ordinal}")
    return {dll: sorted(syms) for dll, syms in out.items()}


def _exports(pe):
    exp = getattr(pe, "DIRECTORY_ENTRY_EXPORT", None)
    if not exp:
        return []
    return sorted(s.name.decode("latin1") for s in exp.symbols if s.name)


def _resource_types(pe):
    types = set()
    root = getattr(pe, "DIRECTORY_ENTRY_RESOURCE", None)
    if not root:
        return types
    for entry in root.entries:
        if entry.id is not None:
            types.add(pefile.RESOURCE_TYPE.get(entry.id, f"id:{entry.id}"))
        elif entry.name is not None:
            types.add(str(entry.name))
    return types


def _version_fixed(pe):
    """The toolchain-independent bits of VERSIONINFO: the numeric file/product
    version from VS_FIXEDFILEINFO plus the identity strings from StringFileInfo."""
    out = {}
    ffi = getattr(pe, "VS_FIXEDFILEINFO", None)
    if ffi:
        f = ffi[0]
        out["FileVersion"] = _ver(f.FileVersionMS, f.FileVersionLS)
        out["ProductVersion"] = _ver(f.ProductVersionMS, f.ProductVersionLS)
    for fileinfo in getattr(pe, "FileInfo", []) or []:
        for entry in fileinfo:
            if getattr(entry, "Key", b"") != b"StringFileInfo":
                continue
            for st in entry.StringTable:
                for key, val in st.entries.items():
                    k = key.decode("latin1")
                    if k in ("OriginalFilename", "InternalName", "CompanyName",
                             "ProductName", "FileVersion", "ProductVersion"):
                        out.setdefault("str:" + k, val.decode("latin1"))
    return out


def _ver(ms, ls):
    return f"{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}"


def extract(path):
    """Parse one PE into a normalized facet dict (asserted + informational)."""
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DEBUG"],
    ])
    ch = pe.FILE_HEADER.Characteristics
    is_dll = bool(ch & pefile.IMAGE_CHARACTERISTICS["IMAGE_FILE_DLL"])
    rtypes = _resource_types(pe)
    facets = {
        # --- asserted ---
        "machine": _machine(pe),
        "subsystem": _subsystem(pe),
        "is_dll": is_dll,
        "sections": sorted(s.Name.rstrip(b"\x00").decode("latin1") for s in pe.sections),
        "imports": _imports(pe),
        "exports": _exports(pe),
        "resource_types": sorted(t for t in rtypes if t != "RT_MANIFEST"),
        "has_version_info": "RT_VERSION" in rtypes,
        "version_fixed": _version_fixed(pe),
        # --- informational (reported, never asserted) ---
        "_info": {
            "has_manifest": "RT_MANIFEST" in rtypes,
            "characteristics": _flag_names(ch, pefile.IMAGE_CHARACTERISTICS),
            "dll_characteristics": _flag_names(pe.OPTIONAL_HEADER.DllCharacteristics,
                                               pefile.DLL_CHARACTERISTICS),
            "timestamp": hex(pe.FILE_HEADER.TimeDateStamp),
            "checksum": hex(pe.OPTIONAL_HEADER.CheckSum),
            "section_count": len(pe.sections),
            "has_debug_dir": bool(getattr(pe, "DIRECTORY_ENTRY_DEBUG", None)),
        },
    }
    pe.close()
    return facets


# --- comparison --------------------------------------------------------------

ASSERTED = ("machine", "subsystem", "is_dll", "sections", "imports", "exports",
            "resource_types", "has_version_info", "version_fixed")


def compare(a, b):
    """Return a list of human-readable strings, one per asserted-facet mismatch.
    Empty list == structurally equivalent."""
    diffs = []
    for key in ASSERTED:
        if key == "imports":
            diffs += _diff_imports(a["imports"], b["imports"])
        elif key in ("sections", "exports", "resource_types"):
            diffs += _diff_set(key, a[key], b[key])
        elif key == "version_fixed":
            diffs += _diff_dict("version", a[key], b[key])
        elif a[key] != b[key]:
            diffs.append(f"{key}: {a[key]!r} != {b[key]!r}")
    return diffs


def _diff_set(name, a, b):
    sa, sb = set(a), set(b)
    out = []
    if sa - sb:
        out.append(f"{name}: only in A: {sorted(sa - sb)}")
    if sb - sa:
        out.append(f"{name}: only in B: {sorted(sb - sa)}")
    return out


def _diff_imports(a, b):
    out = _diff_set("import DLLs", a.keys(), b.keys())
    for dll in sorted(set(a) & set(b)):
        out += _diff_set(f"imports[{dll}]", a[dll], b[dll])
    return out


def _diff_dict(name, a, b):
    out = []
    for k in sorted(set(a) | set(b)):
        if a.get(k) != b.get(k):
            out.append(f"{name}.{k}: {a.get(k)!r} != {b.get(k)!r}")
    return out


def _info_deltas(a, b):
    """Non-failing observations, for the record."""
    out = []
    for k in sorted(set(a["_info"]) | set(b["_info"])):
        if a["_info"].get(k) != b["_info"].get(k):
            out.append(f"  ~ {k}: {a['_info'].get(k)!r} vs {b['_info'].get(k)!r}")
    return out


# --- CLI ---------------------------------------------------------------------

def _pes_by_name(root):
    found = {}
    for dirpath, _, files in os.walk(root):
        for f in files:
            if f.lower().endswith((".exe", ".dll")):
                found.setdefault(f.lower(), os.path.join(dirpath, f))
    return found


def cmd_extract(args):
    print(json.dumps(extract(args.pe), indent=2, sort_keys=True))
    return 0


def cmd_diff(args):
    a, b = extract(args.a), extract(args.b)
    diffs = compare(a, b)
    label = f"{os.path.basename(args.a)}"
    if diffs:
        print(f"FAIL {label}: {len(diffs)} structural difference(s)")
        for d in diffs:
            print(f"  ! {d}")
    else:
        print(f"OK   {label}: structurally equivalent")
    if args.show_info:
        info = _info_deltas(a, b)
        if info:
            print("  (informational, not gated:)")
            print("\n".join(info))
    return 1 if diffs else 0


def cmd_diff_tree(args):
    pa, pb = _pes_by_name(args.tree_a), _pes_by_name(args.tree_b)
    common = sorted(set(pa) & set(pb))
    only_a, only_b = sorted(set(pa) - set(pb)), sorted(set(pb) - set(pa))
    if only_a:
        print(f"note: {len(only_a)} binary(ies) only in A (not compared): {only_a}")
    if only_b:
        print(f"note: {len(only_b)} binary(ies) only in B (not compared): {only_b}")
    failed = 0
    for name in common:
        diffs = compare(extract(pa[name]), extract(pb[name]))
        if diffs:
            failed += 1
            print(f"FAIL {name}: {len(diffs)} difference(s)")
            for d in diffs:
                print(f"  ! {d}")
        else:
            print(f"OK   {name}")
    print(f"\n{len(common) - failed}/{len(common)} binaries structurally equivalent")
    return 1 if failed else 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    pe_ex = sub.add_parser("extract", help="dump one PE's facets as JSON")
    pe_ex.add_argument("pe")
    pe_ex.set_defaults(func=cmd_extract)

    pe_df = sub.add_parser("diff", help="compare two PE binaries")
    pe_df.add_argument("a")
    pe_df.add_argument("b")
    pe_df.add_argument("--show-info", action="store_true",
                       help="also print non-gated informational deltas")
    pe_df.set_defaults(func=cmd_diff)

    pe_tr = sub.add_parser("diff-tree", help="compare two build trees by basename")
    pe_tr.add_argument("tree_a")
    pe_tr.add_argument("tree_b")
    pe_tr.set_defaults(func=cmd_diff_tree)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
