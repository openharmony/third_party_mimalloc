mimalloc是一个具有优异性能的通用内存分配器。 

OpenHarmony中默认使用musl自带的内存分配器，如果您需要更好的性能，可以选择使用mimalloc。

mimalloc是malloc的直接替代品，可以在程序中使用，而无需代码更改。要在应用程序中使用mimalloc内存分配器，需要向BUILD.gn文件添加以下依赖项：
ohos_executable("my_app") {
    install_enable = true
    deps = [
      "//third_party/mimalloc:libmimalloc_shared",
      "base:dep1",
      "core:dep2",
    ]
}