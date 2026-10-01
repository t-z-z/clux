#!/usr/bin/env bash
# clux 前端测试：pass/ 期望通过，fail/ 期望报错
set -u
cd "$(dirname "$0")/.." || exit 2

CLUX=./bin/clux
[ -x "$CLUX" ] || { echo "缺少 $CLUX，请先构建"; exit 2; }

pass_n=0; pass_bad=0; fail_n=0; fail_bad=0
bad_list=()

start_ns=$(date +%s%N)

for f in tools/cases/pass/*.cx; do
    [ -e "$f" ] || continue
    pass_n=$((pass_n+1))
    if ! "$CLUX" -c "$f" >/dev/null 2>&1; then
        pass_bad=$((pass_bad+1)); bad_list+=("PASS 期望通过但失败: $f")
    fi
done

for f in tools/cases/fail/*.cx; do
    [ -e "$f" ] || continue
    fail_n=$((fail_n+1))
    if "$CLUX" -c "$f" >/dev/null 2>&1; then
        fail_bad=$((fail_bad+1)); bad_list+=("FAIL 期望报错但通过: $f")
    fi
done

end_ns=$(date +%s%N)
elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
sec=$((elapsed_ms / 1000))
ms=$((elapsed_ms % 1000))
if   [ "$ms" -lt 10 ];  then ms="00$ms"
elif [ "$ms" -lt 100 ]; then ms="0$ms"
fi

echo "pass: $((pass_n-pass_bad))/$pass_n   fail: $((fail_n-fail_bad))/$fail_n"
echo "耗时: ${sec}.${ms}s"
if [ ${#bad_list[@]} -gt 0 ]; then
    printf '\n%s\n' "${bad_list[@]}"
    exit 1
fi
echo "全部符合预期"
