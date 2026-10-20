#pragma once
/* Host stand-in: every block ends right at a no-access page, so writing or
 * reading past the end crashes at once (like Electric Fence). */
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <stdint.h>
#define MALLOC_CAP_SPIRAM 0
static void *ef_malloc(size_t n)
{
    size_t pg = 4096, data = (n + 16 + pg - 1) / pg * pg;
    uint8_t *m = mmap(NULL, data + pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return NULL;
    mprotect(m + data, pg, PROT_NONE);
    uint8_t *p = m + data - n;
    /* header just before p is inside the mapping only if there's room; keep size in the map start */
    ((size_t *)m)[0] = data + pg;
    ((uint8_t **)m)[1] = p;
    memset(p, 0xA5, n);                 /* catch reads of uninitialised memory as garbage */
    return p;
}
static void *ef_calloc(size_t n, size_t s)
{
    if (s && n > SIZE_MAX / s) return NULL;
    void *p = ef_malloc(n * s);
    if (p) memset(p, 0, n * s);
    return p;
}
static void ef_free(void *p)
{
    if (!p) return;
    uint8_t *m = (uint8_t *)((uintptr_t)p & ~(uintptr_t)4095);
    /* p may sit past the first page: walk back to the header */
    while (((uint8_t **)m)[1] != p) m -= 4096;
    munmap(m, ((size_t *)m)[0]);
}
#define heap_caps_malloc(n, c) ef_malloc(n)
#define heap_caps_calloc(n, s, c) ef_calloc(n, s)
#define free(p) ef_free(p)
