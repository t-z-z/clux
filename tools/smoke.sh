#!/usr/bin/env bash
# clux 端到端冒烟：pass/ 全部用例构建并运行，校验退出码
# 判据：构建成功 且 未被信号杀死 且 退出码等于期望值
#
# 期望退出码默认 0，非零用例在下方 EXPECT 表中登记。
set -u
cd "$(dirname "$0")/.." || exit 2

CLUX=./bin/clux
[ -x "$CLUX" ] || { echo "缺少 $CLUX，请先构建"; exit 2; }

# 用例名（不含 .cx）→ 期望退出码
declare -A EXPECT=(
    [78_array_decl_len]=10
    [class_arg_pack]=1
    [ptr_int_cast]=20
    [ptr_to_slice]=52
)

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

n=0; ok=0; bad=0
bad_list=()

start_ns=$(date +%s%N)

for f in tools/cases/pass/*.cx; do
    [ -e "$f" ] || continue
    n=$((n+1))
    b=$(basename "$f" .cx)
    want=${EXPECT[$b]:-0}

    if ! "$CLUX" -b "$f" -o "$tmp/$b" >/dev/null 2>&1; then
        bad=$((bad+1)); bad_list+=("构建失败: $b")
        continue
    fi

    "$tmp/$b" >/dev/null 2>&1
    rc=$?

    if [ $rc -ge 128 ]; then
        bad=$((bad+1)); bad_list+=("被信号杀死(exit=$rc): $b")
    elif [ $rc -ne $want ]; then
        bad=$((bad+1)); bad_list+=("退出码 $rc，期望 $want: $b")
    else
        ok=$((ok+1))
    fi
done

end_ns=$(date +%s%N)
elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
sec=$((elapsed_ms / 1000))
ms=$((elapsed_ms % 1000))
if   [ "$ms" -lt 10 ];  then ms="00$ms"
elif [ "$ms" -lt 100 ]; then ms="0$ms"
fi

echo "smoke: $ok/$n"
echo "耗时: ${sec}.${ms}s"
if [ ${#bad_list[@]} -gt 0 ]; then
    printf '\n%s\n' "${bad_list[@]}"
    exit 1
fi
echo "全部符合预期"
