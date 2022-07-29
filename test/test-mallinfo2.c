#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <malloc.h>
#include <assert.h>

#define ALLOC_NUM 100

void test_mallinfo2(void)
{
  struct mallinfo2 mi2;

  mi2 = mallinfo2();

  assert(mi2.hblks);
  assert(mi2.hblkhd);
  assert(mi2.uordblks);
  assert(mi2.fordblks);
}

int main(void)
{
  int i;
  char* arr[ALLOC_NUM];

  for (i = 0; i < ALLOC_NUM; i++) {
    arr[i] = (char*)malloc((i + 1) * 1024);
    if (arr[i] == NULL) {
        fprintf(stderr, "Failed memory allocation\n");
        exit(1);
    }
  }

  for (i = 0; i < ALLOC_NUM; i++)
  {
    free(arr[i]);
  }

  test_mallinfo2();

  return 0;
}