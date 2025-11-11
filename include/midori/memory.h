#ifndef MIDORI_MEMORY_H
#define MIDORI_MEMORY_H

#include <SDL3/SDL.h>
#include <cstdlib>
#include <cstring>
#include <tracy/Tracy.hpp>

namespace Midori {
extern "C" {

void *Malloc(size_t size) {
  auto *mem = std::malloc(size);
  TracyAlloc(mem, size);
  return mem;
};

void *Calloc(size_t nmemb, size_t size) {
  auto *mem = std::calloc(nmemb, size);
  TracyAlloc(mem, size);
  return mem;
};

void *Realloc(void *mem, size_t size) {
  TracyFree(mem);
  auto *new_mem = std::realloc(mem, size);
  TracyAlloc(new_mem, size);
  return new_mem;
};

void Free(void *mem) {
  TracyFree(mem);
  std::free(mem);
}
}
}; // namespace Midori

void *operator new(std ::size_t count) {
  auto *ptr = Midori::Malloc(count);
  return ptr;
}
void operator delete(void *ptr) noexcept { Midori::Free(ptr); }

#endif