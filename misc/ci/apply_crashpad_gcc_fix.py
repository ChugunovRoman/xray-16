#!/usr/bin/env python3
"""
Idempotent fixes for Crashpad Linux sources so GCC can parse template-id after ! and ?:.
See misc/ci/patches/sentry-crashpad/0001-crashpad-linux-gcc-template-parse.patch (reference).
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "Externals" / "sentry-native"


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
            (
                """  INITIALIZATION_STATE_SET_INITIALIZING(initialized_);
  if (!(memory.Is64Bit() ? InitializeSpecific<Traits64>(memory, address)
                         : InitializeSpecific<Traits32>(memory, address))) {
    return false;
  }""",
                """  INITIALIZATION_STATE_SET_INITIALIZING(initialized_);
  bool init_ok;
  if (memory.Is64Bit()) {
    init_ok = InitializeSpecific<Traits64>(memory, address);
  } else {
    init_ok = InitializeSpecific<Traits32>(memory, address);
  }
  if (!init_ok) {
    return false;
  }""",
            ),
        ],
    )
    changed |= fix_file(
        ex,
        [
            (
                """  if (process_reader->Is64Bit()) {
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
  }""",
                """  if (process_reader->Is64Bit()) {
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
  }""",
            ),
        ],
    )

    if not changed:
        print("[apply_crashpad_gcc_fix] no changes (already fixed or tree differs)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
