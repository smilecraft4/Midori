#pragma once

#include <stdexcept>
#include <vector>

#include "memory.h"

namespace Midori {

template <class T, class A = std::allocator<T>>
class ring_array {
   public:
    using allocator_type = A;
    using value_type = T;
    using reference = T&;
    using const_reference = const T&;
    using difference_type = A::difference_type;
    using size_type = A::size_type;

   private:
    T* buf_;
    size_type head_{};
    size_type size_{};
    size_type max_size_{};

   public:
    class iterator {
        ring_array<T, A>& arr_;
        size_type index_{};

       public:
        using iterator_category = std::random_access_iterator_tag;
        using pointer = T*;

        iterator(ring_array<T>& arr) : arr_(arr) {}
        iterator(ring_array<T>& arr, size_type index) : arr_(arr), index_(index) {}
        iterator(const iterator&) = default;
        ~iterator() = default;

        iterator& operator=(const iterator&) = default;
        bool operator==(const iterator& other) const { return arr_ == other.arr_ && index_ == other.index_; }
        bool operator!=(const iterator& other) const { return arr_ != other.arr_ || index_ != other.index_; }
        // bool operator<(const iterator&) const;   // optional
        // bool operator>(const iterator&) const;   // optional
        // bool operator<=(const iterator&) const;  // optional
        // bool operator>=(const iterator&) const;  // optional

        iterator& operator++() {
            index_++;
            return *this;
        }
        // iterator operator++(int);                               // optional
        // iterator& operator--();                                 // optional
        // iterator operator--(int);                               // optional
        // iterator& operator+=(size_type);                        // optional;,k
        iterator operator+(size_type offset) const {
            iterator tmp(*this);
            tmp.index_ += offset;
            return tmp;
        }
        // friend iterator operator+(size_type, const iterator&);  // optional
        // iterator& operator-=(size_type);                        // optional
        // iterator operator-(size_type) const;                    // optional
        // difference_type operator-(iterator) const;              // optional

        reference operator*() const { return &arr_[index_]; }
        pointer operator->() const { return *arr_[index_]; }
        // reference operator[](size_type) const;  // optional
    };
    class const_iterator {
       private:
        ring_array<T>& arr_;
        std::size_t index_{};

       public:
        using iterator_category = std::random_access_iterator_tag;  // or another tag
        using pointer = const T*;

        const_iterator(ring_array<T>& arr) : arr_(arr) {}
        const_iterator(ring_array<T>& arr, size_type index) : arr_(arr), index_(index) {}
        const_iterator(const const_iterator&) = default;
        const_iterator(const iterator& other) : arr_(other.arr_), index_(other.index_) {}
        ~const_iterator() = default;

        const_iterator& operator=(const const_iterator&) = default;
        bool operator==(const const_iterator& other) const { return arr_ == other.arr_ && index_ == other.index_; }
        bool operator!=(const const_iterator& other) const { return arr_ != other.arr_ || index_ != other.index_; }
        // bool operator<(const const_iterator&) const;   // optional
        // bool operator>(const const_iterator&) const;   // optional
        // bool operator<=(const const_iterator&) const;  // optional
        // bool operator>=(const const_iterator&) const;  // optional

        const_iterator& operator++() {
            index_++;
            return *this;
        }
        // const_iterator operator++(int);                                     // optional
        // const_iterator& operator--();                                       // optional
        // const_iterator operator--(int);                                     // optional
        // const_iterator& operator+=(size_type);                              // optional
        // const_iterator operator+(size_type) const;                          // optional
        // friend const_iterator operator+(size_type, const const_iterator&);  // optional
        // const_iterator& operator-=(size_type);                              // optional
        // const_iterator operator-(size_type) const;                          // optional
        // difference_type operator-(const_iterator) const;                    // optional

        const_reference operator*() const { return *arr_[index_]; }
        pointer operator->() const { return &arr_[index_]; }
        // reference operator[](size_type) const;  // optional
    };

    using reverse_iterator = std::reverse_iterator<iterator>;              // optional
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;  // optional

    ring_array(size_type max_size) : max_size_(max_size) {
        buf_ = (T*)_aligned_malloc(max_size * sizeof(T), alignof(T));
        assert(buf_);
    }
    ring_array(const ring_array&) = default;
    ~ring_array() {
        clear();
        _aligned_free(buf_);
    };

    ring_array& operator=(const ring_array&) = default;
    bool operator==(const ring_array& other) const {
        return buf_ == other.buf_ && max_size_ == other.max_size_ && size_ == other.size_ && head_ == other.head_;
    }
    bool operator!=(const ring_array& other) const {
        return buf_ != other.buf_ || max_size_ != other.max_size_ || size_ != other.size_ || head_ != other.head_;
    }
    // bool operator<(const ring_array&) const;   // optional
    // bool operator>(const ring_array&) const;   // optional
    // bool operator<=(const ring_array&) const;  // optional
    // bool operator>=(const ring_array&) const;  // optional

    iterator begin() { return iterator(*this); }
    const_iterator begin() const { return const_iterator(*this); }
    // const_iterator cbegin() const;
    iterator end() { return iterator(*this, size_); }
    const_iterator end() const { return const_iterator(*this, size_); }
    // const_iterator cend() const;
    // reverse_iterator rbegin();               // optional
    // const_reverse_iterator rbegin() const;   // optional
    // const_reverse_iterator crbegin() const;  // optional
    // reverse_iterator rend();                 // optional
    // const_reverse_iterator rend() const;     // optional
    // const_reverse_iterator crend() const;    // optional

    // void reserve(syze_type max_size);
    // void resize(syze_type size);

    reference front() { return buf_[head_]; }
    const_reference front() const { return &buf_[head_]; }
    reference back() {
        const size_type i = (head_ + size()) % max_size();
        return buf_[i];
    }
    const_reference back() const {
        const size_type i = (head_ + size()) % max_size();
        return buf_[i];
    }
    // template <class... Args>
    // void emplace_front(Args&&...);  // optional
    template <class... Args>
    void emplace_back(Args&&... args) {
        const size_type i = (head_ + size()) % max_size();
        if (size() == max_size()) {
            std::destroy_at(&buf_[i]);
            head_++;
        } else {
            size_++;
        }
        std::construct_at(&buf_[i], std::forward<Args>(args)...);
    }
    // void push_front(const T&);                    // optional
    // void push_front(T&&);                         // optional
    void push_back(const T& elem) {
        const size_type i = (head_ + size()) % max_size();
        if (size() == max_size()) {
            std::destroy_at(&buf_[i]);
            buf_[i] = elem;
            head_++;
        } else {
            size_++;
        }
        std::construct_at(&buf_[i], elem);
    }
    void push_back(T&& elem) {
        const size_type i = (head_ + size()) % max_size();
        if (size() == max_size()) {
            std::destroy_at(&buf_[i]);
            buf_[i] = elem;
            head_++;
        } else {
            size_++;
        }
        std::construct_at(&buf_[i], elem);
    }
    // void pop_front()
    void pop_back() {
        assert(size_ >= 0);
        const std::size_t i = (head_ + size()) % max_size();
        std::destroy_at(&buf_[i]);
        size_--;
    }
    reference operator[](size_type index) { return at(index); }
    const_reference operator[](size_type index) const { return at(index); }
    reference at(size_type index) {
        const std::size_t i = (head_ + index) % max_size();
        return buf_[i];
    }
    const_reference at(size_type index) const {
        const std::size_t i = (head_ + index) % max_size();
        return buf_[i];
    }

    // template <class... Args>
    // iterator emplace(const_iterator, Args&&...);     // optional
    // iterator insert(const_iterator, const T&);       // optional
    // iterator insert(const_iterator, T&&);            // optional
    // iterator insert(const_iterator, size_type, T&);  // optional
    // template <class iter>
    // iterator insert(const_iterator, iter, iter);                // optional
    // iterator insert(const_iterator, std::initializer_list<T>);  // optional
    // iterator erase(const_iterator it);
    // iterator erase(const_iterator, const_iterator);             // optional
    void truncate(size_type size) {
        if (size >= size_) {
            return;
        }
        for (size_type index = size; index < size_; index++) {
            const size_type i = (head_ + index) % max_size();
            std::destroy_at(&buf_[i]);
        }
        size_ = size;
    }
    void clear() {
        for (size_type index = 0; index < size(); index++) {
            const size_type i = (head_ + index) % max_size();
            std::destroy_at(&buf_[i]);
        }
        size_ = 0;
        head_ = 0;
    }
    // template <class iter>
    // void assign(iter, iter);                // optional
    // void assign(std::initializer_list<T>);  // optional
    // void assign(size_type, const T&);       // optional

    void swap(ring_array& other) {
        std::swap(buf_, other.buf_);
        std::swap(size_, other.size_);
        std::swap(max_size_, other.max_size_);
        std::swap(head_, other.head_);
    }
    size_type size() const { return size_; }
    size_type max_size() const { return max_size_; };
    bool empty() const { return size_ == 0; }

    // A get_allocator() const;  // optional
};

}  // namespace Midori