#!/usr/bin/env bash
# clux IR 对拍：对 pass/ 全部用例生成 IR 并与基线 diff
# 首次生成基线： bash tools/ir_diff.sh --init
set -u
cd "$(dirname "$0")/.." || exit 2

CLUX=./bin/clux
BASE=tools/ir_base
[ -x "$CLUX" ] || { echo "缺少 $CLUX，请先构建"; exit 2; }

if [ "${1:-}" = "--init" ]; then
    mkdir -p "$BASE"
    n=0
    for f in tools/cases/pass/*.cx; do
        [ -e "$f" ] || continue
        b=$(basename "$f" .cx)
        "$CLUX" -t "$f" -o "$BASE/$b.ll" >/dev/null 2>&1 || { echo "IR 生成失败: $f"; exit 1; }
        n=$((n+1))
    done
    # 用例删除后其基线会残留，导致基线数与用例数不一致
    for g in "$BASE"/*.ll; do
        [ -e "$g" ] || continue
        b=$(basename "$g" .ll)
        [ -e "tools/cases/pass/$b.cx" ] || { rm -f "$g"; echo "已清理残留基线: $b"; }
    done
    echo "已生成基线 $n 个"
    exit 0
fi

diff_n=0; miss=0; bad=0
for f in tools/cases/pass/*.cx; do
    [ -e "$f" ] || continue
    b=$(basename "$f" .cx)
    exp="$BASE/$b.ll"
    if [ ! -f "$exp" ]; then miss=$((miss+1)); continue; fi
    tmp=$(mktemp)
    if ! "$CLUX" -t "$f" -o "$tmp" >/dev/null 2>&1; then
        bad=$((bad+1)); echo "IR 生成失败: $f"; rm -f "$tmp"; continue
    fi
    if ! diff -q "$exp" "$tmp" >/dev/null; then
        diff_n=$((diff_n+1)); echo "IR 不一致: $f"
        diff "$exp" "$tmp" | head -20
    fi
    rm -f "$tmp"
done
echo "对拍完成: 不一致 $diff_n, 生成失败 $bad, 缺基线 $miss"
[ $diff_n -eq 0 ] && [ $bad -eq 0 ] && [ $miss -eq 0 ] && echo "全部一致" && exit 0
exit 1
