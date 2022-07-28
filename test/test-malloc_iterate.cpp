#include <cassert>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <cstdint>
#include <cstdlib>
#include <array>
#include <algorithm>

#include <cerrno>

#include <functional>
#include <utility>
#include <thread>
#include <sstream>

#include <cstring>
#include <mutex>
#include <chrono>
#include <condition_variable>

#include "mimalloc.h"
#include "testhelper.h"
#include "mimalloc-types.h"
#include "barrier.hpp"

namespace {
    template <typename T, std::size_t C>
    class FixedCapacityVector {
        static_assert(C > 0);
        std::array<T, C> m_backing_array{};
        std::size_t m_size = 0;

        void ensure_capacity() {
            if (m_size >= C) {
                throw std::length_error("Cannot exceed capacity " + std::to_string(C));
            }
        }

    public:
        [[nodiscard]] std::size_t size() const {
            return m_size;
        }

        void push_back(const T &elem) {
            ensure_capacity();
            m_backing_array[m_size] = elem;
            m_size++;
        }

        void push_back(T &&elem) {
            ensure_capacity();
            m_backing_array[m_size] = std::move(elem);
            m_size++;
        }

        using iterator = typename std::array<T, C>::iterator;

        iterator begin() {
            return m_backing_array.begin();
        }

        iterator end() {
            return m_backing_array.begin() + m_size;
        }
    };
} //anonymous namespace

static constexpr size_t kInitialAllocations = 40;
static constexpr size_t kNumAllocs = 50;

bool test_small_allocations();
bool test_large_allocations();

struct AllocDataType {
  void* ptr;
  size_t size;
  size_t size_reported;
  size_t count;
};

template <std::size_t C>
struct TestDataType {
  size_t total_allocated_bytes{};
  FixedCapacityVector<AllocDataType, C> allocs;
};

template <typename T, std::size_t N, std::size_t C>
static void allocate_sizes(TestDataType<C>* test_data, const std::array<T, N>& sizes, const std::function<void *(size_t)>&);

template <std::size_t C>
static bool verify_ptrs_enabled(TestDataType<C>* test_data);

template <std::size_t C>
static bool verify_ptrs_disabled_sync(TestDataType<C>* test_data, Barrier&);

template <std::size_t C>
static void free_ptrs(TestDataType<C>* test_data);

template <std::size_t C>
static void alloc_ptr(TestDataType<C>* test_data, size_t size, const std::function<void *(size_t)>& alloc_func);

template<typename... T>
static constexpr auto make_array(T &&... values) noexcept ->
std::array<typename std::decay<typename std::common_type<T...>::type>::type, sizeof...(T)> {
  using COMMON_T = typename std::decay<typename std::common_type<T...>::type>::type;
  return std::array<COMMON_T, sizeof...(T)> {
      std::forward<COMMON_T>(values)...
  };
}

const auto small_sizes = make_array(
  8,    16,   32,   48,    64,    80,    96,    112,   128,  160,
  192,  224,  256,  320,   384,   448,   512,   640,   768,  896,
  1024, 1280, 1536, 1792,  2048,  2560,  3072,  3584,  4096, 5120,
  6144, 7168, 8192, 10240, 12288, 14336, 16384, 32768, 65536
);

const auto large_sizes = make_array(
  163840, 196608, 229376, 262144, 327680, 393216, 458752, 524228,
  MI_MEDIUM_OBJ_SIZE_MAX + 1, MI_MEDIUM_OBJ_SIZE_MAX + 2
);

const auto huge_sizes = make_array(
  MI_LARGE_OBJ_SIZE_MAX
);

class AllocGetter {
 public:
  virtual std::function<void *(size_t)> operator()() = 0;
};

struct get_default_heap_alloc : public AllocGetter {
  std::function<void *(size_t)> operator()() override {
    return [] (size_t size) {
      return mi_malloc(size);
    };
  }
};

struct get_non_default_heap_alloc : public AllocGetter {
  get_non_default_heap_alloc() : heap(mi_heap_new()) {}

  ~get_non_default_heap_alloc() {
    mi_heap_delete(heap);
  }

  std::function<void *(size_t)> operator()() override {
    return [this] (size_t size) {
      return mi_heap_malloc(heap, size); 
    };
  }

 private:
  mi_heap_s *heap;
};

template <typename Getter = get_default_heap_alloc, typename T, std::size_t N>
static bool test_simple_allocations_base(const std::array<T, N>& sizes) {
  bool ret = false;
  TestDataType<N * kNumAllocs> test_data;
  Getter getter;
  allocate_sizes(&test_data, sizes, getter());
  ret = verify_ptrs_enabled(&test_data);
  free_ptrs(&test_data);
  
  return ret;
}

inline bool test_small_allocations() {
  return test_simple_allocations_base(small_sizes);
}

inline bool test_large_allocations() {
  return test_simple_allocations_base(large_sizes);
}

inline bool test_huge_allocations() {
  return test_simple_allocations_base(huge_sizes);
}

inline bool test_small_allocations_non_default_heap() {
  return test_simple_allocations_base<get_non_default_heap_alloc>(small_sizes);
}

inline bool test_large_allocations_non_default_heap() {
  return test_simple_allocations_base<get_non_default_heap_alloc>(large_sizes);
}

inline bool test_huge_allocations_non_default_heap() {
  return test_simple_allocations_base<get_non_default_heap_alloc>(huge_sizes);
}


template <typename Getter = get_default_heap_alloc, typename T, std::size_t N>
static bool test_multithread_base(const std::array<T, N>& sizes) {
  constexpr std::size_t num_threads = 1;
  FixedCapacityVector<std::thread, num_threads> threads;
  TestDataType<N * kNumAllocs> test_data;
  bool ret = false;

  std::mutex mutex;
  std::condition_variable cv;
  bool allocated = false;
  bool iterated = false;

  std::thread alloc_thread_fn1([&](){
    Getter getter;
    {
      std::lock_guard<std::mutex> allocate_guard(mutex);
        allocate_sizes(&test_data, sizes, getter());
        allocated = true;
    }
    cv.notify_one();
    std::unique_lock<std::mutex> guard(mutex);
    cv.wait(guard, [&iterated] {return iterated; });
  });


  threads.push_back(std::move(alloc_thread_fn1));
  std::unique_lock<std::mutex> verify_lock(mutex);
  cv.wait(verify_lock, [&allocated] {return allocated; });
  ret = verify_ptrs_enabled(&test_data);
  iterated = true;
  verify_lock.unlock();
  cv.notify_one();

  for(auto &t: threads){
    t.join();
  }

  free_ptrs(&test_data);
  return ret;
}

inline bool test_multithread_small_allocations() {
  return test_multithread_base(small_sizes);
}

inline bool test_multithread_large_allocations() {
  return test_multithread_base(large_sizes);
}

inline bool test_multithread_huge_allocations() {
  return test_multithread_base(huge_sizes);
}

inline bool test_multithread_small_allocations_non_default_heap() {
  return test_multithread_base<get_non_default_heap_alloc>(small_sizes);
}

inline bool test_multithread_large_allocations_non_default_heap() {
  return test_multithread_base<get_non_default_heap_alloc>(large_sizes);
}

inline bool test_multithread_huge_allocations_non_default_heap() {
  return test_multithread_base<get_non_default_heap_alloc>(huge_sizes);
}

template <typename Getter = get_default_heap_alloc, typename T, std::size_t N>
static bool test_multithread_abandoned_allocations_base(const std::array<T, N>& sizes) {
  constexpr std::size_t num_threads = 1;
  FixedCapacityVector<std::thread, num_threads> threads;
  TestDataType<N * kNumAllocs> test_data;
  bool ret = false;
  
  std::thread alloc_thread_fn1([&](){
    Getter getter;
    allocate_sizes(&test_data, sizes, getter());
  });

  threads.push_back(std::move(alloc_thread_fn1));

  for(auto &t: threads){
    t.join();
  }

  ret = verify_ptrs_enabled(&test_data);

  free_ptrs(&test_data);
  return ret;
}

inline bool test_multithread_abandoned_small_allocations() {
  return test_multithread_abandoned_allocations_base(small_sizes);
}

inline bool test_multithread_abandoned_large_allocations() {
  return test_multithread_abandoned_allocations_base(large_sizes);
}

inline bool test_multithread_abandoned_huge_allocations() {
  return test_multithread_abandoned_allocations_base(huge_sizes);
}

inline bool test_multithread_abandoned_small_allocations_non_default_heap() {
  return test_multithread_abandoned_allocations_base<get_non_default_heap_alloc>(small_sizes);
}

inline bool test_multithread_abandoned_large_allocations_non_default_heap() {
  return test_multithread_abandoned_allocations_base<get_non_default_heap_alloc>(large_sizes);
}

inline bool test_multithread_abandoned_huge_allocations_non_default_heap() {
  return test_multithread_abandoned_allocations_base<get_non_default_heap_alloc>(huge_sizes);
}

template <typename Getter = get_default_heap_alloc>
static bool test_iterate_while_disabled() {
  using namespace std::chrono_literals;
  bool ret = false;
  TestDataType<2> test_data;

  Barrier barrier_before(2);
  Barrier barrier_after(2);
  Barrier barrier_verify(2);

  Getter getter;

  std::thread alloc_thread_fn1([&](){
    alloc_ptr(&test_data, 1, getter());
    barrier_before.wait();
    alloc_ptr(&test_data, MI_LARGE_OBJ_SIZE_MAX, getter());
    barrier_after.wait();
    barrier_verify.wait();
  });

  std::array<std::thread, 1> threads {std::move(alloc_thread_fn1)};

  barrier_before.wait();

  std::this_thread::sleep_for(1ms);

  ret = verify_ptrs_disabled_sync(&test_data, barrier_after);
  barrier_verify.wait();
  
  for(auto &t: threads){
    t.join();
  }
  free_ptrs(&test_data);

  return ret;
}
// ---------------------------------------------------------------------------
// Main testing
// ---------------------------------------------------------------------------
int main() {
  CHECK_BODY("mi_malloc_iterate_test_while_disabled", {
    result = test_iterate_while_disabled();
  });

  CHECK_BODY("mi_malloc_iterate_test_small_allocations", {
    result = test_small_allocations();
  });

  CHECK_BODY("mi_malloc_iterate_test_large_allocations", {
    result = test_large_allocations();
  });
  
  CHECK_BODY("mi_malloc_iterate_test_huge_allocations", {
    result = test_huge_allocations();
  });

  CHECK_BODY("mi_malloc_iterate_test_small_allocations_heap", {
    result = test_small_allocations_non_default_heap();
  });

  CHECK_BODY("mi_malloc_iterate_test_large_allocations_heap", {
    result = test_large_allocations_non_default_heap();
  });

  CHECK_BODY("mi_malloc_iterate_test_huge_allocations_heap", {
    result = test_huge_allocations_non_default_heap();
  });

  CHECK_BODY("mi_malloc_iterate_test_small_multithreaded_allocations", {
    result = test_multithread_small_allocations();
  });

  CHECK_BODY("mi_malloc_iterate_test_large_multithreaded_allocations", {
    result = test_multithread_large_allocations();
  });

  CHECK_BODY("mi_malloc_iterate_test_huge_multithreaded_allocations", {
    result = test_multithread_huge_allocations();
  });

  CHECK_BODY("mi_malloc_iterate_test_small_multithreaded_allocations_heap", {
    result = test_multithread_small_allocations_non_default_heap();
  });

  CHECK_BODY("mi_malloc_iterate_test_large_multithreaded_allocations_heap", {
    result = test_multithread_large_allocations_non_default_heap();
  });

  CHECK_BODY("mi_malloc_iterate_test_huge_multithreaded_allocations_heap", {
    result = test_multithread_huge_allocations_non_default_heap();
  });

  CHECK_BODY("mi_malloc_iterate_test_small_multithreaded_abandoned_allocations", {
    result = test_multithread_abandoned_small_allocations();
  });

  CHECK_BODY("mi_malloc_iterate_test_large_multithreaded_abandoned_allocations", {
    result = test_multithread_abandoned_large_allocations();
  });

  CHECK_BODY("mi_malloc_iterate_test_huge_multithreaded_abandoned_allocations", {
    result = test_multithread_abandoned_huge_allocations();
  });

  CHECK_BODY("mi_malloc_iterate_test_small_multithreaded_abandoned_allocations_heap", {
    result = test_multithread_abandoned_small_allocations_non_default_heap();
  });

  CHECK_BODY("mi_malloc_iterate_test_large_multithreaded_abandoned_allocations_heap", {
    result = test_multithread_abandoned_large_allocations_non_default_heap();
  });

  CHECK_BODY("mi_malloc_iterate_test_huge_multithreaded_abandoned_allocations_heap", {
    result = test_multithread_abandoned_huge_allocations_non_default_heap();
  });

  // ---------------------------------------------------
  // Done
  // ---------------------------------------------------[]
  return print_test_summary();
}

template <std::size_t C>
void alloc_ptr(TestDataType<C>* test_data, size_t size, const std::function<void *(size_t)>& alloc_func) {
  void* ptr = alloc_func(size);
  assert(ptr != nullptr);
  AllocDataType alloc{ptr, mi_malloc_usable_size(ptr), 0, 0};
  test_data->allocs.push_back(alloc);
}

template <typename T, std::size_t N, std::size_t C>
void allocate_sizes(TestDataType<C>* test_data, const std::array<T, N>& sizes, const std::function<void *(size_t)>& alloc_func) {

  for (size_t size : sizes) {
    for (size_t i = 0; i < kInitialAllocations; i++) {
      void* ptr = mi_malloc(size);
      assert(ptr != nullptr);
      memset(ptr, 0, size);
      mi_free(ptr);
    }
    for (size_t i = 0; i < kNumAllocs; i++) {
      alloc_ptr(test_data, size, alloc_func);
    }
  }
}

template <std::size_t C>
void free_ptrs(TestDataType<C>* test_data) {
  for (auto & alloc : test_data->allocs) {
    mi_free(alloc.ptr);
  }
}

template <std::size_t C>
static void save_pointers(void* base, size_t size, void* data) {
  auto* test_data = reinterpret_cast<TestDataType<C>*>(data);

  test_data->total_allocated_bytes += size;

  uintptr_t end;
  if (__builtin_add_overflow((uintptr_t)base, size, &end)) {
    // Skip this entry
    return;
  }

  for (auto & alloc : test_data->allocs) {
    auto ptr = reinterpret_cast<uintptr_t>(alloc.ptr);
    if (ptr >= (uintptr_t)base && ptr < end) {
      alloc.count++;
      uintptr_t max_size = end - ptr;

      alloc.size_reported = std::min(alloc.size, max_size);
    }
  }
}

template <std::size_t C>
static bool verify_ptrs(TestDataType<C>* test_data, bool disable) {
  if (disable) {
    mi_malloc_disable();
  }
  bool ret = true;
  auto &allocs = test_data->allocs;
  auto address_cmp = [](const auto &left, const auto &right) {
    return (uintptr_t) left.ptr < (uintptr_t) right.ptr;
  };
  auto min_address_element = std::min_element(allocs.begin(), allocs.end(), address_cmp);
  auto max_address_element = std::max_element(allocs.begin(), allocs.end(), address_cmp);
  mi_malloc_iterate(min_address_element->ptr,
                    (uintptr_t) max_address_element->ptr - (uintptr_t) min_address_element->ptr +
                    max_address_element->size,
                    save_pointers<C>,
                    test_data);
  
  if (disable) {
    mi_malloc_enable();
  }

  for (auto & alloc : allocs) {
    if (1UL != alloc.count) {
      ret = false;
    } else {
      --alloc.count;
    }
  }
  return ret;
}

template <std::size_t C>
bool verify_ptrs_enabled(TestDataType<C>* test_data) {
  return verify_ptrs(test_data, false);
}

template <std::size_t C>
bool verify_ptrs_disabled_sync(TestDataType<C>* test_data, Barrier &barrier) {
  barrier.wait();
  return verify_ptrs(test_data, true);
}