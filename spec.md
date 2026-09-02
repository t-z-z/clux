# volang语言规范文档

## 一、基础信息

语言名称：volang / vo
源文件后缀名：.vo
内存管理默认为RAII

特色：

- 无需分号与逗号
- 直接返回变量
- 使用修饰而非继承的OOP

## 二、词法

注释：
```vo
// 单行注释

/*

多行注释
*/
```

语句和元素自动分隔，使用`;`来强制分隔语句，使用`,`来强制分隔元素

## 三、类型系统

（所有typename和obj都是代称）

### 类型

基本类型：
`int（四字节）, short（二字节）, long（八字节）, float（四字节）, double（八字节）, char, bool（一字节）`

其他内置类型：
`int128, float16, ushort, uint, ulong, uint128, uchar`

let obj类型推导，例`let a=1`，a为int类型，但是没有`let a:int=1`，请写`int a=1`

内置类型都可以使用typename()来做类型转换。

### 变量声明

`int x = 10` 固定类型，不可改变，

不赋初值不允许使用（包括数组）编译期检查，
但是可以先声明后赋值

```vo
int a
int b = a       // 错误
a = 0
int b = a       // 正确
```

同一作用域下的变量名，函数名，类名，命名空间名均不得重名

### 指针

使用`typename-*`表示`typename`类型的指针，
使用`typename-&`表示`typename`类型的引用，
使用`typename-**`表示多重指针，

使用`obj-&`获取地址，
使用`obj-&`解引用指针，（减号加*和&）。

指针不可运算，但可以与数字类型隐式相互转化，会被编译器警告。

`null`为空指针，实际值为0，不可直接当做数字

```vo
int b = a-*
int-* c = a+4        // 下一个int的位置
int-* d = null
```

### 定长数组（切片）

```vo
int[] a = [1 2 3 4 5] // 长度自动推导
int[] b             // 无初值无法自动推导长度
b = [1 2 3]         // 先声明后赋值
int x = b[0]        // x=1
int l = b.len       // l=3
```

数组是一种构造方式，其申明后得到的是一个包含长度的指针。

### 函数指针（或闭包类型）

```vo
$(int)int a=foo     // 限制参数
$int b=foo          // 不要参数
$(int int)int add   // 空函数
```

## 四、函数

### 申明与调用

申明语法格式：

```vo
函数名 -<模板:默认值> (普通参数=默认值) 返回声明=默认值 { 函数体 }
```

模板只允许传入类型或表达式，理论上一个参数可以既用作类型也用作表达式，
没有返回值应省略返回申明

```vo
main() {
    do_something--{
        // 函数调用的代码块参数，可以写在圆括号后，若没有其他参数，可以不写圆括号
        int sum=0
        for(int i=1;i<=9;i+=1){
            sum+=i
        }
    }--{
        // 没有参数时，至少写一个空的括号
        foo()
    }
}

do_something ($void action1 $void action2) int res=0{
    action1()action2()
}
```

不能在函数中申明函数，也不能在函数外部调用函数，main函数会在程序开始时被自动调用。

组合申明（暂未实现）

```vo
pub compose do($void work)while($bool cond) {
    flag start
    work()
    if cond() toflag start
}
main(){
    do--{
        1+1
    }--while(
        1+1==2
    )
}
```

申明形式：`compose 函数名``()函数名``()...`

注意：dowhile不是内置控制流，这里只作为演示使用，实际不能用while申明函数。

### 如何解决语法歧义

```vo
int foo = bar
(42)
int baz = foo
{
    1+1
}
```

我们确实不能直接知道这是函数调用还是变量和表达式，
这是代码块参数还是变量和作用域，

但是在语义分析时我们掌握了他们的类型，就能够把误判的节点修改到正确的位置，即反悔贪心。

比如将`bar(1+1)`识别为函数调用，在语法解析器识别到bar的类型是变量而不是函数时，会将`(1+1)`移到语句层并修改bar的语义。每执行一次反悔，产生一次警告。

当确实不能确定时，直接报错。

### 参数

类型参数（模板）：

```vo
sort-<LEN:len GET:get T>(T arr) {
    for int i=0;i<arr.LEN;i+=1 {
        for int j=i+1;j<arr.LEN;j+=1 {
            if(arr.GET(i)>arr.GET(j)) {
                swap(arr.GET(i),arr.GET(j))
            }
        }
    }
}
swap-<T>(T-& a,T-& b) {
    T t=a
    a=b
    b=t
}
@vec-<T>{
    T[1145] arr
    int len=0
    get(int n)T r{
        if n<len{
            r=arr[n]
        }
    }
    vec(T[] a){
        for(int i=0
            i<a.len&&i<1145
            i+=1
        ) {
            arr[i]=a[i]
        }
        len=a.len
    }
}
main(){
    vec-<int> a
    a=vec(1,2,3,4,5)
    sort-<len get>(a)
}
```

参数申明和变量申明规则完全相同

```vo
foo(const int-& bar) int r {}
const int-& foo_bar = 0

foo(int[] bar) int r {}
int[] bar = [1,2,3]
```

懒惰求值（用常量函数参数，发现是表达式后会自动优化为懒惰求值）：

```vo
foo(const $bool condition const $int expensive_computation) int r=0{
    if(condition()) r=expensive_computation()
}
```

尾随代码块：函数调用时可直接跟花括号，无需括号括起。

```vo
with_lock(mutex)--{
    critical_section()
}
// 不可以不加花括号
with_lock(mutex) --critical_section() // 错误
```

允许在圆括号中使用 type 来传入类型参数
但是只有尖括号里的类型参数支持泛型模板。

当函数的参数申明只有一个类时，支持自动以函数的调用参数列表调用类的构造函数

```vo
@message {
    string[] a;
    init(string[] msg) {
        a=msg;
    }
}
print(message s) {
    for int i=0;i<s.a.len;i+=1 {
        print(s.a[i])
    }
}
main() {
    print("foo""bar")
}
```

### 功能

函数重载：支持参数类型不同，仅返回值与模板参数不同不能重载。

函数指针：`$type(add) f = add; int result = f(1, 2)`

### 函数字面量

语法：`/ [捕获] (参数) 返回 { 函数体 }`

```vo
$(int,int)int max = /(int a,b) int r {
    if (a > b) {
        r=a return
    }
    r=b
}
let foo = {bar()}   // 特例：/[]``()void的语法糖
main(){
    baz(foo)
}
```

```vo
@foo {
    int cnt = 1
    inc($(int)void act){
        act(cnt)
    }
}
main() {
    foo bar
    bar.inc(/(const int& i){
        cnt += i
    })
    // self自动捕获
}
```

## 五、类

或可称为结构体，类在语法结构上是一种容器，用于承载申明。

基本定义：

```vo
@类名`模板` {
    成员申明
    方法申明
}
```

### 方法中的 self

在类的方法内部，可以使用`self`关键字显式访问当前实例的成员。当成员名与局部变量或参数名冲突时，必须使用`self`消除歧义。在无冲突的情况下，可以直接使用成员名，编译器自动将其解析为`self.成员名`。

示例：

```vo
@Counter {
    int value = 0

    increment() {
        value = value + 1
        // 等价于 self.value = self.value + 1
    }

    set_value (int value) {
        self.value = value       // 必须使用 self 区分参数和成员
    }
}
```

### 类的特殊方法

#### 构造方法

构造不会自动被调用，手动调用会返回一个对应类型

```vo
@pointer {
    char-* p
    INIT(){
        p=null
    }
    INIT(char c){
        p=c-&
    }
}
main(){
    pointer p=pointer()
}
```

#### 钩子

DROP 方法在对象被销毁前自动执行，支持修饰符。

定义语法：DROP { 代码 }

```vo
@CacheEntry {
    DROP {
        log("entry destroyed")
    }
}
```

REF钩子在被引用时触发

ACCESS钩子在被获取值时触发

UNREF钩子在结束引用时触发

#### 类型转换方法

类型名加代码定义当改类被当做其他类型读取时会发生什么，
不支持自动链式转换，不可被外部作为函数调用

示例：

```vo
@BigInt {
    static const int MAXN=1000
    int[MAXN] nums
    pub:
    int len
    INIT(int a){
        int i=0
        while a {
            nums[i]=a%10
            a/=10
            i+=1
        }
        len=i
    }
    conversion int r=0{
        int result = 0
        for int i=0;i<len;i+=1 { result = result * 10 + d }
        r=result
    }
}
Bigint a=123
int b=a
```

### 操作符重载

可重载的操作符为操作符优先级表中除了`.` `::` `,` `-&` `=` 的操作符。

可以使用方法修饰符修饰操作符重载。

示例：

```vo
@Vec2 {
    int x, y
    trait operator + (Vec2 other) Vec2 r {
        r=Vec2(x + other.x, y + other.y)
    }
}
```

可以返回引用类型，以同时支持读和写操作

```vo
@Array-<T>{
    T[] data = []
    int len = 0
    trait op [](int index) T-& r{
        r=data[index]-&
    }
}
```

`==` 默认为完全相等，`!=`默认为非完全相等，当比较符（`<` `>` `<=` `>=`）中存在至少一个时，会自动推导到其余的比较符，也可以主动重载。当重载`==`号后，`!=`的默认含义也会对应改变。

二元运算符存在时，会自动推导其对应的赋值运算符，对应的赋值运算符存在时，会自动推导二元运算符，也可以主动重载。

### 模板与泛型

使用尖括号表示模板类型

```vo
@List-<T>{
    pub:
    T[1000] data
    static new(int len){
        for(int i=0; i < len; i+=1){
            data[i] = 0
        }
        return
    }
}
```

### 枚举

```vo
@Color {
    enum{
        Red
        Blue
        Green
    }
    enum Black
    double a
    INIT(double al){
        a=al
    }
}
main() {
    Color c = Color(1.0).Red
}
```

枚举是一种申明，编译时会分析成数字。

## 六、修饰

### 字段修饰符

| 关键字 | 含义 |
| ----- | ---- |
| static | 静态 |
| const | 常量 |
| inline | 内联 |
| readonly | 外部只读 |
| own | 唯一所有权内存管理 |
| malloc | 堆分配 |
| alloca | 栈分配 |

inline也可以修饰字段，将初始化表达式中的函数全部主动内联

### 修饰注入

修饰注入是一种容器操作

```vo
malloc @rc {
    malloc int cnt=1
    REF{
        cnt+=1
    }
    UNREF{
        cnt-=1
        if cnt<=0 {
            self.delete()
        }
    }
}
main(){
    #rc int a=0
}
```

也可以在申明类时直接注入，多个#号多个注入，相同属性会被合并与警告，同名的不同字段会报错

```vo
#foo #bar @baz {
}
main(){
    #a#b int c=0
}
```

可以用类来修饰类

```vo
@foo{
    int a,b
}
#foo @bar{
    bool c
}
```

使用修饰来做接口，零成本抽象

```vo
@arr-<LEN GET> {
    inline len()int r{
        r=LEN
    }
    inline get(int n)int r{
        r=GET
    }
}
@Array#arr-<a.len a[n]> {
    int[42] a
}
```

还有

```vo
@empty_access {
    ACCESS {}
}
@foo #empty_access {
    after ACCESS{
        bar()
    }
}
```

### 方法修饰符

修饰类行为控制：

| 关键字 | 含义 |
| ------ | ---- |
| trait | 修饰目标必须实现 |
| before | 在修饰者前执行 |
| after | 在修饰者后执行 |
| 不写 | 允许被修饰者覆盖，在修饰者后执行 |
| over | 覆盖修饰者方法 |
| final | 禁止覆盖 |

访问控制：

| 关键字 | 含义 |
| ------ | ---- |
| pub | 公开成员（public） |
| priv | 私有成员（private） |
| 不写 | 私有成员 |

修改性控制

| 关键字 | 含义 |
| ------ | ---- |
| mut | 会修改对象 |
| 不写 | 不会修改对象 |

```vo
@Base {
    trait process() (string data)
    can_override() { print("Base") }
    mut before setup() { print("Base setup") }
    mut after cleanup() { print("Base cleanup") }
}

#Base @Child{
    process(string data) { print("Implemented") }
    over can_override() { print("Child") }
    mut before setup() { print("Child setup") }
    mut after cleanup() { print("Child cleanup") }
}
```

## 七、命名空间

命名空间是一种容器

```vo
namespace math {
    add(int a, b) int r { r = a + b }
}
math::add(1, 2)
namespace math:
add(1, 2)
```

## 八、编译期反射

可以访问类型标识枚举，还可以访问字段和修饰符，type是一种类

```vo
@bar{
    int q
}
@foo#bar{
    type a = type._foo
}
main(){
    type x = type._int
    type y = type._foo._a    // y = type
    type z = type._foo[0]    // z = type
    type w = type(a)    // w = type
    int i = sizeof(type._foo)    // i = type enum掩码占用字节数 + 4
    int j = sizeof(type._foo 0)  // j = z
    int k = offset(type._foo 1)  // k = type enum掩码占用字节数
    int l = sizeof(x)       // l = 4
}
```

如果代码中存在type，将会生成的type枚举，否则生成。

如果代码中存在sizeof(变量)，将会生成运行时存在的sizeof函数，offset同理。

## 九、导入系统与标准库

导入std库
`import std`

禁用默认导入：`no import std`
默认自动导入 std

基础模块：

- voio : 输入输出
- metatype : 元类型
- concurrent : 并发
- containers : 容器

见 `lib.md`

## 十、控制流

内置控制流（名称保留，不可覆盖）：

```vo
if($bool cond $void then)else($void else)
if($bool cond $void then)
while($bool cond $void body)
for($void init $bool cond $void next $void body)
for ($void item) in -<T>(T collection $void body) 
    //collection 需要有[]运算符重载或get()方法与len()方法或len字段，或者支持被解读为数组
break
continue
return
flag label // 同一作用域内不能重复label
toflag label // 不能跳出作用域
```

## 十一、并发与异步

并发原语：

```vo
spawn { work } // 开新线程
wait (const bool-& cond) // 阻塞线程直到条件为真
```

使用类定义实现复杂的行为

```vo
@Task -<T>{
    int n
    #rc bool finished = false
    #rc $T work
    Task ($T w){
        work = w
    }
    mut pub start() {
        $T w = work
        spawn {
            w()
            finished = true
        }
    }
}

main() {
    int n,b
    input(n)
    input(b)
    Task-<int> t=Task-<int>{
        solve(n,m)
    }
    init(n m)
    Task.start()
    do_otherthings()
}
```

并发标准库会提供类。

## 十二、内联汇编与保留代码

使用 inline 修饰，使函数在汇编层面内联在调用处

`inline foo(int bar) int`

```asm`` ``` 会在被调用处内联插入汇编代码

```vo
asm `
mov ecx, 0
for_start:
    cmp ecx, 20
    jge for_end
    add ecx, 2
    jmp for_start
for_end:
`
```

flag与toflag的本质也是asm标签跳转

```keep`` ``` 会将代码保留在翻译后的.ll文件中

```vo
keep `
; LLVM IR 内联汇编
define i32 @add(i32 %a, i32 %b) {
    %1 = add i32 %a, %b
    ret i32 %1
}
`
```

## 十三、外部函数接口

```vo
extern "opengl32" {
    glBegin(int mode){}
    glEnd(){}
}
```

## 十四、编译与执行

实现路线：
Volang 源代码 (.vo) -> LLVM IR(.ll) -> 可执行文件

volang到ll的翻译器使用c++编写

工具链命令：
`volc -t main.vo` 翻译
`volc -b main.vo` 构建
`volc -r main.vo` 运行
`volc -c main.vo` 检查
加`-o file` 控制输出文件位置

## 十五、语法糖

`modf modifier:`声明在此之后，同一层级的声明都是modifier修饰符

用`modf:`取消修饰符

```vo
@foo {
    pub:
    int a
    modf static const:
    int b=1
    modf:
    int c
}
```

`alias foo = bar:`语法糖，将下文作用域中的foo当做bar处理，
仅限编译期的文本作用，同名别名会向前覆盖

```vo
int a = 1
namespace foo{
    alias a = b
    int a = 123     // b = 123
    @aaa {          // 还是aaa
        int a       // int b
    }
}
int c = a           // c = 1
```

## 十六、操作符优先级表

| 优先级 | 运算符 | 优先结合 | 说明 |
| ------ | ------ | ------ | ---- |
| 17 | `()` `[]` `.` `::` | 左 | 函数调用、索引、成员访问、命名空间 |
| 16 | `-&` `-*` | 左 | 取地址、解引用 |
| 15 | `+` `-` `!` `~` | 右 | 正负号、逻辑非、按位取非 |
| 14 | `*` `/` `%` | 左 | 乘法、除法、取模 |
| 13 | `+` `-` | 左 | 加法、减法 |
| 12 | `<<` `>>` | 左 | 左移、右移 |
| 11 | `<` `>` `<=` `>=` | 左 | 关系比较 |
| 10 | `==` `!=` | 左 | 相等性比较 |
| 9 | `&` | 左 | 按位与 |
| 8 | `^` | 左 | 按位异或 |
| 7 | `\|` | 左 | 按位或 |
| 6 | `&&` | 左 | 逻辑与（短路） |
| 5 | `\|\|` | 左 | 逻辑或（短路） |
| 4 | `?:` | 右 | 条件运算符 |
| 3 | `=` `+=` `-=` `*=` `/=` `%=` | 右 | 赋值与复合赋值 |
| 2 | `&=` `\|=` `^=` `<<=` `>>=` | 右 | 位运算复合赋值 |
