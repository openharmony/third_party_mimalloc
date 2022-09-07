#include "mimalloc.h"
#include "testhelper.h"

int main(void) {
    CHECK_BODY("test-mi_malloc_backtrace-stub", {
        result = mi_malloc_backtrace(NULL, NULL, 0) == 0;
    });
    return print_test_summary();
}