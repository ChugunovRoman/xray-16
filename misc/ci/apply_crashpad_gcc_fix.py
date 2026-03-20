#!/usr/bin/env python3
"""
Idempotent fixes for Crashpad Linux sources under GCC + CMAKE_UNITY_BUILD.

- debug_rendezvous: `= Foo<Bar>(...)` and ternary + template are parsed as comparisons;
  use `this->template InitializeSpecific<...>(...)`.
- exception_snapshot_linux: `!(ReadSiginfo<...>(...))` breaks parsing; use local bools.

Invoked from misc/ci/ensure_sentry_native.sh after clone/submodule init.
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "Externals" / "sentry-native"

DEBUG_FINAL = """  INITIALIZATION_STATE_SET_INITIALIZING(initialized_);
  // Unity build + GCC: use template keyword so <Traits*> is a template-arg list,
  // not comparison (init_ok = InitializeSpecific < Traits64 > ...).
  bool init_ok;
  if (memory.Is64Bit()) {
    init_ok = this->template InitializeSpecific<Traits64>(memory, address);
  } else {
    init_ok = this->template InitializeSpecific<Traits32>(memory, address);
  }
  if (!init_ok) {
    return false;
  }"""

DEBUG_OLD_TERNARY = """  INITIALIZATION_STATE_SET_INITIALIZING(initialized_);
  if (!(memory.Is64Bit() ? InitializeSpecific<Traits64>(memory, address)
                         : InitializeSpecific<Traits32>(memory, address))) {
    return false;
  }"""

DEBUG_OLD_ASSIGN = """  INITIALIZATION_STATE_SET_INITIALIZING(initialized_);
  bool init_ok;
  if (memory.Is64Bit()) {
    init_ok = InitializeSpecific<Traits64>(memory, address);
  } else {
    init_ok = InitializeSpecific<Traits32>(memory, address);
  }
  if (!init_ok) {
    return false;
  }"""

EX_FINAL = """  if (process_reader->Is64Bit()) {
    // Unity build + GCC: avoid !(ReadSiginfo<...>) — ! and < interact badly; use locals.
    const bool context_ok_64 =
        ReadContext<ContextTraits64>(process_reader, context_address);
    const bool siginfo_ok_64 =
        ReadSiginfo<Traits64>(process_reader, siginfo_address);
    if (!context_ok_64 || !siginfo_ok_64) {
      return false;
    }
  } else {
#if !defined(ARCH_CPU_RISCV64)
    const bool context_ok_32 =
        ReadContext<ContextTraits32>(process_reader, context_address);
    const bool siginfo_ok_32 =
        ReadSiginfo<Traits32>(process_reader, siginfo_address);
    if (!context_ok_32 || !siginfo_ok_32) {
      return false;
    }
#endif
  }"""

EX_OLD_PLAIN = """  if (process_reader->Is64Bit()) {
    if (!ReadContext<ContextTraits64>(process_reader, context_address) ||
        !ReadSiginfo<Traits64>(process_reader, siginfo_address)) {
      return false;
    }
  } else {
#if !defined(ARCH_CPU_RISCV64)
    if (!ReadContext<ContextTraits32>(process_reader, context_address) ||
        !ReadSiginfo<Traits32>(process_reader, siginfo_address)) {
      return false;
    }
#endif
  }"""

EX_OLD_PAREN = """  if (process_reader->Is64Bit()) {
    if (!(ReadContext<ContextTraits64>(process_reader, context_address)) ||
        !(ReadSiginfo<Traits64>(process_reader, siginfo_address))) {
      return false;
    }
  } else {
#if !defined(ARCH_CPU_RISCV64)
    if (!(ReadContext<ContextTraits32>(process_reader, context_address)) ||
        !(ReadSiginfo<Traits32>(process_reader, siginfo_address))) {
      return false;
    }
#endif
  }"""


def fix_file(path: Path, replacements: list[tuple[str, str]]) -> bool:
    if not path.is_file():
        return False
    raw = path.read_bytes().replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    data = raw.decode("utf-8", errors="replace")
    orig = data
    for old, new in replacements:
        if old in data:
            data = data.replace(old, new, 1)
    if data != orig:
        path.write_text(data, encoding="utf-8", newline="\n")
        print(f"[apply_crashpad_gcc_fix] updated {path.relative_to(ROOT)}")
        return True
    return False


def main() -> int:
    if not NATIVE.is_dir():
        print("[apply_crashpad_gcc_fix] skip: no Externals/sentry-native", file=sys.stderr)
        return 0

    dr = NATIVE / "external/crashpad/snapshot/linux/debug_rendezvous.cc"
    ex = NATIVE / "external/crashpad/snapshot/linux/exception_snapshot_linux.cc"

    changed = False
    changed |= fix_file(
        dr,
        [
            (DEBUG_OLD_TERNARY, DEBUG_FINAL),
            (DEBUG_OLD_ASSIGN, DEBUG_FINAL),
        ],
    )
    changed |= fix_file(
        ex,
        [
            (EX_OLD_PLAIN, EX_FINAL),
            (EX_OLD_PAREN, EX_FINAL),
        ],
    )

    if not changed:
        print("[apply_crashpad_gcc_fix] no changes (already fixed or tree differs)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
