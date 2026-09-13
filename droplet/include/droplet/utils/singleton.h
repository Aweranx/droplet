#pragma once

#include <type_traits>

template <typename T>
class Singleton {
 public:
  static T& GetInstance() noexcept(std::is_nothrow_constructible_v<T>) {
    static T instance;
    return instance;
  }

  Singleton(const Singleton&) = delete;
  Singleton& operator=(const Singleton&) = delete;
  Singleton(Singleton&&) = delete;
  Singleton& operator=(Singleton&&) = delete;

 protected:
  Singleton() = default;
  ~Singleton() = default;
};
