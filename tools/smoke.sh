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
    [array_infer_len]=8
    [slice_runtime_len]=6
    [array_infer_multi_var]=6
    [113_global_var]=14
    [114_global_namespace]=6
    [115_global_ptr]=16
    [117_malloc_class_size]=4
    [118_ref_field_copy_alias]=17
    [119_global_init_order]=10
    [120_malloc_array_field_copy]=100
    [121_self_conversion]=4
    [class_arg_pack]=1
    [ptr_int_cast]=20
    [ptr_to_array]=52
    [148_malloc_var_assign]=5
    [varargs_float]=15
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

# extern 库名 + -L：非系统目录下的动态库须能链接，且 -r 运行无需
# LD_LIBRARY_PATH（验证 -Wl,-rpath 生效）；反之去掉 -L 必须链接失败
lib_ok=0
libdir="$tmp/lib"
mkdir -p "$libdir"

cat > "$tmp/cluxtestlib.cpp" <<'EOF'
extern "C" int clux_test_value() { return 33; }
EOF

cat > "$tmp/use_lib.cx" <<'EOF'
extern "cluxtest" { clux_test_value() int r { } }
main() int r {
    r = clux_test_value()
}
EOF

lib_n=1
if ! clang++ -shared -fPIC -o "$libdir/libcluxtest.so" "$tmp/cluxtestlib.cpp" >/dev/null 2>&1; then
    bad_list+=("生成 libcluxtest.so 失败")
else
    "$CLUX" -r "$tmp/use_lib.cx" -L "$libdir" -o "$tmp/use_lib" >/dev/null 2>&1
    rc=$?
    if [ "$rc" -ne 33 ]; then
        bad_list+=("-L 构建运行退出码 $rc，期望 33")
    else
        lib_ok=$((lib_ok+1))
    fi

    lib_n=$((lib_n+1))
    if "$CLUX" -b "$tmp/use_lib.cx" -o "$tmp/use_lib_nol" >/dev/null 2>&1; then
        bad_list+=("未给 -L 时本应链接失败却成功")
    else
        lib_ok=$((lib_ok+1))
    fi
fi

# extern 数组全局：定长数组的布局须与 C 一致，能与 C 侧共享同一段存储
arr_ok=0
arr_n=0

cat > "$tmp/cbuf.c" <<'EOF'
char clux_buf[256];
void clux_fill(void) { clux_buf[0] = 'X'; clux_buf[1] = 0; }
EOF

cat > "$tmp/use_cbuf.cx" <<'EOF'
extern "c" {
    char[256] clux_buf
    clux_fill() int r { }
    puts(char-* s) { }
}
main() int r {
    clux_fill()
    clux_buf[0] = 'H'
    clux_buf[1] = 10
    puts(clux_buf)
    r = 0
}
EOF

arr_n=$((arr_n + 1))
if ! clang++ -c -x c "$tmp/cbuf.c" -o "$tmp/cbuf.o" >/dev/null 2>&1; then
    bad_list+=("生成 cbuf.o 失败")
elif ! "$CLUX" --emit-obj "$tmp/use_cbuf.cx" -o "$tmp/use_cbuf.o" >/dev/null 2>&1; then
    bad_list+=("extern 数组用例翻译失败")
elif ! clang++ "$tmp/use_cbuf.o" "$tmp/cbuf.o" -o "$tmp/use_cbuf" >/dev/null 2>&1; then
    bad_list+=("extern 数组用例链接失败")
else
    out=$("$tmp/use_cbuf")
    if [ "$out" = "H" ]; then
        arr_ok=$((arr_ok + 1))
    else
        bad_list+=("extern 数组用例输出 '$out'，期望 'H'")
    fi
fi

end_ns=$(date +%s%N)
elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
sec=$((elapsed_ms / 1000))
ms=$((elapsed_ms % 1000))
if   [ "$ms" -lt 10 ];  then ms="00$ms"
elif [ "$ms" -lt 100 ]; then ms="0$ms"
fi

echo "smoke: $ok/$n"
echo "lib: $lib_ok/$lib_n"
echo "extern数组: $arr_ok/$arr_n"
echo "耗时: ${sec}.${ms}s"
if [ ${#bad_list[@]} -gt 0 ]; then
    printf '\n%s\n' "${bad_list[@]}"
    exit 1
fi
echo "全部符合预期"
