#include <chrono>
#include <future>
#include <optional>

#include "mimalloc.h"
#include "testhelper.h"
#include "barrier.hpp"

using namespace std::chrono_literals;

constexpr auto TIMEOUT = 1s;

using Duration = decltype(std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::high_resolution_clock::now() - std::chrono::high_resolution_clock::now())
);

bool base_alloc_test(const std::function<std::optional<char *>()> &fun) {
    Barrier barrier_disable(2);
    Barrier barrier_enable(2);

    Duration duration;
    auto t = std::thread([&] {
        barrier_disable.wait();
        auto start = std::chrono::high_resolution_clock::now();
        std::optional<char *> data = fun();
        auto end = std::chrono::high_resolution_clock::now();
        barrier_enable.wait();
        if (data.has_value()) {
            mi_free(data.value());
        }
        duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    });

    mi_malloc_disable();
    barrier_disable.wait();
    std::this_thread::sleep_for(TIMEOUT);
    mi_malloc_enable();
    barrier_enable.wait();
    t.join();

    bool ret = duration >= TIMEOUT;

    return ret;
}

bool test_disable_before_malloc() {
    return base_alloc_test([]() {
        return static_cast<char *>(mi_malloc(sizeof(char) * 48));
    });
}

bool test_disable_before_calloc() {
    return base_alloc_test([]() {
        return static_cast<char *>(mi_calloc(48, sizeof(char)));
    });
}

bool test_disable_before_valloc() {
    return base_alloc_test([]() {
        return static_cast<char *>(mi_valloc(sizeof(char) * 48));
    });
}

bool test_disable_before_realloc() {
    return base_alloc_test([]() {
        return static_cast<char *>(mi_realloc(nullptr, sizeof(char) * 48));
    });
}

bool test_disable_before_free() {
    return base_alloc_test([]() {
        mi_free(nullptr);
        return std::nullopt;
    });
}

//----------------------------------------------------------------------------------
// Main testing
//----------------------------------------------------------------------------------
int main() {
    CHECK_BODY("mi_malloc_disable-before-malloc", {
        result = test_disable_before_malloc();
    });

    CHECK_BODY("mi_malloc_disable-before-calloc", {
        result = test_disable_before_calloc();
    });

    CHECK_BODY("mi_malloc_disable-before-realloc", {
        result = test_disable_before_realloc();
    });

    CHECK_BODY("mi_malloc_disable-before-valloc", {
        result = test_disable_before_valloc();
    });

    CHECK_BODY("mi_malloc_disable-before-free", {
        result = test_disable_before_free();
    });

    // ----------------------------------------------
    // Done
    // ----------------------------------------------
    return print_test_summary();
}