#include <stdbool.h>
#include <unistd.h>
#include <memory.h>
#include <pthread.h>
#include <stdio.h>

#include "mimalloc.h"
#include "mimalloc-types.h" // for MI_DEBUG
#include "testhelper.h"

#define CONCURRENT_THREADS_COUNT 2
#define BARRIERS_COUNT 4
#define BUFFER_SIZE 8192

char stderr_buffer[BUFFER_SIZE];
char stderr_buffer2[BUFFER_SIZE];
char write_cb_buffer[BUFFER_SIZE];

typedef enum {
  FIRST,
  SECOND,
  CORRUPTED,
} stat_cb_state_t;

typedef struct {
  stat_cb_state_t state;
  bool result;
} stat_thread_t;

static pthread_barrier_t barrier;

static const char *strings_to_parse[] = {
    "normal",
    "huge",
    "large",
    "total",
    "malloc req",
    "reserved",
    "committed",
    "reset",
    "touched",
    "segments",
    "-abandoned",
    "-cached",
    "pages",
    "-abandoned",
    "-extended",
    "-noretire",
    "mmaps",
    "commits",
    "threads",
    "searches",
};

bool is_initial_state(char *string);

bool test_callback_stats(void);
bool test_parallel_stats(void);
bool test_merged_stats(void);

bool stats_print_to_buffer(char *buffer);

// ---------------------------------------------------------------------------
// Main testing
// ---------------------------------------------------------------------------
int main(void) {
  mi_option_disable(mi_option_verbose);
  mi_stats_reset();
  // ---------------------------------------------------
  // Stats (Must be run before all other tests to preserve the initial state of allocator)
  // ---------------------------------------------------
  CHECK_BODY("mi_malloc_stats_print-initial", {
    stats_print_to_buffer(stderr_buffer);
    result = is_initial_state(stderr_buffer) == true;
    memset(stderr_buffer, 0, BUFFER_SIZE);
  });
  CHECK_BODY("mi_malloc_stats_print-after-use", {
    int *arr = mi_malloc(20 * sizeof(int));
    result &= stats_print_to_buffer(stderr_buffer);
    mi_free(arr);

    if (result)
      result = is_initial_state(stderr_buffer) == false;
    memset(stderr_buffer, 0, BUFFER_SIZE);
  });
  CHECK_BODY("mi_malloc_stats_print-buffer-callback", {
    result = test_callback_stats();
  });
  CHECK_BODY("mi_malloc_stats_print-thread-safety", {
    result = test_parallel_stats();
  });
  CHECK_BODY("mi_malloc_stats_print-stats-from-another-thread", {
    result = test_merged_stats();
  });

  // ---------------------------------------------------
  // Done
  // ---------------------------------------------------[]
  return print_test_summary();
}

bool stats_print_to_buffer(char *buffer) {
  fflush(stderr);
  int err_pipe[2];
  int saved_stderr = dup(STDERR_FILENO);
  if (pipe(err_pipe) != 0) {
    perror("Can't create pipe");
    return false;
  }
  dup2(err_pipe[1], STDERR_FILENO);
  close(err_pipe[1]);
  mi_malloc_stats_print(NULL, NULL, "");
  fflush(stderr);
  read(err_pipe[0], buffer, BUFFER_SIZE);
  dup2(saved_stderr, STDERR_FILENO);
  return true;
}

char *skip_whitespaces(char *token) {
  while (*token == ' ') {
    ++token;
  }
  return token;
}

int check_stat_string(char *token) {
  for (size_t i = 0; i < sizeof(strings_to_parse) / sizeof(strings_to_parse[0]); ++i) {
    const char *cur = strings_to_parse[i];
    size_t len = strlen(strings_to_parse[i]);
    if (strncmp(token, cur, len) == 0) {
      return (int)len;
    }
  }
  return -1;
}

bool is_initial_state(char *string) {
  char *token = strtok(string, "\n");
  while (token != NULL) {
    int shift;
    token = skip_whitespaces(token);
    if ((shift = check_stat_string(token)) != -1) {
      token += shift;
      while (*token != ':') {
        ++token;
      }
      ++token;
      do {
        token = skip_whitespaces(token);
        if (*token >= '1' && *token <= '9') {
          return false;
        }
        ++token;
      } while (*token != '\n' && *token != '\0');
    }
    token = strtok(NULL, "\n");
  }
  return true;
}

void *allocate_routine(void *arg) {
  pthread_barrier_t *barriers = arg;
  pthread_barrier_wait(&barriers[0]);
  pthread_barrier_wait(&barriers[1]);
  int *arr = mi_malloc(50 * sizeof(int));
  pthread_barrier_wait(&barriers[2]);
  pthread_barrier_wait(&barriers[3]);
  mi_free(arr);
  return NULL;
}

bool are_equal_stats(const char *stat_output1, const char *stat_output2) {
  char *prefix = "heap stats";
  char *suffix = "elapsed";

  stat_output1 = strstr(stat_output1, prefix);
  stat_output2 = strstr(stat_output2, prefix);

  bool result = false;
  if (stat_output1 != NULL && stat_output2 != NULL) {
    char *end1 = strstr(stat_output1, suffix);
    char *end2 = strstr(stat_output2, suffix);
    if (end1 != NULL && end2 != NULL) {
      end1[0] = '\0';
      end2[0] = '\0';
      result = strcmp(stat_output1, stat_output2) == 0;
    }
  }
  return result;
}

void buffer_cb(void *cbopaque, const char *s) {
  char *buffer = (char *)cbopaque;
  strcat(buffer, s);
}

void first_indicator_cb(void *cbopaque, const char *s) {
  pthread_barrier_wait(&barrier);
  stat_cb_state_t *state = (stat_cb_state_t *)cbopaque;
  if (*state != FIRST) {
    *state = CORRUPTED;
  }
}

void second_indicator_cb(void *cbopaque, const char *s) {
  pthread_barrier_wait(&barrier);
  stat_cb_state_t *state = (stat_cb_state_t *)cbopaque;
  if (*state != SECOND) {
    *state = CORRUPTED;
  }
}

void *stat_print_thread(void *arg) {
  stat_thread_t *thread_data = (stat_thread_t *)arg;
  stat_cb_state_t initial_state = thread_data->state;
  void (*write_cb)(void *, const char *) = NULL;
  switch (initial_state) {
  case FIRST: write_cb = &first_indicator_cb;
    break;
  case SECOND: write_cb = &second_indicator_cb;
    break;
  default: thread_data->result = false;
    return NULL;
  }
  mi_malloc_stats_print(write_cb, &initial_state, "");
  if (initial_state != thread_data->state) {
    thread_data->result = false;
  }
  return NULL;
}

bool test_callback_stats(void) {
  mi_malloc_stats_print(&buffer_cb, write_cb_buffer, "");
  stats_print_to_buffer(stderr_buffer);

  bool result = are_equal_stats(write_cb_buffer, stderr_buffer);

  memset(stderr_buffer, 0, BUFFER_SIZE);
  memset(write_cb_buffer, 0, BUFFER_SIZE);
  return result;  
}

bool test_parallel_stats() {
  if (pthread_barrier_init(&barrier, NULL, CONCURRENT_THREADS_COUNT) != 0) {
    perror("test_parallel_stats: barrier init");
    return false;
  }
  pthread_t thread_id[CONCURRENT_THREADS_COUNT];
  stat_thread_t thread_data[] = {
      {FIRST, true},
      {SECOND, true},
  };

  for (int i = 0; i < CONCURRENT_THREADS_COUNT; ++i) {
    pthread_create(&thread_id[i], NULL, &stat_print_thread, &thread_data[i]);
  }

  bool result = true;
  for (int i = 0; i < CONCURRENT_THREADS_COUNT; ++i) {
    pthread_join(thread_id[i], NULL);
    result &= thread_data[i].result;
  }
  return result;
}

bool test_merged_stats() {
  pthread_barrier_t barriers[BARRIERS_COUNT];
  for (size_t i = 0; i < BARRIERS_COUNT; ++i) {
    if (pthread_barrier_init(&barriers[i], NULL, 2) != 0) {
      perror("barrier init failed");
      return false;
    }
  }
  pthread_t thread_id;

  pthread_create(&thread_id, NULL, &allocate_routine, barriers);

  pthread_barrier_wait(&barriers[0]);

  stats_print_to_buffer(stderr_buffer);

  pthread_barrier_wait(&barriers[1]);
  pthread_barrier_wait(&barriers[2]);

  stats_print_to_buffer(stderr_buffer2);

  pthread_barrier_wait(&barriers[3]);
  pthread_join(thread_id, NULL);

  bool result = !are_equal_stats(stderr_buffer, stderr_buffer2);

  memset(stderr_buffer, 0, BUFFER_SIZE);
  memset(stderr_buffer2, 0, BUFFER_SIZE);
  return result;
}