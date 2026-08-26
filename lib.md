# volang 标准库文档

## voio 标准输入输出

```vo
import voio

const print(const string[] values)
const println(const string[] values)
input-<T>(T-& data) bool r
inputwp-<T>(const string-& prompt, T-& data) bool r
```

## metatype 元类型

`import metatype`

元类型主要依靠派生栈帧实现，不使用元类型时，不会触发派生栈帧。

派生栈帧能够在开启前计算将要拓展的栈帧结构，并会通过额外压栈的方式来补充栈空间，对于大结构可能产生较大性能开销，在栈不能满足时，会转向堆。

```vo
main(){
    int a = 0
    type T = type(a)
    T b = 0
    let p
    p = T
    p = type(b)
}
```

动态组合的继承规则与类是一样的

```vo
import voinput
@foo {
    int a,b
}
@bar {
    string s
}
main(){
    type a=@foo #bar {
        bool baz
    }

    // 允许进行迭代
    type x = int
    int cnt = 1
    input(\&a)
    for int i=0; i<cnt; i+=1{
        x = @{x p int n}
    }
    return
}
```

使用len和size来获取元类型的字段数量和占用内存大小

```vo
@foo{int x int y}
@bar{}

main() {
    type T = @{int a bool b}
    
    let count = T.len   // 2
    let size = T.size   // 8（假设对齐=4）
    
    // 字段访问
    type U = T[0]       // int
    type V = T[1]       // bool
    type W = T[2]       // panic!
    
    // 安全使用
    if T.len > 0 {
        type First = T[0]
        First x = 10
    }

    // 修饰符获取使用 T.modifier[]
    type K = @foo#bar{int baz = 42}
    K c
    type x = T.modifier[0][1]   // int
}
```

元类型包含所有能够被申明的类型，包括指针，函数指针，数组，但是不包含引用，因为引用不是类型。

```vo
main() {
    type a = type-*
    type b = a-*    // type-**
    b x = b
}
```

## concurrent 并发

> 画大饼

## containers 容器

> 画大饼
