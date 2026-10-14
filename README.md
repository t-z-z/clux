<div align="center">

<h1>clux</h1>

<p><strong>一种编译到 LLVM IR 的实验性编程语言</strong></p>

<p>
  <img src="https://img.shields.io/badge/Language-C%2B%2B20-blue" alt="C++20">
  <img src="https://img.shields.io/badge/LLVM-IR-orange" alt="LLVM IR">
  <img src="https://img.shields.io/badge/License-Apache--2.0-green" alt="Apache-2.0">
  <img src="https://img.shields.io/badge/Status-early-red" alt="Early">
</p>

</div>

clux（简称 `cx`）是一种正在开发中的编程语言。源文件后缀为 `.cx`，编译流程为：

```text
.cx → LLVM IR (.ll) → 可执行文件
```

编译器使用 C++20 实现，当前主要在 Fedora 43 上开发。

## 当前状态

clux 仍处于早期开发阶段，语言语法、编译器实现、IR 输出和标准库 API 都可能发生较大变化。

- 编译器前端与 LLVM IR 生成已有实现。
- 标准库尚未完成：`cxio`、`metatype` 已有文档，`concurrent`、`containers` 仍为规划中。
- 泛型的函数与类已实现（泛型注入除外）；闭包、编译期反射、组合声明、`let` 推导等特性尚未实现，详见 [`future.md`](future.md)。
- 不建议用于生产环境。

## 设计特色

- 不需要逗号、分号或换行来分隔语句与元素。
- 表达式与类型共享同一套语法结构。
- 使用“修饰注入”实现 OOP，而不是传统继承。
- 函数以变量作为返回值，函数末尾隐式返回返回变量。
- 内存管理采用 RAII。
- 编译到 LLVM IR，再经由 `clang++` 产出可执行文件、汇编或目标文件。

## 快速开始

### 依赖

需要支持 C++20 的 `clang++`、CMake，以及用于最终链接的 `clang++`。

在 Fedora 上可参考：

```bash
sudo dnf install clang cmake
```

### 构建

```bash
cmake -B build -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

构建产物位于：

```text
bin/clux
```

### 第一个程序

创建 `main.cx`：

```cx
add(int a, int b) int r {
    r = a + b
}

main() int r {
    r = add(1, 2)
}
```

检查、翻译、构建并运行：

```bash
bin/clux -c main.cx    # 前端检查
bin/clux -t main.cx    # 生成 LLVM IR
bin/clux -b main.cx    # 构建可执行文件
bin/clux -r main.cx    # 构建并运行
```

### 命令行选项

```text
clux -c main.cx    # 检查
clux -t main.cx    # 翻译为 LLVM IR
clux -b main.cx    # 构建
clux -r main.cx    # 运行
clux -o file       # 指定输出位置
```

## 语言示例

### 函数返回变量

```cx
add(int a, int b) int r = a + b
{}

main() int r = add(1, 2)
{}
```

### 类

```cx
@Counter {
    int value = 0

    increment() {
        value = value + 1
    }
}
```

### 命名空间

```cx
namespace math {
    add(int a, int b) int r {
        r = a + b
    }
}

math::add(1, 2)
```

更多语法与语义说明见 [`spec.md`](spec.md)。

## 文档

- [`spec.md`](spec.md)：语言完整规范
- [`lib.md`](lib.md)：标准库 API 文档
- [`future.md`](future.md)：未来特性规划
- [`architecture.md`](architecture.md)：编译器架构、模块划分、测试与代码规范

## 路线图

标准库：

- 完善 `cxio`、`metatype`
- 实现 `concurrent`
- 实现 `containers`

语言特性：

- 泛型
- 闭包与代码块
- 编译期反射
- 组合声明
- `let` 类型推导

稳定性：

- 稳定语法与语义
- 稳定 IR 输出
- 完善标准库 API

## 贡献

欢迎提交 Issue 和 PR。参与开发前请先阅读 [`architecture.md`](architecture.md)，其中包含模块划分、测试体系与代码规范。

建议：

- 保持 C++20 标准，命名空间使用 `cx::`。
- 新增语言行为时，同时添加 `tools/cases/pass/` 或 `tools/cases/fail/` 用例。
- 若有意修改 IR 输出，请同步重建本地 IR 基线，并在 PR 中说明原因。
- 修改规范或标准库文档前，请先讨论。

## 许可证

本项目采用 Apache License, Version 2.0 授权。
完整文本见 [LICENSE](LICENSE)。
