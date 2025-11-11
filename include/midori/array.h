#ifndef MIDORI_DYNARRAY_H
#define MIDORI_DYNARRAY_H

#include <SDL3/SDL.h>
#include <algorithm>

namespace Midori {
template <typename T> class Array {
private:
  T *data_ = nullptr;
  size_t length_ = 0;
  size_t capacity_ = 0;

public:
  // TODO: add move semantics
  // Array(const Array &) = delete;
  // Array(Array &&) = delete;
  // Array &operator=(const Array &) = delete;
  // Array &operator=(Array &&) = delete;

  Array() = default;

  ~Array() {
    SDL_free(data_);
    data_ = nullptr;
    length_ = 0;
    capacity_ = 0;
  }

  void Reserve(const size_t capacity) {
    SDL_assert(capacity && "Not implemented");
    if (data_ == nullptr) {
      data_ = static_cast<T *>(SDL_malloc(capacity * sizeof(T)));
    } else {
      data_ = static_cast<T *>(SDL_realloc(data_, capacity * sizeof(T)));
    }
    capacity_ = capacity;
    length_ = std::min(length_, capacity);
    SDL_assert(data_);
  }

  void Resize(const size_t length) {
    if (length > capacity_) {
      Reserve(length);
    }

    length_ = length;
  }

  [[nodiscard]] bool Empty() const { return length_ == 0U; }

  [[nodiscard]] size_t Length() const { return length_; }

  [[nodiscard]] size_t Capacity() const { return capacity_; }

  [[nodiscard]] T Get(const size_t index) const {
    SDL_assert(index < length_ && "Out of bound");
    return data_[index];
  }

  void Push(const T &element) {
    if (capacity_ == length_) {
      Reserve(capacity_ * 2);
    }
    data_[length_] = element;
    length_++;
  }

  void Pop() {
    if (Empty()) {
      return;
    }
    length_--;
    if (capacity_ == length_ * 2) {
      Reserve(length_);
    }
  }

  void Erase(size_t index) { SDL_assert_always(false && "Not implemented"); }
  void Insert(size_t index, const T *element) {
    SDL_assert_always(false && "Not implemented");
  }
};

} // namespace Midori

#endif