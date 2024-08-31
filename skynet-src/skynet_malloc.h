#ifndef skynet_malloc_h
#define skynet_malloc_h

#include <stddef.h>
// malloc()函数保留指定字节数的内存块。并且，它返回一个可以转换为任何形式指针的指针. malloc(nsize)
#define skynet_malloc malloc
// 函数malloc()分配内存并且保持内存未初始化，而calloc()函数分配内存并且将所有位初始化为零. calloc(n, size);
#define skynet_calloc calloc
// 如果动态分配的内存不足或超过要求，您可以使用该函数更改先前分配的内存的大小realloc(). realloc(ptr, nsize)
#define skynet_realloc realloc
// 释放内存
#define skynet_free free
#define skynet_memalign memalign
#define skynet_aligned_alloc aligned_alloc
#define skynet_posix_memalign posix_memalign

void *skynet_malloc(size_t sz);

void *skynet_calloc(size_t nmemb, size_t size);

void *skynet_realloc(void *ptr, size_t size);

void skynet_free(void *ptr);

char *skynet_strdup(const char *str);

void *skynet_lalloc(void *ptr, size_t osize, size_t nsize);    // use for lua
void *skynet_memalign(size_t alignment, size_t size);

void *skynet_aligned_alloc(size_t alignment, size_t size);

int skynet_posix_memalign(void **memptr, size_t alignment, size_t size);

#endif
