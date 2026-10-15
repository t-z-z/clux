# clux 编译器架构

本文档说明 clux 编译器的编译流程、模块划分、项目结构、测试体系与代码规范。

面向语言使用者与标准库的文档见 [`spec.md`](spec.md)、[`lib.md`](lib.md)、[`future.md`](future.md)。

## 编译流程

```text
.cx → LLVM IR (.ll) → 可执行文件
```

前端负责 `.cx` 到 LLVM IR 文本的生成，最终由 `clang++` 完成汇编、链接，产出可执行文件、汇编或目标文件。

## 模块划分

| 模块 | 文件 | 职责 |
| --- | --- | --- |
| 词法分析 | [`src/lexer.cpp`](src/lexer.cpp), [`include/lexer.h`](include/lexer.h), [`include/token.h`](include/token.h) | Token 定义、词法扫描、错误报告 |
| 语法分析 | [`src/parser.cpp`](src/parser.cpp), [`include/parser.h`](include/parser.h) | 递归下降解析、AST 构建 |
| AST 与克隆 | [`include/ast.h`](include/ast.h), [`src/clone.cpp`](src/clone.cpp) | AST 节点定义、`CompUnit` 节点池、深拷贝 |
| 类型工具 | [`include/sem_types.h`](include/sem_types.h), [`src/sem_types.cpp`](src/sem_types.cpp) | 类型谓词、类型名、字面量范围等无状态工具 |
| 语义分析 | [`src/semantic.cpp`](src/semantic.cpp), [`include/semantic.h`](include/semantic.h) | 符号收集、作用域管理、表达式类型推导 |
| 别名展开 | [`src/semantic_def.cpp`](src/semantic_def.cpp), [`include/semantic_def.h`](include/semantic_def.h) | `typedef` / `exprdef` 使用点展开成目标节点的克隆件 |
| 常数求值 | [`src/const_eval.cpp`](src/const_eval.cpp), [`include/const_eval.h`](include/const_eval.h) | 编译期常量表达式求值 |
| 修饰注入 | [`src/inject.cpp`](src/inject.cpp), [`include/inject.h`](include/inject.h) | 类 `#修饰` 展开与成员合并 |
| 类型转换 | [`src/convert.cpp`](src/convert.cpp) | 隐式/显式转换判定与检查 |
| 调用解析 | [`src/call_resolve.cpp`](src/call_resolve.cpp) | 调用路径解析、实参匹配 |
| 流检查 | [`src/flow_check.cpp`](src/flow_check.cpp), [`include/flow_check.h`](include/flow_check.h) | 跳转 label、变量初始化、返回变量赋值 |
| 诊断系统 | [`src/diagnostic.cpp`](src/diagnostic.cpp), [`include/diagnostic.h`](include/diagnostic.h) | 错误/警告/信息的统一格式输出 |
| 前端入口 | [`src/compiler.cpp`](src/compiler.cpp), [`include/compiler.h`](include/compiler.h) | `check` / `translate` 接口、import 解析与路由 |
| IR 生成 | [`src/translator.cpp`](src/translator.cpp), [`src/ir_writer.cpp`](src/ir_writer.cpp), [`include/translator.h`](include/translator.h), [`include/ir_writer.h`](include/ir_writer.h) | AST → LLVM IR 文本 |
| 符号名生成 | [`src/translator_mangle.cpp`](src/translator_mangle.cpp), [`include/translator_mangle.h`](include/translator_mangle.h) | mangled name、类型键 |
| 命令行驱动 | [`src/main.cpp`](src/main.cpp) | 参数解析、调用前端、经 `clang++` 产出可执行/汇编/目标文件 |

辅助头文件：

- [`include/string_pool.h`](include/string_pool.h)：分块字符串池
- [`include/string_view_map.h`](include/string_view_map.h)：透明查找 map

语义分析的多文件划分共用 `SemanticAnalyzer` 类，方法定义分散在各自的 `.cpp`。AST 语义字段（`type` / `sym` / `conv` 等）的写入与读取契约见 `include/ast.h` 顶部注释。

## 项目结构

```text
include/         编译器头文件
src/             词法、语法、语义、IR 生成、驱动等实现
lib/             标准库源文件（.cx）
tools/cases/     测试用例，pass/ 期望通过，fail/ 期望报错
tools/ir_base/   IR 对拍基线（本地生成，不随仓库分发）
spec.md          语言完整规范
lib.md           标准库 API 文档
future.md        未来特性规划
```

标准库以 `.cx` 源码形式提供，`lib/` 下每个文件对应一个 C 头文件的 `extern "C"` 绑定：
`cstdio`（stdio.h）、`cstdlib`（stdlib.h）、`cstring`（string.h）、`cmath`（math.h）、
`cctype`（ctype.h）、`ctime`（time.h）、`cinttypes`（inttypes.h）、`csignal`（signal.h）、
`clocale`（locale.h）、`cerrno`（errno.h）。`import` 解析与路由由前端入口负责，默认自动导入 `std`。

## 当前实现要点

以下是理解当前实现时必须知道的约定，其中部分与语言规范中的最终目标存在差距。

1. 无分号、无逗号：语句和元素自动分隔，`;` 和 `,` 仅用于强制分隔。
2. 直接返回变量：函数末尾表达式隐式返回。
3. 修饰而非继承的 OOP：使用 `#修饰符 类名` 注入行为。类声明以类名加 `{` 起始，`@` 已非必需。
4. 数组与 C 定长数组一致：`T[N]`（`T[]` 由初值推导长度）在 IR 里就是 `[N x T]`，存储内联、整体拷贝、可按值传参与返回，`.len` 是编译期常量，数组名退化成指针。
5. `T{}` / `T{n}` 是拥有自己存储的切片，只出现在局部变量，不能作形参、返回类型或类字段，跨函数传递一律用指针。长度是编译期常量时存储放函数入口块、整个函数一次；长度在运行期时仍是栈上 `alloca`，用 `llvm.stacksave` / `llvm.stackrestore` 在作用域结束归还栈（`alloca` 本身只在函数返回时释放，循环里会逐轮吃栈）。`malloc` 修饰则走堆分配、不自动回收。
6. 枚举底层是 `i32`：成员值在语义遍历时按声明顺序定值，显式初值必须是编译期整数常量且只能引用已定值的前序成员（`E::A` 限定名），缺省初值沿用前一个值加一；取值超出 `int` 范围报错而不是截断。
7. 三种别名机制各管一段：`define` 是词法阶段的 token 替换，跟随 `{}` 作用域、以 `:` 收尾、不跨模块；`typedef` / `exprdef` 是语义阶段展开，别名在 `collect` 阶段登记进符号表（因而可跨模块），使用点则在语义主遍历走到它时才展开：节点内容换成目标节点的克隆件，再按新形态遍历一遍，链式别名随之逐层展开。因此别名和局部变量同序——文件作用域的别名对其后的代码生效，函数体内的别名只对该块内其后的代码生效，声明之前用不到。代价是别名须先声明后使用（与 C 的 typedef 一致）：多模块按逆 BFS 序分析，被导入方先展开完，导入方的调用点才能比对到已展开的签名。
8. extern 函数可以按值返回聚合（`div` / `ldiv` / `imaxdiv` 一类）：声明与调用都写真实结构体类型，寄存器怎么收发由 LLVM 后端按目标平台决定；返回值拷进调用方的存储后，再按类值交出去。类在 clux 内部的值模型始终是存储指针，只有跨 ABI 边界才按结构体收发，clux 自身函数的类返回仍走 sret。
9. future.md中内容均未实现

## 构建

依赖支持 C++20 的 `clang++` 与 CMake。

```bash
cmake -B build -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

产物位于 `bin/clux`。

常用动作：

```bash
bin/clux -c main.cx    # 前端检查
bin/clux -t main.cx    # 生成 IR
bin/clux -b main.cx    # 构建
bin/clux -r main.cx    # 运行
```

## 测试

### 前端检查

```bash
bash tools/run_tests.sh
```

用例位于 `tools/cases/`：`pass/` 期望前端通过，`fail/` 期望报错。

### IR 对拍

用于防止语义重构意外改动生成结果。

IR 基线不随仓库分发，拉取后需先在本地重建一次：

```bash
bash tools/ir_diff.sh          # 对 pass/ 全部用例生成 IR 并与基线 diff
bash tools/ir_diff.sh --init   # 重建基线（仅当 IR 变更是有意为之）
```

基线存于 `tools/ir_base/`。

### 冒烟测试

```bash
bash tools/smoke.sh
```

用时较长，不适合高频使用。

## 代码规范

- 局部应该向上级负责自身的正确性与安全性，只能向下依赖不能向上依赖。
- 使用 C++20 标准。
- 命名空间统一为 `cx::`。
- `TokenKind` 使用宏定义统一管理，见 `token.h`。
- 错误处理：词法/语法/语义错误分别收集，最后统一报告。
- 知道明确用途才添加新函数。
- 多尝试复用代码。
- 注释只写必要说明，不写装饰性分隔线。
- 设计决策以项目维护者的判断为准。

## 贡献流程

- 新增语言行为时，同时添加 `tools/cases/pass/` 或 `tools/cases/fail/` 用例。
- 修改规范或标准库文档前，请先讨论。
