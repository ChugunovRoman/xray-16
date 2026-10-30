#!/usr/bin/env bash
# Game-layout zips of the engine for the launcher (plans/launcher/engine-distribution-plan.md, sec. 4):
#   engine-<os>[-<distro>]-<arch>-<edition>.zip   executables and shared libraries, files in the root of the zip
#   symbols-<os>[-<distro>]-<arch>-<edition>.zip  debug info: <file>.debug (Linux) / <file>.dSYM (macOS)
# The names follow the launcher (launcher-new/src-tauri/src/utils/engine.rs: normalize_arch, normalize_edition).
# The binaries in the engine zip are stripped; Linux ones keep a .gnu_debuglink to their .debug file.
#
#   misc/pack_game_layout.sh <os: linux|macos> <distro|-> <arch> <configuration> <build output dir>
# The zips are written to the current directory.
set -eu

os="$1"
distro="$2"
arch_raw="$3"
config="$4"
build_out="$5"

case "$(echo "$arch_raw" | tr '[:upper:]' '[:lower:]')" in
x86_64 | x64 | amd64) arch=amd64 ;;
aarch64 | arm64) arch=arm64 ;;
x86 | i386 | i686) arch=x86 ;;
*) arch=$(echo "$arch_raw" | tr '[:upper:]' '[:lower:]') ;;
esac

case "$(echo "$config" | tr '[:upper:]' '[:lower:]' | tr -d ' _-')" in
release) edition=release ;;
debug) edition=debug ;;
mixed) edition=mixed ;;
mastergold | releasemastergold) edition=mastergold ;;
*)
    echo "::error::pack_game_layout: unknown configuration '$config'" >&2
    exit 1
    ;;
esac

# The distro token is only in Linux names
if [ "$os" = linux ] && [ "$distro" != "-" ] && [ -n "$distro" ]; then
    suffix="$os-$distro-$arch-$edition"
else
    suffix="$os-$arch-$edition"
fi
engine_zip="$PWD/engine-$suffix.zip"
symbols_zip="$PWD/symbols-$suffix.zip"

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/engine" "$stage/symbols"

# The binaries of the game bin/: the executables and the shared libraries in the root of the build output
# (the renderers are shared libraries there too)
count=0
for file in "$build_out"/*; do
    [ -f "$file" ] || continue
    name=$(basename "$file")
    case "$name" in
    *.so | *.so.* | *.dylib) ;;
    *.*) continue ;;
    *) [ -x "$file" ] || continue ;;
    esac
    cp -p "$file" "$stage/engine/$name"
    count=$((count + 1))
done
if [ "$count" -eq 0 ]; then
    echo "::error::pack_game_layout: no executables or shared libraries in $build_out" >&2
    exit 1
fi

# Split debug info: the symbols go to their own zip, the engine zip gets stripped binaries
symbols=0
for file in "$stage/engine"/*; do
    name=$(basename "$file")
    if [ "$os" = macos ]; then
        if command -v dsymutil >/dev/null 2>&1 && dsymutil "$file" -o "$stage/symbols/$name.dSYM" 2>/dev/null; then
            strip -S "$file" 2>/dev/null || true
            symbols=$((symbols + 1))
        fi
    else
        if command -v objcopy >/dev/null 2>&1 && objcopy --only-keep-debug "$file" "$stage/symbols/$name.debug" 2>/dev/null; then
            objcopy --strip-debug "$file" 2>/dev/null || true
            (cd "$stage/symbols" && objcopy --add-gnu-debuglink="$name.debug" "$file" 2>/dev/null) || true
            symbols=$((symbols + 1))
        fi
    fi
done

rm -f "$engine_zip" "$symbols_zip"
(cd "$stage/engine" && zip -q -r -9 "$engine_zip" .)
echo "pack_game_layout: $count file(s) -> $(basename "$engine_zip")"
if [ "$symbols" -gt 0 ]; then
    (cd "$stage/symbols" && zip -q -r -9 "$symbols_zip" .)
    echo "pack_game_layout: $symbols symbol file(s) -> $(basename "$symbols_zip")"
else
    echo "::warning::pack_game_layout: no debug info extracted (objcopy/dsymutil missing?), no symbols zip"
fi
