#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
static inline void *heap_caps_aligned_alloc(size_t a, size_t n, int caps) { (void)caps; return aligned_alloc(a, (n + a - 1) / a * a); }
static inline void *heap_caps_calloc(size_t n, size_t s, int caps) { (void)caps; return calloc(n, s); }
static inline void *heap_caps_realloc(void *p, size_t n, int caps) { (void)caps; return realloc(p, n); }
static inline void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
