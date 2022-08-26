# Mimalloc perfomance tests

**xmalloc-test** - multi-threads benchmark with 100 purely allocating threads, and 100 purely deallocating threads with objects of various sizes migrating between them. This asymmetric producer/consumer pattern is usually difficult to handle by allocators with thread-local caches.

**bench-malloc-thread** - multi-threads benchmark originally prepared for the glibc memory allocator

### How to prepare:

1. Build tests:
```
$ ./build.sh --product-name rk3568 --build-target third_party/mimalloc:mimalloc_test
```

2. Copy tests to device:
```
$ ./hdc_std file send ~/workspace/ohos/out/rk3568/tests/unittest/mimalloc_test/ /data
```

3. Make tests executable on the device:
```
# chmod -R a+x /data/mimalloc_test/
```

### Versions of tests:

- xmalloc-test - test binary that use **mimalloc** memory allocator
- xmalloc-test-default - test binary that use **default musl** allocator
- bench-malloc-thread - test binary that use **mimalloc** memory allocator
- bench-malloc-thread-default - test binary that use **default musl** allocator

#### Run xmalloc-tests:
```
# ./xmalloc-test -w 4 -t 5 -s 64
# ./xmalloc-test-default -w 4 -t 5 -s 64
```

Output is relative time (**rtime**  smaller is better):
```
# ./xmalloc-test -w 4 -t 5 -s 64                                               
rtime: 15.258, free/sec: 6.554 M
# ./xmalloc-test-default -w 4 -t 5 -s 64                                       
rtime: 125.008, free/sec: 0.800 M
```

#### Run bench-malloc-thread:
```
# ./bench-malloc-thread 4
# ./bench-malloc-thread-default 4
```

Output is number of iterations that tests can execute in constant time (**iterations** bigger is better):
```
# ./bench-malloc-thread 4                                                      
18120892 iterations
# ./bench-malloc-thread-default 4                                              
3996257 iterations
```
