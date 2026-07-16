#!/usr/bin/env python3
"""Tests for pe_diff.py.

Two layers:
  * Pure-logic tests of compare() with crafted facet dicts -- portable, always run.
  * Real-PE tests that parse binaries under build-win*/ if present -- skipped when
    those trees are absent (e.g. a checkout with no local build).

Run: python3 ci_scripts/pe_diff_test.py
"""
import copy
import os
import shutil
import sys
import tempfile

import pe_diff

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def _base_facets():
    return {
        "machine": "IMAGE_FILE_MACHINE_AMD64",
        "subsystem": "IMAGE_SUBSYSTEM_WINDOWS_GUI",
        "is_dll": False,
        "sections": [".text", ".rdata", ".rsrc"],
        "imports": {"kernel32.dll": ["CreateFileW", "ExitProcess"],
                    "user32.dll": ["MessageBoxW"]},
        "exports": ["GetParam", "SetParam"],
        "resource_types": ["RT_DIALOG", "RT_ICON", "RT_VERSION"],
        "has_version_info": True,
        "version_fixed": {"FileVersion": "5.4.0.0", "str:OriginalFilename": "ttermpro.exe"},
        "_info": {"timestamp": "0x1", "checksum": "0x0", "has_manifest": True,
                  "characteristics": [], "dll_characteristics": [],
                  "section_count": 3, "has_debug_dir": False},
    }


# --- pure-logic tests --------------------------------------------------------

def test_identical_is_equivalent():
    a = _base_facets()
    assert pe_diff.compare(a, copy.deepcopy(a)) == []


def test_machine_mismatch_detected():
    a = _base_facets()
    b = copy.deepcopy(a)
    b["machine"] = "IMAGE_FILE_MACHINE_I386"
    diffs = pe_diff.compare(a, b)
    assert any("machine" in d for d in diffs), diffs


def test_import_symbol_mismatch_detected():
    a = _base_facets()
    b = copy.deepcopy(a)
    b["imports"]["kernel32.dll"] = ["CreateFileW"]  # dropped ExitProcess
    diffs = pe_diff.compare(a, b)
    assert any("kernel32" in d and "ExitProcess" in d for d in diffs), diffs


def test_missing_import_dll_detected():
    a = _base_facets()
    b = copy.deepcopy(a)
    del b["imports"]["user32.dll"]
    diffs = pe_diff.compare(a, b)
    assert any("import DLLs" in d and "user32.dll" in d for d in diffs), diffs


def test_export_mismatch_detected():
    a = _base_facets()
    b = copy.deepcopy(a)
    b["exports"] = ["GetParam"]
    diffs = pe_diff.compare(a, b)
    assert any("exports" in d and "SetParam" in d for d in diffs), diffs


def test_version_field_mismatch_detected():
    a = _base_facets()
    b = copy.deepcopy(a)
    b["version_fixed"]["str:OriginalFilename"] = "other.exe"
    diffs = pe_diff.compare(a, b)
    assert any("OriginalFilename" in d for d in diffs), diffs


def test_info_only_delta_is_tolerated():
    """The whole point: two PEs differing only in _info (timestamp, checksum,
    manifest, DllCharacteristics) are structurally equivalent."""
    a = _base_facets()
    b = copy.deepcopy(a)
    b["_info"]["timestamp"] = "0xdeadbeef"
    b["_info"]["checksum"] = "0x12345"
    b["_info"]["has_manifest"] = False
    b["_info"]["dll_characteristics"] = ["IMAGE_DLLCHARACTERISTICS_GUARD_CF"]
    b["_info"]["has_debug_dir"] = True
    assert pe_diff.compare(a, b) == []


def test_manifest_resource_not_asserted():
    """RT_MANIFEST lives in _info, not resource_types -- an MSVC default manifest
    on a binary putup leaves manifest-less must not fail the gate."""
    a = _base_facets()
    assert "RT_MANIFEST" not in a["resource_types"]


# --- real-PE tests (skipped if no local build) -------------------------------

X64 = os.path.join(REPO, "build-win", "teraterm", "teraterm", "ttermpro.exe")
X86 = os.path.join(REPO, "build-win32", "teraterm", "teraterm", "ttermpro.exe")
DLL = os.path.join(REPO, "build-win", "ttssh2", "ttxssh", "ttxssh.dll")


def test_extract_real_x64_exe():
    if not os.path.exists(X64):
        return "skip (no build-win)"
    f = pe_diff.extract(X64)
    assert f["machine"] == "IMAGE_FILE_MACHINE_AMD64", f["machine"]
    assert f["is_dll"] is False
    assert "ttpcmn.dll" in f["imports"], sorted(f["imports"])
    assert "DequoteParam" in f["exports"]
    assert f["has_version_info"] is True
    assert f["_info"]["has_manifest"] is True  # teraterm.exe carries a manifest
    # flag decode must actually resolve names (regression: empty on wrong dict)
    assert "IMAGE_FILE_EXECUTABLE_IMAGE" in f["_info"]["characteristics"]
    assert f["_info"]["dll_characteristics"], "dll_characteristics decoded empty"


def test_extract_real_dll_no_manifest():
    if not os.path.exists(DLL):
        return "skip (no build-win)"
    f = pe_diff.extract(DLL)
    assert f["is_dll"] is True
    assert f["has_version_info"] is True
    assert f["_info"]["has_manifest"] is False  # ttxssh has no manifest rc


def test_diff_real_selfsame_is_equivalent():
    if not os.path.exists(X64):
        return "skip (no build-win)"
    assert pe_diff.compare(pe_diff.extract(X64), pe_diff.extract(X64)) == []


def test_diff_real_crossarch_flags_machine():
    if not (os.path.exists(X64) and os.path.exists(X86)):
        return "skip (need build-win + build-win32)"
    diffs = pe_diff.compare(pe_diff.extract(X64), pe_diff.extract(X86))
    assert any("machine" in d for d in diffs), diffs


def test_tolerate_timestamp_on_real_pe():
    """Behavioral proof the tolerate logic holds on a real binary: patch only the
    TimeDateStamp of a copy and confirm the gate still calls it equivalent."""
    if not os.path.exists(X64):
        return "skip (no build-win)"
    import pefile
    with tempfile.TemporaryDirectory() as d:
        patched = os.path.join(d, "patched.exe")
        shutil.copy(X64, patched)
        pe = pefile.PE(patched)
        pe.FILE_HEADER.TimeDateStamp ^= 0xFFFFFFFF  # flip every bit
        pe.write(patched)
        pe.close()
        a, b = pe_diff.extract(X64), pe_diff.extract(patched)
        assert a["_info"]["timestamp"] != b["_info"]["timestamp"], "patch didn't take"
        assert pe_diff.compare(a, b) == [], "timestamp should be tolerated"


# --- selfcheck (pure-logic) --------------------------------------------------

def _tree_facets(machine="IMAGE_FILE_MACHINE_AMD64", with_apps=True):
    def one(manifest=False, ver=False):
        f = _base_facets()
        f["machine"] = machine
        f["_info"]["has_manifest"] = manifest
        f["has_version_info"] = ver
        return f
    d = {"ttpcmn.dll": one(), "ttxssh.dll": one()}
    if with_apps:
        for app in pe_diff.MANIFEST_APPS:
            d[app] = one(manifest=True, ver=True)
    return d


def test_selfcheck_clean_passes():
    assert pe_diff.selfcheck(_tree_facets(), "x64") == []


def test_selfcheck_wrong_machine_fails():
    problems = pe_diff.selfcheck(_tree_facets("IMAGE_FILE_MACHINE_I386"), "x64")
    assert problems and all("machine" in p for p in problems), problems


def test_selfcheck_missing_gui_app_fails():
    d = _tree_facets()
    del d["ttpmenu.exe"]
    problems = pe_diff.selfcheck(d, "x64")
    assert any("ttpmenu.exe" in p and "not found" in p for p in problems), problems


def test_selfcheck_gui_app_without_manifest_fails():
    d = _tree_facets()
    d["ttermpro.exe"]["_info"]["has_manifest"] = False
    problems = pe_diff.selfcheck(d, "x64")
    assert any("ttermpro.exe" in p and "manifest" in p for p in problems), problems


def test_selfcheck_gui_app_without_version_fails():
    d = _tree_facets()
    d["ttpmacro.exe"]["has_version_info"] = False
    problems = pe_diff.selfcheck(d, "x64")
    assert any("ttpmacro.exe" in p and "version" in p for p in problems), problems


# --- selfcheck (real trees) --------------------------------------------------

TREES = {"x64": os.path.join(REPO, "build-win"),
         "x86": os.path.join(REPO, "build-win32"),
         "arm64": os.path.join(REPO, "build-arm64")}


def _real_selfcheck(arch):
    tree = TREES[arch]
    if not os.path.isdir(tree):
        return None
    facets = {n: pe_diff.extract(p) for n, p in pe_diff._pes_by_name(tree).items()}
    return pe_diff.selfcheck(facets, arch)


def test_selfcheck_real_x64():
    r = _real_selfcheck("x64")
    return "skip (no build-win)" if r is None else (_assert_empty(r) or None)


def test_selfcheck_real_x86():
    r = _real_selfcheck("x86")
    return "skip (no build-win32)" if r is None else (_assert_empty(r) or None)


def test_selfcheck_real_arm64():
    r = _real_selfcheck("arm64")
    return "skip (no build-arm64)" if r is None else (_assert_empty(r) or None)


def test_selfcheck_real_wrong_arch_fails():
    tree = TREES["x64"]
    if not os.path.isdir(tree):
        return "skip (no build-win)"
    facets = {n: pe_diff.extract(p) for n, p in pe_diff._pes_by_name(tree).items()}
    problems = pe_diff.selfcheck(facets, "arm64")  # x64 tree, wrong expected arch
    assert problems and all("machine" in p for p in problems), problems


def _assert_empty(r):
    assert r == [], r


# --- runner ------------------------------------------------------------------

def main():
    tests = [(n, f) for n, f in sorted(globals().items())
             if n.startswith("test_") and callable(f)]
    passed = skipped = failed = 0
    for name, fn in tests:
        try:
            result = fn()
            if isinstance(result, str) and result.startswith("skip"):
                print(f"SKIP {name} -- {result}")
                skipped += 1
            else:
                print(f"PASS {name}")
                passed += 1
        except AssertionError as e:
            print(f"FAIL {name}: {e}")
            failed += 1
        except Exception as e:  # noqa: BLE001
            print(f"ERROR {name}: {type(e).__name__}: {e}")
            failed += 1
    print(f"\n{passed} passed, {skipped} skipped, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.path.insert(0, HERE)
    sys.exit(main())
