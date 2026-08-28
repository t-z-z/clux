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
    type t = type(a)
    mt::type T = t
    mt::var b = mt::build(t 0)
    mt::var c = mt::build(T 0)
}
```

允许组合，迭代

```vo
import voinput
@foo {
    int a,b
}
@bar {
    string s
}
main(){
    mt::type a = mt::struct(foo,bar)

    mt::type x = type._int
    int cnt = 1
    input(cnt-&)
    for int i=0; i<cnt; i+=1{
        x = mt::struct(x type._int)
    }
    for int i=0; i<cnt; i+=1{
        x = x[0]
    }
}
```

元类型包含所有能够被申明的类型，包括指针，函数指针，数组，但是不包含引用，因为引用不是类型。

```vo
main() {
    mt::type a = mt::ptr(type._int)
    mt::type b = mt::arr(type._int)
    mt::type c = mt::func(mt::params(type._int)mt::returns(type._int))
}
```

## concurrent 并发

> 画大饼

## containers 容器

> 画大饼
