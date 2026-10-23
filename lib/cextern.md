# cextern

C 标准库绑定，对应 C99 的 24 个头文件。为 clux 提供 C 函数的声明与 LLVM IR 实现。

目标平台：**x86_64 Linux (glibc)**。不做多平台，不依赖 clux ABI，不做宽字符。

## 怎么用

直接 import 对应的头文件：

```cx
import cstdio
import ctime

main() int r {
    printf("hello\n")
    time_t t = time(null)
    tm-* lt = localtime(t-&)
    printf("year: %d\n" tm_year(lt) + 1900)
    r = 0
}
```

每个 `.cx` 文件对应一个 C 标准头文件，`cstdio.cx` 对应 `<stdio.h>`，`ctime.cx` 对应 `<time.h>`，以此类推。

### C 结构体

C 结构体（`tm`、`lconv`、`jmp_buf`、`fenv_t`）在 clux 侧是**不透明类型**，不能直接访问字段，而是通过**字段 getter** 读取：

```cx
tm-* lt = localtime(t-&)
int year = tm_year(lt) + 1900
int mon  = tm_mon(lt) + 1
```

getter 的名字规则是 `结构体名_字段名`。`struct tm` 的 `tm_year` 字段对应 `tm_year(lt)`。

### 引用参数

需要传入结构体让函数填写的接口，参数类型是**引用**（`T-&`），调用时直接传量名：

```cx
time_t t = 0
time(t)                    // t 是 lvalue，自动绑定引用
```

```cx
_imaxdiv_t q = _imaxdiv(10, 3)
long quot = _imaxdiv_quot(q)   // 正确
long quot = _imaxdiv_quot(q-&) // 错误：q-& 是 _imaxdiv_t-*，不是 _imaxdiv_t
```

引用参数要求实参是 **lvalue**（有名字的量、解引用结果），不能是字面量、临时值、`-&` 取地址的结果。

### 按值声明的结构体

`jmp_buf`、`fenv_t` 这类需要在栈上分配的结构体，用默认构造初始化：

```cx
jmp_buf env = jmp_buf()
fenv_t fenv = fenv_t()
```

不初始化会报 `use of uninitialized value`。

### `setjmp` 的特殊情况

C 的 `setjmp` 是宏，不是函数。clux 无函数式宏，所以 `csetjmp.cx` 直接暴露底层真实符号 `_setjmp`：

```cx
jmp_buf env = jmp_buf()
int code = _setjmp(env)
if code == 0 {
    // 首次执行
    longjmp(env 42)
} else {
    // 从 longjmp 跳回
}
```

`_setjmp` 对应 C 的 `setjmp`（不保存信号掩码）。需要保存信号掩码时用 `__sigsetjmp(env, savemask)`，对应 C 的 `sigsetjmp`。

## 为什么这么写

### 为什么结构体用 LLVM IR 表达，不用 clux 类

C 结构体的内存布局由 C ABI 决定，而 clux 类的布局由 clux 决定。两者不保证一致。如果 clux 侧用类来表达 `struct tm`，一旦 clux 的类布局规则（对齐、填充、字段顺序）与 C 不同，`localtime` 返回的指针在 clux 侧读出来的字段就是错的。

用 `extern "LLVMIR"` 声明空壳 + `keep` 里手写实际布局，把布局控制权完全交给 IR，clux 侧不参与，就不会有 ABI 耦合。

### 为什么用字段 getter，不用字段访问

既然结构体在 clux 侧是不透明类型，就不能写 `lt.tm_year`。字段 getter 是唯一能读到字段的方式。每个 getter 在 IR 里做一次 `getelementptr` + `load`，语义清晰，且 clux 侧不需要知道字段偏移。

`tm` 有 9 个字段就写 9 个 getter，`lconv` 有 24 个字段就写 24 个 getter。繁琐但直接。

### 为什么引用参数传 lvalue，不传指针

clux 的引用参数（`T-&`）在 IR 侧就是 `ptr`，和 C 的指针语义一致。区别在 clux 侧的写法：引用参数直接传量名，指针参数要写 `-&`。这让调用更接近 C 的写法——C 里 `time(&t)` 传地址，clux 里 `time(t)` 传 lvalue，编译器自动取地址。

这条规则也意味着引用参数不能绑临时值。`time(time(null))` 不合法，因为内层 `time(null)` 的返回值不是 lvalue。这其实和 C 一致——C 里也不能写 `time(&time(NULL))`。

### 为什么常量用 exprdef

C 的 `#define INT_MAX 2147483647` 是预处理宏。clux 的 `define` 不跨模块，所以库里不能用。`exprdef` 是表达式别名，跨模块可见，正好对应 C 的常量宏：

```cx
exprdef INT_MAX = 2147483647
```

类型别名（`typedef time_t = long`）用 `typedef`，也跨模块。

### 为什么 `LDBL_*` 不提供

clux 没有 `long double` 类型。`<float.h>` 里的 `LDBL_MAX`、`LDBL_EPSILON` 等即使定义了，clux 侧也没有类型能承接。强行定义只会误导使用者，不如不提供。

### 为什么放弃某些头文件

放弃的头文件都卡在同一个根因上——**clux 没有跨模块的函数式宏**：

- `<cassert.h>` 的 `assert` 展开为 `((expr) ? (void)0 : __assert_fail(...))`，需要参数替换和调用点信息
- `<cstdarg.h>` 的 `va_start`/`va_arg`/`va_end` 是编译期原语，访问调用函数的栈帧
- `<ctgmath.h>` 的泛型宏靠 `_Generic` 做类型分派

这三个不是"不想做"，是"当前机制做不到"。

另外三个是别的原因：

- `<ccomplex.h>` 的 `complex` 是 C 语言级类型，clux 无对应类型，ABI 对不上
- `<ciso646.h>` 是操作符别名（`and` = `&&`），原本打算用 `operdef` 实现，后因编译开销取消
- `<cwchar.h>` / `<cwctype.h>` 明确不做宽字符

`<cstdbool.h>` 不需要，clux 内置 `bool`。

## 头文件清单

### 已实现

| 头文件 | 文件 |
| --- | --- |
| `<cctype.h>` | `cctype.cx` |
| `<cerrno.h>` | `cerrno.cx` |
| `<cfenv.h>` | `cfenv.cx` |
| `<cfloat.h>` | `cfloat.cx` |
| `<cinttypes.h>` | `cinttypes.cx` |
| `<climits.h>` | `climits.cx` |
| `<clocale.h>` | `clocale.cx` |
| `<cmath.h>` | `cmath.cx` |
| `<csetjmp.h>` | `csetjmp.cx` |
| `<csignal.h>` | `csignal.cx` |
| `<cstddef.h>` | `cstddef.cx` |
| `<cstdint.h>` | `cstdint.cx` |
| `<cstdio.h>` | `cstdio.cx` |
| `<cstdlib.h>` | `cstdlib.cx` |
| `<cstring.h>` | `cstring.cx` |
| `<ctime.h>` | `ctime.cx` |

### 放弃

| 头文件 | 原因 |
| --- | --- |
| `<cassert.h>` | `assert` 是函数式宏 |
| `<ccomplex.h>` | clux 无原生复数类型 |
| `<ciso646.h>` | 操作符别名，实际无用 |
| `<cstdarg.h>` | `va_arg` 是编译期原语 |
| `<ctgmath.h>` | 泛型宏，无 `_Generic` |
| `<cwchar.h>` | 不做宽字符 |
| `<cwctype.h>` | 不做宽字符 |

### 不需要

| 头文件 | 原因 |
| --- | --- |
| `<cstdbool.h>` | clux 内置 `bool` |

## 已知限制

- **`FE_DFL_ENV`**：glibc 中是特殊指针值 `(fenv_t*)-1`，clux 的 `exprdef` 只能产生标量，无法产生 `fenv_t-&` 类型，使用时需绕过。
- **`FLT_ROUNDS`**：C 中反映当前舍入模式，是动态的。clux 侧作为 `exprdef` 只能给编译期常量。
- **`setjmp` 名字**：C 里叫 `setjmp`，clux 里叫 `_setjmp`。这是宏无法跨模块的直接后果。
