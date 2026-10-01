# clux 未来特性规划

## 泛型

当前无法识别

函数泛型

```cx
foo-<T>(T bar) T r=bar {}
main(){
    foo-<int>(1)
}
```

类泛型

```cx
@Array-<T> {
    T[100] data
    int len = 0
    op [](int index) T-& r {
        r = data[index]-&
    }
}
```

修饰泛型

```cx
#foo-<int> @bar {}
```

## 编译期反射

当前无法识别

```cx
@foo{int a,b}
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