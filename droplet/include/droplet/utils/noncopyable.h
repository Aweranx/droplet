#pragma once

class Noncopyable {
 protected:
  constexpr Noncopyable() noexcept = default;
  ~Noncopyable() = default;

  Noncopyable(const Noncopyable&) = delete;
  Noncopyable& operator=(const Noncopyable&) = delete;
  Noncopyable(Noncopyable&&) = delete;
  Noncopyable& operator=(Noncopyable&&) = delete;
};

class MoveOnly {
 protected:
  constexpr MoveOnly() noexcept = default;
  ~MoveOnly() = default;

  MoveOnly(const MoveOnly&) = delete;
  MoveOnly& operator=(const MoveOnly&) = delete;
};