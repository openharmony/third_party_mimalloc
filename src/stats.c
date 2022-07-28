/* ----------------------------------------------------------------------------
Copyright (c) 2018-2021, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/
#include "mimalloc.h"
#include "mimalloc-internal.h"
#include "mimalloc-atomic.h"

#include <stdio.h>  // fputs, stderr
#include <string.h> // memset
#include <inttypes.h> //PRId64, PRIu8

#define XML_PRINT_AMOUNT_BUFFER_SIZE 64
#define XML_PRINT_BINS_BUFFER_SIZE 64
#define XML_STATS_PRINT_BUFFERED_COUNT 255

#if defined(_MSC_VER) && (_MSC_VER < 1920)
#pragma warning(disable:4204)  // non-constant aggregate initializer
#endif

/* -----------------------------------------------------------
  Statistics operations
----------------------------------------------------------- */

static bool mi_is_in_main(void* stat) {
  return ((uint8_t*)stat >= (uint8_t*)&_mi_stats_main
         && (uint8_t*)stat < ((uint8_t*)&_mi_stats_main + sizeof(mi_stats_t)));  
}

static void mi_stat_update(mi_stat_count_t* stat, int64_t amount) {
  if (amount == 0) return;
  if (mi_is_in_main(stat))
  {
    // add atomically (for abandoned pages)
    int64_t current = mi_atomic_addi64_relaxed(&stat->current, amount);
    mi_atomic_maxi64_relaxed(&stat->peak, current + amount);
    if (amount > 0) {
      mi_atomic_addi64_relaxed(&stat->allocated,amount);
    }
    else {
      mi_atomic_addi64_relaxed(&stat->freed, -amount);
    }
  }
  else {
    // add thread local
    stat->current += amount;
    if (stat->current > stat->peak) stat->peak = stat->current;
    if (amount > 0) {
      stat->allocated += amount;
    }
    else {
      stat->freed += -amount;
    }
  }
}

void _mi_stat_counter_increase(mi_stat_counter_t* stat, size_t amount) {  
  if (mi_is_in_main(stat)) {
    mi_atomic_addi64_relaxed( &stat->count, 1 );
    mi_atomic_addi64_relaxed( &stat->total, (int64_t)amount );
  }
  else {
    stat->count++;
    stat->total += amount;
  }
}

void _mi_stat_increase(mi_stat_count_t* stat, size_t amount) {
  mi_stat_update(stat, (int64_t)amount);
}

void _mi_stat_decrease(mi_stat_count_t* stat, size_t amount) {
  mi_stat_update(stat, -((int64_t)amount));
}

// must be thread safe as it is called from stats_merge
static void mi_stat_add(mi_stat_count_t* stat, const mi_stat_count_t* src, int64_t unit) {
  if (stat==src) return;
  if (src->allocated==0 && src->freed==0) return;
  mi_atomic_addi64_relaxed( &stat->allocated, src->allocated * unit);
  mi_atomic_addi64_relaxed( &stat->current, src->current * unit);
  mi_atomic_addi64_relaxed( &stat->freed, src->freed * unit);
  // peak scores do not work across threads.. 
  mi_atomic_addi64_relaxed( &stat->peak, src->peak * unit);
}

static void mi_stat_counter_add(mi_stat_counter_t* stat, const mi_stat_counter_t* src, int64_t unit) {
  if (stat==src) return;
  mi_atomic_addi64_relaxed( &stat->total, src->total * unit);
  mi_atomic_addi64_relaxed( &stat->count, src->count * unit);
}

// must be thread safe as it is called from stats_merge
static void mi_stats_add(mi_stats_t* stats, const mi_stats_t* src) {
  if (stats==src) return;
  mi_stat_add(&stats->segments, &src->segments,1);
  mi_stat_add(&stats->pages, &src->pages,1);
  mi_stat_add(&stats->reserved, &src->reserved, 1);
  mi_stat_add(&stats->committed, &src->committed, 1);
  mi_stat_add(&stats->reset, &src->reset, 1);
  mi_stat_add(&stats->page_committed, &src->page_committed, 1);

  mi_stat_add(&stats->pages_abandoned, &src->pages_abandoned, 1);
  mi_stat_add(&stats->segments_abandoned, &src->segments_abandoned, 1);
  mi_stat_add(&stats->threads, &src->threads, 1);

  mi_stat_add(&stats->malloc, &src->malloc, 1);
  mi_stat_add(&stats->segments_cache, &src->segments_cache, 1);
  mi_stat_add(&stats->normal, &src->normal, 1);
  mi_stat_add(&stats->huge, &src->huge, 1);
  mi_stat_add(&stats->large, &src->large, 1);

  mi_stat_counter_add(&stats->pages_extended, &src->pages_extended, 1);
  mi_stat_counter_add(&stats->mmap_calls, &src->mmap_calls, 1);
  mi_stat_counter_add(&stats->commit_calls, &src->commit_calls, 1);

  mi_stat_counter_add(&stats->page_no_retire, &src->page_no_retire, 1);
  mi_stat_counter_add(&stats->searches, &src->searches, 1);
  mi_stat_counter_add(&stats->normal_count, &src->normal_count, 1);
  mi_stat_counter_add(&stats->huge_count, &src->huge_count, 1);
  mi_stat_counter_add(&stats->large_count, &src->large_count, 1);
#if MI_STAT>1
  for (size_t i = 0; i <= MI_BIN_HUGE; i++) {
    if (src->normal_bins[i].allocated > 0 || src->normal_bins[i].freed > 0) {
      mi_stat_add(&stats->normal_bins[i], &src->normal_bins[i], 1);
    }
  }
#endif
}

/* -----------------------------------------------------------
  Display statistics
----------------------------------------------------------- */

typedef enum {
    TABLE,
    XML
} print_mode_t;

/**
 * Trim string from the end in-place
 * @param s - a null-terminated string
 */
static void trim_right_in_place(char *s) {
  size_t end_exclusive = strlen(s);
  while (end_exclusive > 0 && s[end_exclusive - 1] == ' ') {
    end_exclusive--;
  }
  s[end_exclusive] = '\0';
}

// unit > 0 : size in binary bytes 
// unit == 0: count as decimal
// unit < 0 : count in binary
static void mi_printf_amount(int64_t n, int64_t unit, mi_output_fun* out, void* arg, const char* fmt, print_mode_t print_mode) {
  char buf[32]; buf[0] = 0;  
  int  len = 32;
  const char* suffix = (unit <= 0 ? " " : "B");
  const int64_t base = (unit == 0 ? 1000 : 1024);
  if (unit>0) n *= unit;

  const int64_t pos = (n < 0 ? -n : n);
  if (pos < base) {
    if (n!=1 || suffix[0] != 'B') {  // skip printing 1 B for the unit column
      snprintf(buf, len, "%d %-3s", (int)n, (n==0 ? "" : suffix));
    }
  }
  else {
    int64_t divider = base;    
    const char* magnitude = "K";
    if (pos >= divider*base) { divider *= base; magnitude = "M"; }
    if (pos >= divider*base) { divider *= base; magnitude = "G"; }
    const int64_t tens = (n / (divider/10));
    const int64_t whole = tens/10;
    const int8_t frac1 = (int8_t)(tens%10);
    char unitdesc[8];
    snprintf(unitdesc, 8, "%s%s%s", magnitude, (base==1024 ? "i" : ""), suffix);
    snprintf(buf, len, "%" PRId64 ".%" PRId8 " %-3s", whole, (frac1 < 0 ? -frac1 : frac1), unitdesc);
  }
  if (print_mode == XML) {
    trim_right_in_place(buf);
  }
  _mi_fprintf(out, arg, (fmt==NULL ? "%11s" : fmt), buf);
}


static void mi_print_amount(int64_t n, int64_t unit, mi_output_fun* out, void* arg) {
  mi_printf_amount(n,unit,out,arg,NULL,TABLE);
}

static void mi_print_count(int64_t n, int64_t unit, mi_output_fun* out, void* arg) {
  if (unit==1) _mi_fprintf(out, arg, "%11s"," ");
          else mi_print_amount(n,0,out,arg);
}

static void mi_stat_print(const mi_stat_count_t* stat, const char* msg, int64_t unit, mi_output_fun* out, void* arg ) {
  _mi_fprintf(out, arg,"%10s:", msg);
  if (unit>0) {
    mi_print_amount(stat->peak, unit, out, arg);
    mi_print_amount(stat->allocated, unit, out, arg);
    mi_print_amount(stat->freed, unit, out, arg);
    mi_print_amount(stat->current, unit, out, arg);
    mi_print_amount(unit, 1, out, arg);
    mi_print_count(stat->allocated, unit, out, arg);
    if (stat->allocated > stat->freed)
      _mi_fprintf(out, arg, "  not all freed!\n");
    else
      _mi_fprintf(out, arg, "  ok\n");
  }
  else if (unit<0) {
    mi_print_amount(stat->peak, -1, out, arg);
    mi_print_amount(stat->allocated, -1, out, arg);
    mi_print_amount(stat->freed, -1, out, arg);
    mi_print_amount(stat->current, -1, out, arg);
    if (unit==-1) {
      _mi_fprintf(out, arg, "%22s", "");
    }
    else {
      mi_print_amount(-unit, 1, out, arg);
      mi_print_count((stat->allocated / -unit), 0, out, arg);
    }
    if (stat->allocated > stat->freed)
      _mi_fprintf(out, arg, "  not all freed!\n");
    else
      _mi_fprintf(out, arg, "  ok\n");
  }
  else {
    mi_print_amount(stat->peak, 1, out, arg);
    mi_print_amount(stat->allocated, 1, out, arg);
    _mi_fprintf(out, arg, "%11s", " ");  // no freed 
    mi_print_amount(stat->current, 1, out, arg);
    _mi_fprintf(out, arg, "\n");
  }
}

static void mi_stat_counter_print(const mi_stat_counter_t* stat, const char* msg, mi_output_fun* out, void* arg ) {
  _mi_fprintf(out, arg, "%10s:", msg);
  mi_print_amount(stat->total, -1, out, arg);
  _mi_fprintf(out, arg, "\n");
}

static void mi_stat_counter_print_avg(const mi_stat_counter_t* stat, const char* msg, mi_output_fun* out, void* arg) {
  const int64_t avg_tens = (stat->count == 0 ? 0 : (stat->total*10 / stat->count));
  const int64_t avg_whole = avg_tens/10;
  const int8_t avg_frac1 = (int8_t)(avg_tens%10);
  _mi_fprintf(out, arg, "%10s: %5" PRId64 ".%" PRId8 " avg\n", msg, avg_whole, avg_frac1);
}

typedef struct {
  int64_t avg_whole;
  int8_t avg_frac;
} avg_t;

static avg_t mi_get_avg(const mi_stat_counter_t *stat) {
  const int64_t avg_tens = (stat->count == 0 ? 0 : (stat->total*10 / stat->count));
  return (avg_t) {.avg_whole = avg_tens / 10, .avg_frac = (int8_t) (avg_tens % 10)};
}

static void mi_stat_counter_print_avg_xml(const mi_stat_counter_t* stat, const char* name, mi_output_fun* out, void* arg) {
  const avg_t avg = mi_get_avg(stat);
  _mi_fprintf(out, arg, "<%s>%" PRId64 ".%" PRId8 " avg </%s>\n", name, avg.avg_whole, avg.avg_frac, name);
}

static void mi_print_header(mi_output_fun* out, void* arg ) {
  _mi_fprintf(out, arg, "%10s: %10s %10s %10s %10s %10s %10s\n", "heap stats", "peak   ", "total   ", "freed   ", "current   ", "unit   ", "count   ");
}

#if MI_STAT>1
static void mi_stats_print_bins(const mi_stat_count_t* bins, uint8_t max, const char* fmt, mi_output_fun* out, void* arg) {
  bool found = false;
  char buf[64];
  for (uint8_t i = 0; i <= max; i++) {
    if (bins[i].allocated > 0) {
      found = true;
      size_t unit = _mi_bin_size((uint8_t)i);
      snprintf(buf, 64, "%s %3" PRIu8, fmt, (long)i);
      mi_stat_print(&bins[i], buf, unit, out, arg);
    }
  }
  if (found) {
    _mi_fprintf(out, arg, "\n");
    mi_print_header(out, arg);
  }
}
#endif



//------------------------------------------------------------
// Use an output wrapper for line-buffered output
// (which is nice when using loggers etc.)
//------------------------------------------------------------
typedef struct buffered_s {
  mi_output_fun* out;   // original output function
  void*          arg;   // and state
  char*          buf;   // local buffer of at least size `count+1`
  size_t         used;  // currently used chars `used <= count`  
  size_t         count; // total chars available for output
} buffered_t;

static void mi_buffered_flush(buffered_t* buf) {
  buf->buf[buf->used] = 0;
  _mi_fputs(buf->out, buf->arg, NULL, buf->buf);
  buf->used = 0;
}

static void mi_buffered_out(const char* msg, void* arg) {
  buffered_t* buf = (buffered_t*)arg;
  if (msg==NULL || buf==NULL) return;
  for (const char* src = msg; *src != 0; src++) {
    char c = *src;
    if (buf->used >= buf->count) mi_buffered_flush(buf);
    mi_assert_internal(buf->used < buf->count);
    buf->buf[buf->used++] = c;
    if (c == '\n') mi_buffered_flush(buf);
  }
}

//------------------------------------------------------------
// Print statistics
//------------------------------------------------------------

typedef struct {
  void (*write_cb) (void*, const char*);
  char *arg;
} arg_cb_wrapper_t;

static void mi_stat_process_info(mi_msecs_t* elapsed, mi_msecs_t* utime, mi_msecs_t* stime, size_t* current_rss, size_t* peak_rss, size_t* current_commit, size_t* peak_commit, size_t* page_faults);

static void mi_stat_print_xml_element(const mi_stat_count_t *, const char *, int64_t, mi_output_fun *, void *);

static void mi_print_allocations(const mi_stats_t *stats, mi_output_fun *out, void *arg, print_mode_t print_mode) {
  void (*stat_print_cb)
  (
      const mi_stat_count_t *,
      const char *, int64_t,
      mi_output_fun *,
      void *
  ) = print_mode == TABLE ? mi_stat_print : mi_stat_print_xml_element;
  stat_print_cb(&stats->normal, "normal",
      (stats->normal_count.count == 0 ? 1 : -(stats->normal.allocated / stats->normal_count.count)), out, arg);
  stat_print_cb(&stats->large, "large",
      (stats->large_count.count == 0 ? 1 : -(stats->large.allocated / stats->large_count.count)), out, arg);
  stat_print_cb(&stats->huge, "huge",
      (stats->huge_count.count == 0 ? 1 : -(stats->huge.allocated / stats->huge_count.count)), out, arg);
  mi_stat_count_t total = {0, 0, 0, 0};
  mi_stat_add(&total, &stats->normal, 1);
  mi_stat_add(&total, &stats->large, 1);
  mi_stat_add(&total, &stats->huge, 1);
  stat_print_cb(&total, "total", 1, out, arg);
}

static void _mi_stats_print(mi_stats_t* stats, mi_output_fun* out0, void* arg0) mi_attr_noexcept {
  // wrap the output function to be line buffered
  char buf[256];
  buffered_t buffer = { out0, arg0, NULL, 0, 255 };
  buffer.buf = buf;
  mi_output_fun* out = &mi_buffered_out;
  void* arg = &buffer;

  // and print using that
  mi_print_header(out,arg);
  #if MI_STAT>1
  mi_stats_print_bins(stats->normal_bins, MI_BIN_HUGE, "normal",out,arg);
  #endif
  #if MI_STAT
  mi_print_allocations(stats, out, arg, TABLE);
  #endif
  #if MI_STAT>1
  mi_stat_print(&stats->malloc, "malloc req", 1, out, arg);
  _mi_fprintf(out, arg, "\n");
  #endif
  mi_stat_print(&stats->reserved, "reserved", 1, out, arg);
  mi_stat_print(&stats->committed, "committed", 1, out, arg);
  mi_stat_print(&stats->reset, "reset", 1, out, arg);
  mi_stat_print(&stats->page_committed, "touched", 1, out, arg);
  mi_stat_print(&stats->segments, "segments", -1, out, arg);
  mi_stat_print(&stats->segments_abandoned, "-abandoned", -1, out, arg);
  mi_stat_print(&stats->segments_cache, "-cached", -1, out, arg);
  mi_stat_print(&stats->pages, "pages", -1, out, arg);
  mi_stat_print(&stats->pages_abandoned, "-abandoned", -1, out, arg);
  mi_stat_counter_print(&stats->pages_extended, "-extended", out, arg);
  mi_stat_counter_print(&stats->page_no_retire, "-noretire", out, arg);
  mi_stat_counter_print(&stats->mmap_calls, "mmaps", out, arg);
  mi_stat_counter_print(&stats->commit_calls, "commits", out, arg);
  mi_stat_print(&stats->threads, "threads", -1, out, arg);
  mi_stat_counter_print_avg(&stats->searches, "searches", out, arg);
  _mi_fprintf(out, arg, "%10s: %7zu\n", "numa nodes", _mi_os_numa_node_count());
  
  mi_msecs_t elapsed;
  mi_msecs_t user_time;
  mi_msecs_t sys_time;
  size_t current_rss;
  size_t peak_rss;
  size_t current_commit;
  size_t peak_commit;
  size_t page_faults;
  mi_stat_process_info(&elapsed, &user_time, &sys_time, &current_rss, &peak_rss, &current_commit, &peak_commit, &page_faults);
  _mi_fprintf(out, arg, "%10s: %7" PRId64 ".%03" PRId64 " s\n", "elapsed", elapsed/1000, elapsed%1000);
  _mi_fprintf(out, arg, "%10s: user: %" PRId64 ".%03" PRId64 " s, system: %" PRId64 ".%03" PRId64 " s, faults: %zu, rss: ", "process",
              user_time/1000, user_time%1000, sys_time/1000, sys_time%1000, page_faults);
  mi_printf_amount((int64_t)peak_rss, 1, out, arg, "%s", TABLE);
  if (peak_commit > 0) {
    _mi_fprintf(out, arg, ", commit: ");
    mi_printf_amount((int64_t)peak_commit, 1, out, arg, "%s", TABLE);
  }
  _mi_fprintf(out, arg, "\n");  
}

static mi_msecs_t mi_process_start; // = 0

static mi_stats_t* mi_stats_get_default(void) {
  mi_heap_t* heap = mi_heap_get_default();
  return &heap->tld->stats;
}

static void mi_stats_merge_from(mi_stats_t* stats) {
  if (stats != &_mi_stats_main) {
    mi_stats_add(&_mi_stats_main, stats);
    memset(stats, 0, sizeof(mi_stats_t));
  }
}

void mi_stats_reset(void) mi_attr_noexcept {
  mi_stats_t* stats = mi_stats_get_default();
  if (stats != &_mi_stats_main) { memset(stats, 0, sizeof(mi_stats_t)); }
  memset(&_mi_stats_main, 0, sizeof(mi_stats_t));
  if (mi_process_start == 0) { mi_process_start = _mi_clock_start(); };
}

void mi_stats_merge(void) mi_attr_noexcept {
  mi_stats_merge_from( mi_stats_get_default() );
}

void _mi_stats_done(mi_stats_t* stats) {  // called from `mi_thread_done`
  mi_stats_merge_from(stats);
}

void mi_stats_print_out(mi_output_fun* out, void* arg) mi_attr_noexcept {
  mi_stats_merge_from(mi_stats_get_default());
  _mi_stats_print(&_mi_stats_main, out, arg);
}

void mi_stats_print(void* out) mi_attr_noexcept {
  // for compatibility there is an `out` parameter (which can be `stdout` or `stderr`)
  mi_stats_print_out((mi_output_fun*)out, NULL);
}

void mi_thread_stats_print_out(mi_output_fun* out, void* arg) mi_attr_noexcept {
  _mi_stats_print(mi_stats_get_default(), out, arg);
}

static void mi_stats_print_out_callback_wrapper(const char* message, void* arg) {
  // swap arguments for compatibility with je_malloc_stats_print
  const arg_cb_wrapper_t *arg_wrapper = arg;
  arg_wrapper->write_cb(arg_wrapper->arg, message);
}

static mi_stats_t mi_stats_merge_all_heaps_stats(void) {
  mi_stats_t merged_stats = _mi_stats_get_empty_stats();
  _mi_heap_lock_heap_queue();
  mi_stats_add(&merged_stats, &_mi_stats_main);
  mi_heap_t* heap = _mi_heap_main_get();
  while (heap != NULL) {
    mi_stats_add(&merged_stats, &heap->tld->stats);
    heap = heap->next_thread_heap;
  }
  _mi_heap_unlock_heap_queue();
  return merged_stats;
}

void mi_malloc_stats_print(void (*write_cb) (void*, const char*), void* cbopaque, const char* opts) {
  MI_UNUSED(opts);
  mi_stats_t merged_stats = mi_stats_merge_all_heaps_stats();
  if (write_cb == NULL) {
      _mi_stats_print(&merged_stats, NULL, NULL);
  }
  else {
      arg_cb_wrapper_t arg = {
          .write_cb = write_cb,
          .arg = cbopaque,
      };
      _mi_stats_print(&merged_stats, &mi_stats_print_out_callback_wrapper, (void *)&arg);
  }
}

static void print_to_file(const char *s, void *fp) {
  fputs(s, fp);
}

static void mi_print_amount_xml(const char *name, int64_t n, int64_t unit, mi_output_fun *out, void *arg) {
  char buf[XML_PRINT_AMOUNT_BUFFER_SIZE];
  snprintf(buf, XML_PRINT_AMOUNT_BUFFER_SIZE, "<%s>%%s</%s>\n", name, name);
  mi_printf_amount(n, unit, out, arg, buf, XML);
}

static void mi_print_count_xml(int64_t n, int64_t unit, mi_output_fun *out, void *arg) {
  if (unit == 1) {
    return;
  }
  mi_print_amount_xml("count", n, 0, out, arg);
}

static void mi_stat_print_allocation_result_xml(const mi_stat_count_t *stat, mi_output_fun *out, void *arg) {
  if(stat->allocated > stat->freed) {
    _mi_fprintf(out, arg, "<result>not all freed!</result>\n");
  }
  else {
    _mi_fprintf(out, arg, "<result>ok</result>\n");
  }
}

// unit > 0 => size in binary bytes (number + B|KiB|MiB|GiB)
static void mi_stat_print_size_stats_xml(const mi_stat_count_t *stat, int64_t unit, mi_output_fun *out, void *arg) {
  mi_print_amount_xml("peak", stat->peak, unit, out, arg); // size in binary bytes
  mi_print_amount_xml("total", stat->allocated, unit, out, arg);
  mi_print_amount_xml("freed", stat->freed, unit, out, arg);
  mi_print_amount_xml("current", stat->current, unit, out, arg);
  if (unit != 1) { // hack
    mi_print_amount_xml("unit", unit, 1, out, arg);
  }
  mi_print_count_xml(stat->allocated, unit, out, arg);
}

// unit < 0 => count in binary (number or number + Ki|Mi|Gi)
static void mi_stat_print_binary_count_stats_xml(const mi_stat_count_t *stat, int64_t unit, mi_output_fun *out, void *arg) {
  mi_print_amount_xml("peak", stat->peak, -1, out, arg);
  mi_print_amount_xml("total", stat->allocated, -1, out, arg);
  mi_print_amount_xml("freed", stat->freed, -1, out, arg);
  mi_print_amount_xml("current", stat->current, -1, out, arg);
  if (unit != -1) {
    mi_print_amount_xml("unit", -unit, 1, out, arg);
    // unit == 0 => count in decimal (number or number + Ki|Mi|Gi)
    mi_print_count_xml((stat->allocated / -unit), 0, out, arg);
  }
}

// unit > 0 => size in binary bytes (number + B|KiB|MiB|GiB)
static void mi_stat_print_size_stats_no_freed_xml(const mi_stat_count_t *stat, mi_output_fun *out, void *arg) {
  mi_print_amount_xml("peak", stat->peak, -1, out, arg);
  mi_print_amount_xml("total", stat->allocated, -1, out, arg);
  mi_print_amount_xml("current", stat->current, -1, out, arg);  
}

static void mi_stat_print_body_xml(const mi_stat_count_t *stat, int64_t unit, mi_output_fun *out, void *arg) {
  if (unit == 0) {
    mi_stat_print_size_stats_no_freed_xml(stat, out, arg);
    return;
  }
  if (unit > 0) {
    mi_stat_print_size_stats_xml(stat, unit, out, arg);
  } else {
    mi_stat_print_binary_count_stats_xml(stat, unit, out, arg);
  }
  mi_stat_print_allocation_result_xml(stat, out, arg);
}

static void mi_stat_print_xml_element_with_attrs(const mi_stat_count_t *stat, const char *name, const char *attrs, int64_t unit, mi_output_fun *out, void *arg) {

  _mi_fprintf(out, arg, "<%s%s>\n", name, attrs);
  mi_stat_print_body_xml(stat, unit, out, arg);
  _mi_fprintf(out, arg, "</%s>\n", name);
}

static void mi_stat_print_xml_element(const mi_stat_count_t *stat, const char *name, int64_t unit, mi_output_fun *out, void *arg) {
  mi_stat_print_xml_element_with_attrs(stat, name, "", unit, out, arg);
}

static void mi_stat_counter_print_xml(const mi_stat_counter_t *stat, const char *name, mi_output_fun *out, void *arg) {
  mi_print_amount_xml(name, stat->total, -1, out, arg);
}

static void mi_print_milliseconds_xml(const char *name, mi_msecs_t msecs, mi_output_fun *out, void *arg) {
  _mi_fprintf(out, arg, "<%s>%" PRId64 ".%03" PRId64 " s</%s>\n", name, msecs / 1000, msecs % 1000, name);
}

#if MI_STAT > 1

static void mi_stats_print_bins_xml(const mi_stat_count_t *bins, uint8_t max, mi_output_fun *out, void *arg) {
  char buf[XML_PRINT_BINS_BUFFER_SIZE];
  for (uint8_t i = 0; i <= max; i++) {
    if (bins[i].allocated > 0) {
      size_t unit = _mi_bin_size(i);
      snprintf(buf, XML_PRINT_BINS_BUFFER_SIZE, " size_class=\"%" PRIu8 "\"", i);
      mi_stat_print_xml_element_with_attrs(&bins[i], "bin", buf, unit, out, arg);
    }
  }
}

#endif

static void mi_stats_print_process_info_xml(mi_output_fun *out, void *arg) {
  mi_msecs_t elapsed;
  mi_msecs_t user_time;
  mi_msecs_t sys_time;
  size_t current_rss;
  size_t peak_rss;
  size_t current_commit;
  size_t peak_commit;
  size_t page_faults;
  mi_stat_process_info(&elapsed, &user_time, &sys_time, &current_rss, &peak_rss, &current_commit, &peak_commit, 
                       &page_faults);
  mi_print_milliseconds_xml("elapsed", elapsed, out, arg);
  _mi_fprintf(out, arg, "<process>\n");
  mi_print_milliseconds_xml("user", user_time, out, arg);
  mi_print_milliseconds_xml("system", sys_time, out, arg);
  _mi_fprintf(out, arg, "<faults>%zu</faults>\n", page_faults);
  mi_print_amount_xml("rss", peak_rss, 1, out, arg);
  if (peak_commit > 0) {
    mi_print_amount_xml("commit", peak_commit, 1, out, arg);
  }
  _mi_fprintf(out, arg, "</process>\n");
}

static void mi_stats_print_segments_xml(const mi_stats_t *stats, mi_output_fun *out, void *arg) {
  _mi_fprintf(out, arg, "<segments>\n");
  mi_stat_print_body_xml(&stats->segments, -1, out, arg);
  mi_stat_print_xml_element(&stats->segments_abandoned, "abandoned", -1, out, arg);
  mi_stat_print_xml_element(&stats->segments_cache, "cached", -1, out, arg);
  _mi_fprintf(out, arg, "</segments>\n");
}

static void mi_stats_print_pages_xml(const mi_stats_t *stats, mi_output_fun *out, void *arg) {
  _mi_fprintf(out, arg, "<pages>\n");
  mi_stat_print_body_xml(&stats->pages, -1, out, arg);
  mi_stat_print_xml_element(&stats->pages_abandoned, "abandoned", -1, out, arg);
  mi_stat_counter_print_xml(&stats->pages_extended, "extended", out, arg);
  mi_stat_counter_print_xml(&stats->page_no_retire, "noretire", out, arg);
  _mi_fprintf(out, arg, "</pages>\n");
}

static void mi_stats_print_xml(const mi_stats_t *stats, mi_output_fun *out0, void *arg0) mi_attr_noexcept {
  // wrap the output function to be line buffered
  char buf[XML_STATS_PRINT_BUFFERED_COUNT + 1];
  buffered_t buffer = {out0, arg0, buf, 0, XML_STATS_PRINT_BUFFERED_COUNT};
  mi_output_fun *out = &mi_buffered_out;
  void *arg = &buffer;

#if MI_STAT > 1
  _mi_fprintf(out, arg, "<bins>\n");
  mi_stats_print_bins_xml(stats->normal_bins, MI_BIN_HUGE, out, arg);
  _mi_fprintf(out, arg, "</bins>\n");
#endif
#if MI_STAT
  _mi_fprintf(out, arg, "<allocations>\n");
  mi_print_allocations(stats, out, arg, XML);
  _mi_fprintf(out, arg, "</allocations>\n");

#endif
#if MI_STAT > 1
  mi_stat_print_xml_element(&stats->malloc, "malloc_req", 1, out, arg);
#endif
  mi_stat_print_xml_element(&stats->reserved, "reserved", 1, out, arg);
  mi_stat_print_xml_element(&stats->committed, "committed", 1, out, arg);
  mi_stat_print_xml_element(&stats->reset, "reset", 1, out, arg);
  mi_stat_print_xml_element(&stats->page_committed, "touched", 1, out, arg);

  mi_stats_print_segments_xml(stats, out, arg);

  mi_stats_print_pages_xml(stats, out, arg);

  mi_stat_counter_print_xml(&stats->mmap_calls, "mmaps", out, arg);
  mi_stat_counter_print_xml(&stats->commit_calls, "commits", out, arg);
  mi_stat_print_xml_element(&stats->threads, "threads", -1, out, arg);

  mi_stat_counter_print_avg_xml(&stats->searches, "searches", out, arg);
  _mi_fprintf(out, arg, "<numa_nodes>%zu</numa_nodes>\n", _mi_os_numa_node_count());
  mi_stats_print_process_info_xml(out, arg);
}

int mi_malloc_info(int options, FILE *fp) {
  if (options != 0) {
    errno = EINVAL;
    return -1;
  }
  _mi_fprintf(print_to_file, fp, "<?xml version=\"1.0\"?>\n");
  _mi_fprintf(print_to_file, fp, "<malloc version=\"mimalloc-%d\">\n", mi_version());
  _mi_heap_lock_heap_queue();

  _mi_fprintf(print_to_file, fp, "<stats_main>\n");
  mi_stats_print_xml(&_mi_stats_main, print_to_file, fp);
  _mi_fprintf(print_to_file, fp, "</stats_main>\n");
  mi_heap_t* heap = _mi_heap_main_get();
  while (heap != NULL) {
    _mi_fprintf(print_to_file, fp, "<heap thread_id=\"%zu\">\n", heap->thread_id);
    mi_stats_print_xml(&heap->tld->stats, print_to_file,fp);
    heap = heap->next_thread_heap;
    _mi_fprintf(print_to_file, fp, "</heap>\n");
  }
  _mi_heap_unlock_heap_queue();
  _mi_fprintf(print_to_file, fp, "</malloc>\n");
  return 0;
}

// ----------------------------------------------------------------
// Basic timer for convenience; use milli-seconds to avoid doubles
// ----------------------------------------------------------------
#ifdef _WIN32
#include <windows.h>
static mi_msecs_t mi_to_msecs(LARGE_INTEGER t) {
  static LARGE_INTEGER mfreq; // = 0
  if (mfreq.QuadPart == 0LL) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    mfreq.QuadPart = f.QuadPart/1000LL;
    if (mfreq.QuadPart == 0) mfreq.QuadPart = 1;
  }
  return (mi_msecs_t)(t.QuadPart / mfreq.QuadPart);  
}

mi_msecs_t _mi_clock_now(void) {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return mi_to_msecs(t);
}
#else
#include <time.h>
#if defined(CLOCK_REALTIME) || defined(CLOCK_MONOTONIC)
mi_msecs_t _mi_clock_now(void) {
  struct timespec t;
  #ifdef CLOCK_MONOTONIC
  clock_gettime(CLOCK_MONOTONIC, &t);
  #else  
  clock_gettime(CLOCK_REALTIME, &t);
  #endif
  return ((mi_msecs_t)t.tv_sec * 1000) + ((mi_msecs_t)t.tv_nsec / 1000000);
}
#else
// low resolution timer
mi_msecs_t _mi_clock_now(void) {
  return ((mi_msecs_t)clock() / ((mi_msecs_t)CLOCKS_PER_SEC / 1000));
}
#endif
#endif


static mi_msecs_t mi_clock_diff;

mi_msecs_t _mi_clock_start(void) {
  if (mi_clock_diff == 0.0) {
    mi_msecs_t t0 = _mi_clock_now();
    mi_clock_diff = _mi_clock_now() - t0;
  }
  return _mi_clock_now();
}

mi_msecs_t _mi_clock_end(mi_msecs_t start) {
  mi_msecs_t end = _mi_clock_now();
  return (end - start - mi_clock_diff);
}


// --------------------------------------------------------
// Basic process statistics
// --------------------------------------------------------

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#pragma comment(lib,"psapi.lib")

static mi_msecs_t filetime_msecs(const FILETIME* ftime) {
  ULARGE_INTEGER i;
  i.LowPart = ftime->dwLowDateTime;
  i.HighPart = ftime->dwHighDateTime;
  mi_msecs_t msecs = (i.QuadPart / 10000); // FILETIME is in 100 nano seconds
  return msecs;
}

static void mi_stat_process_info(mi_msecs_t* elapsed, mi_msecs_t* utime, mi_msecs_t* stime, size_t* current_rss, size_t* peak_rss, size_t* current_commit, size_t* peak_commit, size_t* page_faults) 
{
  *elapsed = _mi_clock_end(mi_process_start);
  FILETIME ct;
  FILETIME ut;
  FILETIME st;
  FILETIME et;
  GetProcessTimes(GetCurrentProcess(), &ct, &et, &st, &ut);
  *utime = filetime_msecs(&ut);
  *stime = filetime_msecs(&st);
  PROCESS_MEMORY_COUNTERS info;
  GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info));
  *current_rss    = (size_t)info.WorkingSetSize;
  *peak_rss       = (size_t)info.PeakWorkingSetSize;
  *current_commit = (size_t)info.PagefileUsage;
  *peak_commit    = (size_t)info.PeakPagefileUsage;
  *page_faults    = (size_t)info.PageFaultCount;  
}

#elif !defined(__wasi__) && (defined(__unix__) || defined(__unix) || defined(unix) || defined(__APPLE__) || defined(__HAIKU__))
#include <stdio.h>
#include <unistd.h>
#include <sys/resource.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#if defined(__HAIKU__)
#include <kernel/OS.h>
#endif

static mi_msecs_t timeval_secs(const struct timeval* tv) {
  return ((mi_msecs_t)tv->tv_sec * 1000L) + ((mi_msecs_t)tv->tv_usec / 1000L);
}

static void mi_stat_process_info(mi_msecs_t* elapsed, mi_msecs_t* utime, mi_msecs_t* stime, size_t* current_rss, size_t* peak_rss, size_t* current_commit, size_t* peak_commit, size_t* page_faults)
{
  *elapsed = _mi_clock_end(mi_process_start);
  struct rusage rusage;
  getrusage(RUSAGE_SELF, &rusage);
  *utime = timeval_secs(&rusage.ru_utime);
  *stime = timeval_secs(&rusage.ru_stime);
#if !defined(__HAIKU__)
  *page_faults = rusage.ru_majflt;
#endif
  // estimate commit using our stats
  *peak_commit    = (size_t)(mi_atomic_loadi64_relaxed((_Atomic(int64_t)*)&_mi_stats_main.committed.peak));
  *current_commit = (size_t)(mi_atomic_loadi64_relaxed((_Atomic(int64_t)*)&_mi_stats_main.committed.current));
  *current_rss    = *current_commit;  // estimate 
#if defined(__HAIKU__)
  // Haiku does not have (yet?) a way to
  // get these stats per process
  thread_info tid;
  area_info mem;
  ssize_t c;
  get_thread_info(find_thread(0), &tid);
  while (get_next_area_info(tid.team, &c, &mem) == B_OK) {
    *peak_rss += mem.ram_size;
  }
  *page_faults = 0;
#elif defined(__APPLE__)
  *peak_rss = rusage.ru_maxrss;         // BSD reports in bytes
  struct mach_task_basic_info info;
  mach_msg_type_number_t infoCount = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &infoCount) == KERN_SUCCESS) {
    *current_rss = (size_t)info.resident_size;
  }
#else
  *peak_rss = rusage.ru_maxrss * 1024;  // Linux reports in KiB
#endif  
}

#else
#ifndef __wasi__
// WebAssembly instances are not processes
#pragma message("define a way to get process info")
#endif

static void mi_stat_process_info(mi_msecs_t* elapsed, mi_msecs_t* utime, mi_msecs_t* stime, size_t* current_rss, size_t* peak_rss, size_t* current_commit, size_t* peak_commit, size_t* page_faults)
{
  *elapsed = _mi_clock_end(mi_process_start);
  *peak_commit    = (size_t)(mi_atomic_loadi64_relaxed((_Atomic(int64_t)*)&_mi_stats_main.committed.peak));
  *current_commit = (size_t)(mi_atomic_loadi64_relaxed((_Atomic(int64_t)*)&_mi_stats_main.committed.current));
  *peak_rss    = *peak_commit;
  *current_rss = *current_commit;
  *page_faults = 0;
  *utime = 0;
  *stime = 0;
}
#endif


mi_decl_export void mi_process_info(size_t* elapsed_msecs, size_t* user_msecs, size_t* system_msecs, size_t* current_rss, size_t* peak_rss, size_t* current_commit, size_t* peak_commit, size_t* page_faults) mi_attr_noexcept
{
  mi_msecs_t elapsed = 0;
  mi_msecs_t utime = 0;
  mi_msecs_t stime = 0;
  size_t current_rss0 = 0;
  size_t peak_rss0 = 0;
  size_t current_commit0 = 0;
  size_t peak_commit0 = 0;
  size_t page_faults0 = 0;  
  mi_stat_process_info(&elapsed,&utime, &stime, &current_rss0, &peak_rss0, &current_commit0, &peak_commit0, &page_faults0);
  if (elapsed_msecs!=NULL)  *elapsed_msecs = (elapsed < 0 ? 0 : (elapsed < (mi_msecs_t)PTRDIFF_MAX ? (size_t)elapsed : PTRDIFF_MAX));
  if (user_msecs!=NULL)     *user_msecs     = (utime < 0 ? 0 : (utime < (mi_msecs_t)PTRDIFF_MAX ? (size_t)utime : PTRDIFF_MAX));
  if (system_msecs!=NULL)   *system_msecs   = (stime < 0 ? 0 : (stime < (mi_msecs_t)PTRDIFF_MAX ? (size_t)stime : PTRDIFF_MAX));
  if (current_rss!=NULL)    *current_rss    = current_rss0;
  if (peak_rss!=NULL)       *peak_rss       = peak_rss0;
  if (current_commit!=NULL) *current_commit = current_commit0;
  if (peak_commit!=NULL)    *peak_commit    = peak_commit0;
  if (page_faults!=NULL)    *page_faults    = page_faults0;
}

mi_decl_export void mi_stats_mallinfo(mallinfo_t *minfo) mi_attr_noexcept
{
  if (minfo == NULL) return;
  mi_stats_merge_from(mi_stats_get_default());

  minfo->reserved = _mi_stats_main.reserved.allocated;
  minfo->mmap_calls = _mi_stats_main.mmap_calls.count;

  mi_stat_count_t total = { 0, 0, 0, 0 };
  mi_stat_add(&total, &_mi_stats_main.normal, 1);
  mi_stat_add(&total, &_mi_stats_main.large, 1);
  mi_stat_add(&total, &_mi_stats_main.huge, 1);

  minfo->allocated = total.allocated;
  minfo->freed = total.freed;
}

