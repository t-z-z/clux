# clux 未来特性规划

## 闭包与代码块

`\() {}` 函数字面量、`{ }` 块表达式、尾随代码块 `f()--{ }` 三者同源，都是
「把代码当值」。当前一律报 `not supported yet`：语法能读完，但不构造 AST 节点。

现状是纯函数指针，没有捕获：

```cx
triple(int a) int r { r = a * 3 }
apply($int(int) g, int v) int r { r = g(v) }
main() {
    int x = apply(triple, 3)   // 普通函数名当函数指针值，可传可调
}
```

### 嵌套闭包与 self

内层闭包引用的自由变量可能已被外层捕获，需要闭包传递（transitive capture）。
闭包体内引用类字段依赖 `self`，需明确 `self` 是否可捕获。

### 尾随代码块

`f()--{ }` 复用块表达式节点，与闭包一并实现。
`--` 同时是中缀操作（组合声明），前缀位置出现才是代码块。

## 编译期反射

当前无法识别

```cx
foo{int a,b}
main() {
    type x = type.foo
    int i = sizeof(type.foo)
    int j = offset(x 1)
    type k = fieldtype(x 0)
}
```

`sizeof(变量)` 会生成运行时的 `sizeof` 函数，`offset`,`fieldtype` 同理。

## 组合声明

当前无法识别

```cx
foo(int a)--bar(int b)int r=a+b{

}
main(){
    foo(1)--bar(2)
}
```

## let推导

`let` 推导变量类型：

```cx
let a = 1        // a 为 int
```

当前报错 `'let' is not implemented yet`

## lib.md内容均未设计完成未实现

当前lib.md内容未设计完成，尚未开始实现

目前可导入的lib模块只有cextern的16个c头文件对应模块