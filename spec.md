# clux 语言规范

## 一、基础信息

语言名称：clux / cx
源文件后缀：`.cx`
内存管理：RAII
编译流程：`.cx` → LLVM IR (`.ll`) → 可执行文件

特色：
- 不需要逗号，分号，换行
- 修饰注入的OOP方式
- 函数以变量作返回值

## 二、编程模型

一个 clux 程序从内到外由这几层概念构成：

```
表达式与类型 → 语句 → 函数 → 声明 → 容器 → 修饰
```

- **表达式与类型**：程序处理的数据，以及描述数据的方式。两者共享同一套语法结构。
- **语句**：控制执行流程。
- **函数**：一组语句的集合，可以被调用。
- **声明**：向当前作用域引入新名字。
- **容器**：持有名字的盒子。类与命名空间是两种容器。
- **修饰**：附加到声明或容器上的标签，改变其语义。

这是从"最小单元"到"组织方式"的嵌套。读到后面章节时，前面章节的概念已经介绍完毕，不会出现前向依赖。

## 三、词法

注释：

```cx
// 单行注释

/*
多行注释
*/
```

分隔符：

语句和元素自动分隔。

- `;` 强制分隔语句
- `,` 强制分隔元素

## 四、表达式与类型

表达式与类型共享同一套语法结构。一个结构出现在什么位置，由上下文决定它是表达式还是类型。

### 字面量

字面量直接产生值。

```cx
42
"hello"
true
```

### 基本类型

`int`（四字节）、`short`（二字节）、`long`（八字节）、`float`（四字节）、`double`（八字节）、`char`、`bool`（一字节）。

其他内置类型：`int128`、`float16`、`ushort`、`uint`、`ulong`、`uint128`、`uchar`。

内置类型都可以使用 `typename()` 做类型转换。

### 标识符与路径

标识符引用一个名字。路径用 `::` 连接多个名字，逐级进入容器。

```cx
a
foo::bar
```

### 一元运算

```cx
-a          // 取负
!a          // 逻辑非
~a          // 按位取非
a-*         // 解引用
a-&         // 取地址
```

`null` 为空指针，实际值为 0，不可直接当做数字。类指针的成员访问不会自动解引用。

```cx
int b = a-*
int-* c = int-*(long(a)+4)
int-* d = null
```

### 二元运算

```cx
a + b
a == b
a && b
```

### 调用与索引

```cx
foo(1, 2)
arr[0]
```

调用需要一个函数作为被调用者，函数在第六章介绍。

### 指针与引用类型

用 `typename-*` 表示指针类型，`typename-&` 表示引用类型，`typename-**` 表示多重指针。

```cx
int-* p
int-& r = a
int-** pp
```

指针类型与一元运算的 `-*`、`-&` 是同一个 token 在不同位置的角色。

### 数组类型

用 `T[]` 表示 T 的数组，用 `T[N]` 表示长度为 N 的 T 数组。

```cx
int[] a = [1 2 3 4 5]   // 长度由初始化推导
int[42] buf              // 长度为 42
Foo-<int>[] data         // Foo-<int> 的数组
T[] arr                  // T 的数组
```

数组长度是类型的一部分。长度可以是编译期常量，也可以是运行期值。

```cx
int[5] a = [1 2 3 4 5]  // 长度编译期已知
int n = 10
alloca int[n] b         // 长度运行期已知
```

**动态声明长度的数组必需由储存修饰符修饰**。长度是编译期常量时，默认栈分配；长度是运行期值时，必须显式声明存储方式：

```cx
int n = 10
alloca int[n] b          // 栈上的动态长度数组
malloc int[n] c          // 堆上的动态长度数组
```

数组的值在内存中是一段连续存储。`b.len` 取长度。长度是编译期常量时，`b.len` 是编译期常量；长度是运行期值时，`b.len` 读取存储的长度值。

### 函数指针类型

```cx
$(int)int f        // 参数为 int，返回 int
$int g             // 无参数，返回 int
```

### 类型转换

使用 `typename()` 做类型转换。

```cx
int a = 1
long b = long(a)
```

类可以定义 `conversion` 特殊方法，不支持自动链式转换。

## 五、语句

语句控制执行流程。语句不产生值。

```cx
if cond { ... } else { ... }
while cond { ... }
for int i = 0; i < n; i += 1 { ... }
for item in collection { ... }
break
continue
return
flag label
toflag label
```

## 六、函数

函数是一组语句的集合，可以被调用。

```cx
add(int a, int b) int r {
    r = a + b
}
```

函数的返回声明 `int r` 声明了一个返回变量 `r`。函数末尾会隐式返回 `r` 的值，不需要写 `return r`。也可以显式 `return` 提前返回。

参数与变量声明规则相同。

### 尾随代码块

函数调用时可以直接跟花括号，作为尾随代码块参数：

```cx
with_lock(mutex)--{
    critical_section()
}
```

### 函数字面量

```cx
$(int,int)int max = \(int a, int b) int r {
    if a > b { r = a return }
    r = b
}
```

### 泛型

```cx
foo-<T>(T bar) T r=bar {}
main(){
    foo-<int>(1)
}
```

## 七、声明

声明是引入名字的语法单元。一组声明构成一个作用域。

### 变量声明

变量声明引入一个值量。不赋初值不允许使用（编译期检查），但可以先声明后赋值。

```cx
int a = 10
int b
b = 0
```

`let` 推导变量类型：

```cx
let a = 1        // a 为 int
```

### 函数声明

函数声明引入一个调用量。

### 枚举申明

枚举是一种非容器声明，编译时被分析成数字。

```cx
enum Color {
    Red Blue Green
}
main() {
    color = color::Red
}
```

### 命名

同一作用域下：

**所有声明不得重名**

```cx
add_int(int a, int b) int r = a + b {}
add_double(double a, double b) double r = a + b {}
```

## 八、容器

容器是持有名字的组。

### 命名空间

命名空间只引入名字集合。

```cx
namespace math {
    add(int a, int b) int r { r = a + b }
}

math::add(1, 2)
```

### 类

类是一种容器，其声明一种类型。类可以包含变量声明、函数声明、特殊方法、嵌套容器。

```cx
@Counter {
    int value = 0

    increment() {
        value = value + 1
    }
}
```

类的方法内部可以使用 `self` 显式访问当前实例的成员。无冲突时直接使用成员名，编译器自动解析为 `self.成员名`。

```cx
@Counter {
    int value = 0

    set_value(int value) {
        self.value = value
    }
}
```

### 特殊方法

特殊方法是类容器中的声明，用于定义构造、销毁、引用、访问、类型转换行为。

- `INIT`：构造方法，手动调用返回一个对应类型的值
- `DROP`：对象销毁前自动执行
- `REF` / `DEREF`：被引用 / 结束引用时触发
- `ACCESS`：被获取值时触发
- `conversion`：被当其他类型读取时触发

```cx
@pointer {
    char-* p
    INIT() { p = null }
    INIT(char c) { p = c-& }
}

pointer p = pointer()
```

```cx
@CacheEntry {
    DROP { log("destroyed") }
}
```

### 操作符重载

类中可以重载操作符。可重载的操作符为优先级表中除 `.` `::` `,` `-&` `=` 之外的操作符。

```cx
@Vec2 {
    int x, y
    trait operator + (Vec2 other) Vec2 r {
        r = Vec2(x + other.x, y + other.y)
    }
}
```

### 泛型

```cx
@Array-<T> {
    T[100] data
    int len = 0
    trait op [](int index) T-& r {
        r = data[index]-&
    }
}
```

## 九、修饰

### 修饰的方向性

修饰在**声明点结算**：`#M @D` 中 `M` 必须完整可解析。因此修饰**不能依赖被修饰者的信息**。

两者方向相反，不可互相替代。

### 字段修饰符

| 关键字 | 含义 |
| --- | --- |
| static | 静态 |
| const | 常量 |
| inline | 内联 |
| readonly | 外部只读 |
| own | 唯一所有权 |
| malloc | 堆分配 |
| alloca | 栈分配 |

### 方法修饰符

访问控制：`pub` 公开，`priv` 私有（类默认私有，命名空间默认公开）。

```cx
@Base {
    trait process(string data) {}
    Base_setup() { print("Base setup") }
    setup() { Base_setup() }
}

#Base @Child {
    process(string data) { print("Implemented") }
    Child_setup { Base_setup() print("Child setup") }
    setup() { Child_setup() }
}
```

### 修饰注入

修饰注入是一种容器操作，将修饰符注入到类声明上。

```cx
#foo #bar @baz { }
```

修饰注入也支持泛型：

```cx
#foo-<int> @bar {}
```

## 十、控制流

内置控制流名称保留，不可覆盖：

```cx
if($bool cond $void then)else($void else)
if($bool cond $void then)
while($bool cond $void body)
for($void init $bool cond $void next $void body)
for($void item) in (T collection $void body)
break
continue
return
flag label
toflag label
```

`flag` 与 `toflag` 的本质是 asm 标签跳转。

## 十一、并发与异步

`spawn` 处理函数时，函数会开启新线程执行

```cx
work(int a)int r=a+1
{}
main(){
    spawn(work(1))
}
```

更复杂的并发行为可以封装成类。

## 十二、编译期反射

`type` 是一种类，可以访问类型标识、字段、修饰符。

```cx
@foo #bar { type a = type._foo }

main() {
    type x = type._int
    int i = sizeof(type._foo)
    int j = offset(type._foo 1)
}
```

`sizeof(变量)` 会生成运行时的 `sizeof` 函数，`offset` 同理。

## 十三、导入系统与标准库

```cx
import std
no import std
```

默认自动导入 std。基础模块：

- `cxio`：输入输出
- `metatype`：元类型
- `concurrent`：并发
- `containers`：容器

## 十四、内联汇编与保留代码

`inline` 修饰使函数在汇编层面内联在调用处：

```cx
inline foo(int bar) int
```

``asm `...` `` 在被调用处内联插入汇编代码：

```cx
asm `
mov ecx, 0
`
```

``keep `...` `` 会将代码保留在翻译后的 `.ll` 文件中：

```cx
keep `
define i32 @add(i32 %a, i32 %b) {
    %1 = add i32 %a, %b
    ret i32 %1
}
`
```

## 十五、外部函数接口

```cx
extern "opengl32" {
    glBegin(int mode) {}
    glEnd() {}
}
```

## 十六、编译与执行

工具链命令：

```
clux -t main.cx    // 翻译
clux -b main.cx    // 构建
clux -r main.cx    // 运行
clux -c main.cx    // 检查
clux -o file       // 输出位置
```

## 十七、语法糖

`modf modifier:` 声明此后的同级声明都是 modifier 修饰：

```cx
@foo {
    pub:
    int a
    modf static const:
    int b = 1
    modf:
    int c
}
```

`alias foo = bar:` 将下文作用域中的 `foo` 当 `bar` 处理，仅编译期文本作用：

```cx
namespace foo {
    alias a = b
    int a = 123     // 实际 int b = 123
}
```

## 十八、操作符优先级表

| 优先级 | 运算符 | 结合 |
| --- | --- | --- |
| 17 | `()` `[]` `.` `::` | 左 |
| 16 | `-&` `-*` | 左 |
| 15 | `+` `-` `!` `~` | 右 |
| 14 | `*` `/` `%` | 左 |
| 13 | `+` `-` | 左 |
| 12 | `<<` `>>` | 左 |
| 11 | `<` `>` `<=` `>=` | 左 |
| 10 | `==` `!=` | 左 |
| 9 | `&` | 左 |
| 8 | `^` | 左 |
| 7 | `\|` | 左 |
| 6 | `&&` | 左 |
| 5 | `\|\|` | 左 |
| 4 | `?:` | 右 |
| 3 | `=` `+=` `-=` `*=` `/=` `%=` | 右 |
| 2 | `&=` `\|=` `^=` `<<=` `>>=` | 右 |