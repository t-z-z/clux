// 跨模块的 typedef/exprdef 定义：符号表合并后对导入方可见
typedef byte_size = long
exprdef magic = 21 * 2

double_it(byte_size a) byte_size r { r = a * 2 }
