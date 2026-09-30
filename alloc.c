/* The heap of cowsay.wasm, in place of wasi-libc's dlmalloc.
 *
 * The program prints one message and exits, so memory is never given back.
 * dlmalloc would add about 11 kB to the binary for reuse that never pays.
 * Only the wasm build links this file; the native build keeps the system allocator.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE 65536

/* malloc must return memory aligned for any type: 16 bytes on wasm32, for long double.
 * The size header takes one such unit. */
#define ALIGN 16

extern unsigned char __heap_base;

static unsigned char *top = &__heap_base;

/* The latest block, which realloc can grow in place. */
static unsigned char *last;

static size_t *size_of(unsigned char *block) { return (size_t *)(block - ALIGN); }

/* Make [block, block + n) addressable, growing linear memory as needed. */
static int reserve(unsigned char *block, size_t n) {
  if (n > SIZE_MAX - (uintptr_t)block) return 0;
  size_t end = (uintptr_t)block + n;
  size_t have = __builtin_wasm_memory_size(0) * PAGE_SIZE;
  if (end <= have) return 1;
  return __builtin_wasm_memory_grow(0, (end - have - 1) / PAGE_SIZE + 1) != SIZE_MAX;
}

void *malloc(size_t n) {
  uintptr_t aligned = ((uintptr_t)top + ALIGN - 1) & ~(uintptr_t)(ALIGN - 1);
  unsigned char *block = (unsigned char *)aligned + ALIGN;
  if (!reserve(block, n)) {
    errno = ENOMEM;
    return NULL;
  }
  *size_of(block) = n;
  top = block + n;
  last = block;
  return block;
}

void free(void *p) { (void)p; }

void *calloc(size_t count, size_t size) {
  if (size && count > SIZE_MAX / size) {
    errno = ENOMEM;
    return NULL;
  }
  // Memory that realloc gave up by shrinking the latest block is not zero.
  void *p = malloc(count * size);
  if (p) memset(p, 0, count * size);
  return p;
}

void *realloc(void *p, size_t n) {
  unsigned char *block = p;
  if (!block) return malloc(n);
  if (block == last) {
    if (!reserve(block, n)) {
      errno = ENOMEM;
      return NULL;
    }
    *size_of(block) = n;
    top = block + n;
    return block;
  }
  size_t old = *size_of(block);
  if (n <= old) return block;
  unsigned char *moved = malloc(n);
  if (moved) memcpy(moved, block, old);
  return moved;
}
