#pragma once

// #include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>

namespace toyqueue {

enum class q_loc_status : uint8_t { empty, busy, not_empty };

template <std::movable T>
class fix_cap_queue {
 public:
  using value_t = T;
  using status = q_loc_status;

 private:
  using index_t = size_t;

  struct location {
    std::optional<value_t> data;
    std::atomic<status> flag{status::empty};
  };
  using container_t = std::vector<location>;

  container_t array;
  std::atomic<index_t> head;
  std::atomic<index_t> tail;
  const size_t cap;

  struct flag_guard {
    std::atomic<status>& flag;
    status set_to{};
    ~flag_guard() {
      flag.store(set_to, std::memory_order_release);
    }
  };

  struct clear_data_guard {
    std::optional<value_t>& data;
    bool disable{false};
    ~clear_data_guard() {
      if (!disable) {
        data = std::nullopt;
      }
    }
  };

 public:
  fix_cap_queue(size_t log_cap) : array(to_cap(log_cap)), cap{to_cap(log_cap)} {}

  /** @warning validity not guaranteed under concurrency;
               应在有锁的情形下使用, 或在生产者已停止时用作判定结束的谓词;
               若无 push 操作发生 (生产者已停止), empty() 返回 true, 则队列一定为空;
               返回 false 则队列仍可能因为并发的 pop 变为空;
   */
  bool empty() const noexcept {
    return empty_(head.load(std::memory_order_relaxed), tail.load(std::memory_order_relaxed));
  }

  bool full() const noexcept {
    return full_(head.load(std::memory_order_relaxed), tail.load(std::memory_order_relaxed));
  }

  /**
   @return optional<value_t>, 尝试 pop 失败时返回 nullopt;
   @note 当且仅当 pop 成功时, 通过 CAS 原子地移动 head 指针;
         pop 失败当且仅当尝试 CAS 前看到的队列为空 (from the view of this thread);
         自旋地等到 data location 的 flag 为 not_empty, 才会 pop 并返回 front;
         当 data location 的 flag 置为 empty 时 (memory_order_release), pop 完成;
         应尽可能保证 T 类型 移动构造/赋值 nothrow, 接收返回值时发生异常会丢失该数据, 但队列仍有效;
   */
  std::optional<value_t> try_pop() noexcept(std::is_nothrow_move_constructible_v<T>) {
    index_t cur_head = head.load(std::memory_order_relaxed);
    bool not_empty{};
    while ((not_empty = !empty_(cur_head, tail.load(std::memory_order_relaxed))) &&
           !head.compare_exchange_weak(cur_head, next_index(cur_head), std::memory_order_relaxed)) {
      std::this_thread::yield();
    }
    if (!not_empty) {
      return std::nullopt;
    }
    location& loc = array[loc_index(cur_head)];
    status expected = status::not_empty;
    while (!loc.flag.compare_exchange_weak(expected, status::busy, std::memory_order_acquire,
                                           std::memory_order_relaxed)) {
      expected = status::not_empty;
      std::this_thread::yield();
    }
    flag_guard fg{.flag = loc.flag, .set_to = status::empty};
    clear_data_guard dg{loc.data};
    return std::move(loc.data);
  }

  /**
   @return bool, 尝试 push 失败时返回 false, value 状态不变;
   @note 当且仅当 push 成功时, 通过 CAS 原子地移动 tail 指针;
         push 失败当且仅当尝试 CAS 前看到的队列为满 (from the view of this thread);
         自旋地等到 data location 的 flag 为 empty, 才会 push 写入, 并返回 true;
         当 data location 的 flag 置为 not_empty 时 (memory_order_release), push 完成;
         应尽可能保证 T 类型 移动构造/赋值 nothrow, push 构造时发生异常会丢失数据, 但队列仍有效;
   */
  template <typename U>
    requires requires(std::optional<value_t> data, U&& value) { data = std::forward<U>(value); }
  bool try_push(U&& value) noexcept(std::is_nothrow_move_constructible_v<T>) {
    index_t cur_tail = tail.load(std::memory_order_relaxed);
    bool not_full{};
    while ((not_full = !full_(head.load(std::memory_order_relaxed), cur_tail)) &&
           !tail.compare_exchange_weak(cur_tail, next_index(cur_tail), std::memory_order_relaxed)) {
      std::this_thread::yield();
    }
    if (!not_full) {
      return false;
    }
    location& loc = array[loc_index(cur_tail)];
    status expected = status::empty;
    while (!loc.flag.compare_exchange_weak(expected, status::busy, std::memory_order_acquire,
                                           std::memory_order_relaxed)) {
      expected = status::empty;
      std::this_thread::yield();
    }
    flag_guard fg{.flag = loc.flag, .set_to = status::not_empty};
    clear_data_guard dg{loc.data};
    loc.data = std::forward<U>(value);
    dg.disable = true;
    return true;
  }

 private:
  index_t next_index(index_t idx) const noexcept {
    return idx + (index_t)1;
  }

  size_t loc_index(index_t index) const noexcept {
    return index & (cap - (size_t)1);
  }

  bool empty_(index_t head, index_t tail) const noexcept {
    return head == tail;
  }

  bool full_(index_t head, index_t tail) const noexcept {
    return head + cap == tail;
  }

  static size_t to_cap(size_t log_cap) {
    static constexpr size_t max_log_cap = 40;
    if (log_cap > max_log_cap) {
      // spdlog::warn(std::format("log_cap received {}, restricted to {}", log_cap, max_log_cap));
      log_cap = max_log_cap;
    }
    return (size_t)1 << log_cap;
  }
};

template <std::movable T>
class naive_fix_cap_queue {
 public:
  using value_t = T;
  using container_t = std::vector<std::optional<T>>;

  container_t array;
  size_t head{0};
  size_t tail{0};
  const size_t cap;

  naive_fix_cap_queue(size_t cap) : array(cap + 1), cap{cap} {};

  bool empty() const noexcept {
    return head == tail;
  }

  bool full() const noexcept {
    return head == next_index(tail);
  }

  void push(T value) noexcept(std::is_nothrow_move_constructible_v<T>) {
    array[tail] = std::move(value);
    tail = next_index(tail);
    if (tail == head) {
      head = next_index(head);
    }
  }

  T const& front() const noexcept {
    return array[head];
  }

  T& front() noexcept {
    return array[head];
  }

  std::optional<T> pop() noexcept {
    std::optional<T> t = std::move(array[head]);
    array[head] = std::nullopt;
    head = next_index(head);
    return t;
  }

 private:
  size_t next_index(size_t index) const noexcept {
    return index == cap ? 0 : index + 1;
  }
};

};  // namespace toyqueue
