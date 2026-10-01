# clux 标准库文档

## cxio 标准输入输出

```cx
import cxio
```

### 输出

```cx
const print(const char[][] values)
const println(const char[][] values)
```

```cx
print("hello" "world")
println("done")
```

### 输入

```cx
input() char r
```

`input` 从标准输入读入一个字符

## metatype 元类型

```cx
import metatype
```

元类型主要依靠派生栈帧实现。不使用元类型时，不会触发派生栈帧。

派生栈帧能够在开启前计算将要拓展的栈帧结构，并会通过额外压栈的方式来补充栈空间。对于大结构可能产生较大性能开销，在栈不能满足时，会转向堆。

**注意**：`type` 关键字是编译期反射的入口（见语言规范第十三章）；`mt::type` 是运行期类型对象，两者不是同一概念。

```cx
main() {
    int a = 0
    type t = type(a)          // 编译期反射
    mt::type T = t            // 运行期类型对象
    mt::var b = mt::build(t 0)
    mt::var c = mt::build(T 0)
}
```

允许组合、迭代：

```cx
import cxio

@foo {
    int a, b
}
@bar {
    string s
}

main() {
    mt::type a = mt::struct(foo, bar)

    mt::type x = type.int
    int cnt = 1
    input-<int>::read(cnt-&)
    for int i = 0; i < cnt; i += 1 {
        x = mt::struct(x, type.int)
    }
    for int i = 0; i < cnt; i += 1 {
        x = x[0]
    }
}
```

元类型包含所有能够被申明的类型，包括指针、函数指针、数组，但不包含引用，因为引用不是类型。

```cx
main() {
    mt::type a = mt::ptr(type.int)
    mt::type b = mt::arr(type.int)
    mt::type c = mt::func(mt::params(type.int), mt::returns(type.int))
}
```

`mt::func` 接受两个参数：参数类型列表和返回类型列表。两者之间用逗号强制分隔，避免与相邻调用混淆。

## concurrent 并发

> 画大饼

## containers 容器

> 画大饼

---
