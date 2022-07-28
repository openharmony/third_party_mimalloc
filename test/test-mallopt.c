#include <mimalloc.h>
#include <assert.h>

int main()
{
  assert(1 == mallopt(mi_option_show_errors, 1));
  assert(1 == mallopt(mi_option_show_errors, 0));

  assert(1 == mallopt(mi_option_show_stats, 1));
  assert(1 == mallopt(mi_option_show_stats, 0));

  assert(1 == mallopt(mi_option_verbose, 1));
  assert(1 == mallopt(mi_option_verbose, 0));

  assert(0 == mallopt(mi_option_verbose + 128, 1));

  return 0;
}