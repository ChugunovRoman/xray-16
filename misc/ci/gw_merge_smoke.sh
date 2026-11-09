#!/usr/bin/env bash
# Smoke test of a built gw_merge (src/utils/gw_merge): the usage, a merge of two addons, a conflict, the check of a
# text, the plan of an installation over a base folder, a snapshot of the catalog and a list of the files of the base
# and, when a LuaJIT library is given, that --check-lua loads it. Runs in the job gw_merge of
# .github/workflows/cibuild.yml on the packed tool; locally (Git Bash) on gw_merge.exe the same way.
#
#   bash misc/ci/gw_merge_smoke.sh <gw_merge> [<LuaJIT library>]
#
# Exit code: 0 - every check passed, 1 - a check failed (the failed one is printed).
set -euo pipefail

GW_MERGE="${1:?usage: gw_merge_smoke.sh <gw_merge> [<LuaJIT library>]}"
LUA_LIB="${2:-}"

# Absolute paths: the test runs in a temporary folder
absolute() {
    echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
}
[ -f "$GW_MERGE" ] || { echo "::error::gw_merge smoke test: no file '$GW_MERGE'"; exit 1; }
GW_MERGE="$(absolute "$GW_MERGE")"
if [ -n "$LUA_LIB" ]; then
    [ -f "$LUA_LIB" ] || { echo "::error::gw_merge smoke test: no file '$LUA_LIB'"; exit 1; }
    LUA_LIB="$(absolute "$LUA_LIB")"
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

fail() {
    echo "::error::gw_merge smoke test: $*"
    exit 1
}

# Runs gw_merge, expects the exit code $1; the output goes to out.txt / err.txt
expect() {
    local want="$1"
    shift
    local rc=0
    "$GW_MERGE" "$@" >out.txt 2>err.txt || rc=$?
    if [ "$rc" -ne "$want" ]; then
        cat out.txt err.txt
        fail "'gw_merge $*' exited with $rc, expected $want"
    fi
}

# 1. No arguments: the usage, code 2
expect 2
grep -q "usage:" err.txt || fail "no usage text"

# 2. Two addons change different keys of one section: merged, code 0
printf '[s]\na = 1\nb = 2\n' > base.ltx
printf '[s]\na = 10\nb = 2\n' > one.ltx
printf '[s]\na = 1\nb = 20\n' > two.ltx
expect 0 merge ltx --base base.ltx --side one=one.ltx --side two=two.ltx --name configs/test.ltx \
    -o merged.ltx --report report.json
printf '[s]\na = 10\nb = 20\n' > expected.ltx
cmp -s merged.ltx expected.ltx || { cat merged.ltx; fail "unexpected merge result"; }
grep -q '"status": "merged"' report.json || { cat report.json; fail "the report is not 'merged'"; }
grep -q '"algorithm": "gw-merge/' report.json || { cat report.json; fail "the report has no algorithm"; }

# 3. Two addons change the same key: an unresolved conflict, code 1
printf '[s]\na = 99\nb = 2\n' > three.ltx
expect 1 merge ltx --base base.ltx --side one=one.ltx --side three=three.ltx -o conflict.ltx --report conflict.json

# 3a. Compatibility patches with targets of their own (--patch-targets): p1 for a+b, p2 for b+c - each decides the
# conflict of its addons (resolved_by patch), code 0. One --patches for both could decide one of them only
printf '[s]\nx = 1\ny = 1\nz = 1\n' > pbase.ltx
printf '[s]\nx = 2\ny = 1\nz = 1\n' > pa.ltx
printf '[s]\nx = 3\ny = 3\nz = 1\n' > pb.ltx
printf '[s]\nx = 1\ny = 4\nz = 1\n' > pc.ltx
printf '[s]\nx = 9\n' > p1.ltx
printf '[s]\ny = 8\n' > p2.ltx
expect 0 merge ltx --base pbase.ltx --side a=pa.ltx --side b=pb.ltx --side c=pc.ltx --patch p1=p1.ltx \
    --patch p2=p2.ltx --patch-targets p1=a,b --patch-targets p2=b,c -o patched.ltx --report patched.json
printf '[s]\nx = 9\ny = 8\nz = 1\n' > expected.ltx
cmp -s patched.ltx expected.ltx || { cat patched.ltx; fail "unexpected result of two patches"; }
[ "$(grep -c '"resolved_by": "patch"' patched.json)" -eq 2 ] || { cat patched.json; fail "the patches did not decide"; }
# The old form (one --patches for every patch) still works: p2 taken for a+b leaves the conflict of b and c
expect 1 merge ltx --base pbase.ltx --side a=pa.ltx --side b=pb.ltx --side c=pc.ltx --patch p1=p1.ltx \
    --patch p2=p2.ltx --patches a,b -o patched.ltx --report patched.json
# A target that is not a side: the patch is skipped as the engine skips it (not_a_side), the conflict stays
expect 1 merge ltx --base pbase.ltx --side a=pa.ltx --side b=pb.ltx --patch p1=p1.ltx --patch-targets p1=a,d \
    -o patched.ltx --report patched.json
grep -q "not_a_side: d" err.txt || { cat err.txt; fail "no not_a_side"; }
grep -q '"addons": \["a", "b"\]' patched.json || { cat patched.json; fail "a skipped patch is a side"; }
# --patch-targets of no --patch: an error
expect 2 merge ltx --base pbase.ltx --side a=pa.ltx --side b=pb.ltx --patch p1=p1.ltx --patch-targets p9=a,b

# 4. The check of a text: a valid ltx passes, a broken script fails
expect 0 check ltx merged.ltx
printf 'function f()\n  return 1\nend\n' > ok.script
printf 'function f(\n  return 1\n' > bad.script
expect 1 check script bad.script

# 4a. plan (00-roadmap.md 5a, decision 63): the plan of an installation from folders - the sides, [private], the
# folders "@...", a compatibility patch, the rules of appdata/addons.ltx, the merge with --merge
mkdir -p plan/base/configs plan/addons/one/gamedata/configs plan/addons/two/gamedata/configs \
    plan/addons/two/gamedata/@alias/configs plan/addons/three/merge/gamedata/configs
printf '[s]\na = 1\nb = 2\nc = 3\n' > plan/base/configs/x.ltx
printf '[t]\nk = 1\n' > plan/base/configs/y.ltx
printf '[addon]\nid = one\nversion = 1.0.0\n[private]\nconfigs\\y.ltx\n' > plan/addons/one/addon.ltx
printf '[s]\na = 10\nb = 2\nc = 3\n' > plan/addons/one/gamedata/configs/x.ltx
printf '[t]\nk = 5\n' > plan/addons/one/gamedata/configs/y.ltx
printf '[addon]\nid = two\nversion = 1.0.0\n' > plan/addons/two/addon.ltx
printf '[s]\na = 11\nb = 20\nc = 3\n' > plan/addons/two/gamedata/configs/x.ltx
printf '[t]\nk = 6\n' > plan/addons/two/gamedata/configs/y.ltx
printf '[s]\na = 12\n' > plan/addons/two/gamedata/@alias/configs/x.ltx
printf '[addon]\nid = three\nversion = 1.0.0\npatches = one, two\n' > plan/addons/three/addon.ltx
printf '[s]\na = 99\n' > plan/addons/three/merge/gamedata/configs/x.ltx
expect 0 plan --base plan/base --addon plan/addons/one --addon plan/addons/two --addon plan/addons/three --merge \
    --texts plan/out -o plan.json
grep -q '"plan_format": 1' plan.json || { cat plan.json; fail "plan: no plan_format"; }
grep -q '"resolved_by": "patch"' plan.json || { cat plan.json; fail "plan: the patch did not decide"; }
# [private] configs\y.ltx of one: two alone replaces y.ltx, no record; the folder "@alias" is not a side
grep -q 'configs\\\\y.ltx", "kind"' plan.json && { cat plan.json; fail "plan: a [private] file is a side"; }
grep -q '@alias' plan.json && { cat plan.json; fail "plan: a folder @... is a side"; }
printf '[s]\na = 99\nb = 20\nc = 3\n' > expected.ltx
cmp -s plan/out/configs/x.ltx expected.ltx || { cat plan/out/configs/x.ltx; fail "plan: unexpected merged text"; }
# The list of the launcher: three is off, pair:one = two puts one over two - the order decides for one ("pair")
printf '[addons]\nformat = 1\n[addon.two]\npath = plan/addons/two\n[addon.one]\npath = plan/addons/one\n' > addons.ltx
printf '[addon.three]\npath = plan/addons/three\nenabled = false\n[conflicts]\npair:one = two\n' >> addons.ltx
expect 0 plan --base plan/base --addons-dir plan/addons --list addons.ltx --game-root . --case windows --merge \
    -o plan2.json
grep -q '"winner": "one", "resolved_by": "pair"' plan2.json || { cat plan2.json; fail "plan: the pair rule"; }
grep -q '"kind": "pair", "winner": "one", "target": "two", "applied": true' plan2.json || \
    { cat plan2.json; fail "plan: the status of the pair rule"; }
# --all-files: a record of every text file the layers serve - one addon alone too ("extra": true, one_addon); the
# [private] y.ltx of one is not there; without --all-files the plan of one addon has no record
expect 0 plan --base plan/base --addon plan/addons/one -o plan_one.json
grep -q '"files": \[\]' plan_one.json || { cat plan_one.json; fail "plan: one addon, a record without --all-files"; }
grep -q '"all_files"' plan_one.json && { cat plan_one.json; fail "plan: all_files without --all-files"; }
expect 0 plan --base plan/base --addon plan/addons/one --all-files -o plan_all.json
grep -q '"all_files": true' plan_all.json || { cat plan_all.json; fail "plan: --all-files, no all_files"; }
grep -q '"file": "configs\\\\x.ltx", "kind": "ltx",' plan_all.json || { cat plan_all.json; fail "plan: --all-files, no x.ltx"; }
grep -q '"extra": true, "status": "skipped", "note": "one_addon"' plan_all.json || \
    { cat plan_all.json; fail "plan: --all-files, x.ltx is not one_addon"; }
grep -q '"file": "configs\\\\y.ltx"' plan_all.json && { cat plan_all.json; fail "plan: --all-files lists a [private] file"; }
# With two addons: x.ltx is the record of the merge (not extra), y.ltx (two alone serves it) is extra
expect 0 plan --base plan/base --addon plan/addons/one --addon plan/addons/two --all-files -o plan_all2.json
[ "$(grep -c '"extra": true' plan_all2.json)" -eq 1 ] || { cat plan_all2.json; fail "plan: --all-files, extra records"; }
grep -q '"file": "configs\\\\y.ltx", "kind": "ltx",' plan_all2.json || { cat plan_all2.json; fail "plan: --all-files, no y.ltx"; }
# A [private] file is not the target of a patch file (the index of the engine has it under @<id>\ only): mod_z_four.ltx
# of four and five is a file of two addons (a conflict), not a patch of z.ltx
mkdir -p plan/extra/four/gamedata/configs plan/extra/five/gamedata/configs
printf '[addon]\nid = four\nversion = 1.0.0\n[private]\nconfigs\\z.ltx\n' > plan/extra/four/addon.ltx
printf '[z]\nk = 1\n' > plan/extra/four/gamedata/configs/z.ltx
printf '[z]\nk = 2\n' > plan/extra/four/gamedata/configs/mod_z_four.ltx
printf '[addon]\nid = five\nversion = 1.0.0\n' > plan/extra/five/addon.ltx
printf '[z]\nk = 3\n' > plan/extra/five/gamedata/configs/mod_z_four.ltx
expect 0 plan --base plan/base --addon plan/extra/four --addon plan/extra/five -o plan_priv.json
grep -q '"note": "patch_file"' plan_priv.json && { cat plan_priv.json; fail "plan: a [private] file is a patch target"; }
grep -q '"file": "configs\\\\mod_z_four.ltx", "text": true' plan_priv.json || \
    { cat plan_priv.json; fail "plan: a file of two addons with a [private] target is not a conflict"; }
# No addon: usage
expect 2 plan --base plan/base
# Two bases, --base-texts without --base-snapshot: usage
expect 2 plan --base plan/base --base-list plan.json --addon plan/addons/one
expect 2 plan --base plan/base --base-texts plan.json --addon plan/addons/one

# 4b. plan --base-snapshot: the snapshot of the catalog (tools/addons_snapshot builder.py: base_snapshot.json.gz -
# the files with their sha256, the forms of the texts; base_texts.tar.gz - the normalized texts) of the same base gives
# the same plan and the same merged text as the folder
sha() { sha256sum "$1" | cut -d' ' -f1; }
size() { wc -c < "$1" | tr -d ' '; }
mkdir -p snap
(cd plan/base && tar -cf ../../snap/base_texts.tar configs/x.ltx configs/y.ltx)
gzip -n -9 snap/base_texts.tar
x=plan/base/configs/x.ltx
y=plan/base/configs/y.ltx
# The texts are ASCII with LF: the normalized text is the file itself (one sha256 for both)
printf '{"schema": "gw-base-snapshot", "schema_version": 1, "game": {"tag": "v1"}, "ltx": {"skipped": [1, {"a": "]"}]},
 "files": {"configs\\\\x.ltx": [%s, "%s"], "configs\\\\y.ltx": [%s, "%s"]},
 "texts": {"storage": {"mode": "tar", "file": "base_texts.tar.gz", "sha256": "%s"},
  "files": {"configs\\\\x.ltx": {"enc": "ascii", "bom": false, "eol": "lf", "sha256": "%s"},
            "configs\\\\y.ltx": {"enc": "ascii", "bom": false, "eol": "lf", "sha256": "%s"}}}}\n' \
    "$(size $x)" "$(sha $x)" "$(size $y)" "$(sha $y)" "$(sha snap/base_texts.tar.gz)" "$(sha $x)" "$(sha $y)" \
    | gzip -n -9 > snap/base_snapshot.json.gz
expect 0 plan --base-snapshot snap/base_snapshot.json.gz --addon plan/addons/one --addon plan/addons/two \
    --addon plan/addons/three --merge --texts plan/out_snap -o plan_snap.json
grep -q '"base_source": "snapshot"' plan_snap.json || { cat plan_snap.json; fail "plan: no base_source snapshot"; }
grep -q '"game_tag": "v1"' plan_snap.json || { cat plan_snap.json; fail "plan: the tag of the snapshot"; }
cmp -s plan/out_snap/configs/x.ltx expected.ltx || { cat plan/out_snap/configs/x.ltx; fail "plan: snapshot merged text"; }
# The same plan as the folder: only the members of the base differ
top() { sed -e '/^  "base/d' -e '/^  "game_tag"/d' -e '/^  "approximations"/d' "$1"; }
diff <(top plan.json) <(top plan_snap.json) || fail "plan: --base-snapshot gives another plan than --base"
# --base-texts named explicitly: the same; the tar of another snapshot is refused
expect 0 plan --base-snapshot snap/base_snapshot.json.gz --base-texts snap/base_texts.tar.gz \
    --addon plan/addons/one --addon plan/addons/two --addon plan/addons/three --merge -o plan_snap2.json
diff <(top plan_snap.json) <(top plan_snap2.json) || fail "plan: --base-texts gives another plan"
(cd plan/base && tar -cf ../../other.tar configs/x.ltx)
gzip -n -9 other.tar
expect 2 plan --base-snapshot snap/base_snapshot.json.gz --base-texts other.tar.gz --addon plan/addons/one \
    --addon plan/addons/two --merge
grep -q "not the texts of the snapshot" err.txt || { cat err.txt; fail "plan: a foreign tar is taken"; }
# No texts next to the snapshot: existence only - no_base_text, a warning
mkdir -p snap_alone
cp snap/base_snapshot.json.gz snap_alone/
expect 0 plan --base-snapshot snap_alone/base_snapshot.json.gz --addon plan/addons/one --addon plan/addons/two \
    --merge -o plan_alone.json
grep -q '"note": "no_base_text"' plan_alone.json || { cat plan_alone.json; fail "plan: no texts, no no_base_text"; }
grep -q 'no texts of the base' plan_alone.json || { cat plan_alone.json; fail "plan: no texts, no warning"; }
# A broken snapshot: an error, code 2
printf 'not gzip' > broken.json.gz
expect 2 plan --base-snapshot broken.json.gz --addon plan/addons/one
head -c 40 snap/base_snapshot.json.gz > cut.json.gz
expect 2 plan --base-snapshot cut.json.gz --addon plan/addons/one

# 4c. plan --base-list (appdata\logs\addons_base_files.txt of the engine: a player's base lies in the archives):
# existence only - the sides, the patches, the winners as the folder plans them, the merge not (no_base_text)
printf '# gw_base_files 1\n# game_tag v2\nconfigs\\x.ltx\nconfigs/Y.ltx\t%s\n../outside.ltx\n' "$(sha $y)" > base_files.txt
expect 0 plan --base-list base_files.txt --case windows --addon plan/addons/one --addon plan/addons/two \
    --addon plan/addons/three --merge -o plan_list.json
grep -q '"base_source": "list"' plan_list.json || { cat plan_list.json; fail "plan: no base_source list"; }
grep -q '"game_tag": "v2"' plan_list.json || { cat plan_list.json; fail "plan: the tag of the list"; }
grep -q '"note": "no_base_text"' plan_list.json || { cat plan_list.json; fail "plan: list, no no_base_text"; }
grep -q '"merge": true' plan_list.json && { cat plan_list.json; fail "plan: list, a file merged without a base text"; }
grep -q '"id": "three", "targets": \["one", "two"\], "status": "applied"' plan_list.json || \
    { cat plan_list.json; fail "plan: list, the patch is not chosen"; }
grep -q '"winner": "addons_merged", "resolved_by": "merge"' plan_list.json || \
    { cat plan_list.json; fail "plan: list, the conflict is not the merge of the engine"; }
grep -q '1 line(s) are not a file name' plan_list.json || { cat plan_list.json; fail "plan: list, a bad line taken"; }
printf '# gw_base_files 9\nconfigs\\x.ltx\n' > base_files9.txt
expect 2 plan --base-list base_files9.txt --addon plan/addons/one

# 4d. diff3 (decision 64 item 6): a change that contains the touching change of the other side (an insertion at its
# edge of the lines it puts there) - the larger one once; the touching changes repeat a line otherwise - a conflict
printf 'A\nB\nC\n' > l_base.txt
printf 'A\nX\nB\nC\n' > l_a.txt
printf 'A\nX\nY\nC\n' > l_b.txt
printf 'X\nB\nC\n' > l_c.txt
expect 0 lines l_base.txt l_a.txt l_b.txt
[ "$(grep -c '^X$' out.txt)" -eq 1 ] && grep -q '^Y$' out.txt || { cat out.txt; fail "lines: a contained change twice"; }
expect 1 lines l_base.txt l_c.txt l_b.txt
# One line of the larger change folds one insertion only: X before and after B (a) against B -> X (b) is not X once
printf 'A\nX\nB\nX\nC\n' > l_d.txt
printf 'A\nX\nC\n' > l_e.txt
expect 1 lines l_base.txt l_d.txt l_e.txt
expect 1 lines l_base.txt l_e.txt l_d.txt

# 4e. delta + apply (decision 64 item 5): a gap of an ltx (the lines before the first section) the base and the addon
# both changed - diff3, no conflict; the code between the atoms of a Lua script both changed - a conflict, the base
# keeps its code
mkdir -p d_anc/configs d_anc/scripts d_add/gamedata/configs d_add/gamedata/scripts
printf '[addon]\nid = d_add\nversion = 1.0.0\ntype = content\n' > d_add/addon.ltx
printf 'x = 1\ny = 2\nz = 3\n[s]\nk = 1\n' > d_anc/configs/p.ltx
printf 'x = 10\ny = 2\nz = 3\n[s]\nk = 1\n' > d_add/gamedata/configs/p.ltx
printf 'x = 1\ny = 2\nz = 30\n[s]\nk = 1\n' > d_base.ltx
printf 'function a() end\nfoo()\nbar()\nfunction c() end\n' > d_anc/scripts/p.script
printf 'function a() end\nfoo(1)\nbar()\nfunction c() end\n' > d_add/gamedata/scripts/p.script
printf 'function a() end\nfoo()\nbar(2)\nfunction c() end\n' > d_base.script
expect 0 delta --ancestor-dir d_anc --addon d_add --game v1 -o d_delta.json
expect 0 apply ltx --delta d_delta.json --vpath configs/p.ltx d_base.ltx -o d_out.ltx --report d_ltx.json
printf 'x = 10\ny = 2\nz = 30\n[s]\nk = 1\n' > expected.ltx
cmp -s d_out.ltx expected.ltx || { cat d_out.ltx d_ltx.json; fail "apply: an ltx gap both changed is not merged"; }
expect 1 apply script --delta d_delta.json --vpath scripts/p.script d_base.script -o d_out.script --report d_lua.json
cmp -s d_out.script d_base.script || { cat d_out.script; fail "apply: the base does not keep its top-level code"; }
grep -q '"winner": "core"' d_lua.json || { cat d_lua.json; fail "apply: the top-level conflict is not won by the base"; }

# 5. LuaJIT for --check-lua: the library loads and has the Lua C API (code 2 otherwise)
if [ -n "$LUA_LIB" ]; then
    expect 0 check script ok.script "--check-lua=$LUA_LIB"
    expect 1 check script bad.script "--check-lua=$LUA_LIB"
    # Packed next to gw_merge (the archive of the CI): found without a path too (RUNPATH $ORIGIN on Linux)
    case "$(basename "$LUA_LIB")" in
        libluajit-5.1.so.2|LuaJIT.dll)
            if [ "$(dirname "$LUA_LIB")" = "$(dirname "$GW_MERGE")" ]; then
                expect 0 check script ok.script --check-lua
                expect 1 check script bad.script --check-lua
            fi
            ;;
    esac
fi

echo "gw_merge smoke test: ok"
