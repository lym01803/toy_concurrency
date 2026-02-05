#pragma once

#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <semaphore>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

#include "async_tool.h"
#include "toyqueue.h"

namespace playground {

class guarded_thread {
  std::thread t;

 public:
  guarded_thread(std::thread t) : t(std::move(t)) {}
  guarded_thread(const guarded_thread& other) = delete;
  guarded_thread(guarded_thread&& other) noexcept = default;
  guarded_thread& operator=(const guarded_thread& other) = delete;
  guarded_thread& operator=(guarded_thread&& other) noexcept {
    if (t.joinable()) {
      try {
        t.join();
      } catch (...) {
        std::terminate();
      }
    }
    t = std::move(other.t);
    return *this;
  }

  ~guarded_thread() {
    if (t.joinable()) {
      t.join();
    }
  }

  std::thread& operator*() noexcept {
    return t;
  }
  std::thread const& operator*() const noexcept {
    return t;
  }
  std::thread* operator->() noexcept {
    return &t;
  }
  std::thread const* operator->() const noexcept {
    return &t;
  }
  std::thread release() noexcept {
    return std::move(t);
  }
};

struct stoppable_cv {
  stoppable_cv() noexcept = default;

  bool is_stopped() const noexcept {
    return stoped.load();
  }

  void stop() noexcept {
    stoped = true;
    cv.notify_all();
  }

  std::condition_variable& operator*() noexcept {
    return cv;
  }
  std::condition_variable const& operator*() const noexcept {
    return cv;
  }
  std::condition_variable* operator->() noexcept {
    return &cv;
  }
  std::condition_variable const* operator->() const noexcept {
    return &cv;
  }

 private:
  std::condition_variable cv;
  std::atomic<bool> stoped{false};
};

struct default_id_generator {
  size_t current_id{};
  size_t operator()() noexcept {
    return ++current_id;
  }
};

struct default_timestamp_generator {
  using time_point_t = std::chrono::time_point<std::chrono::system_clock>;
  time_point_t operator()() noexcept {
    return std::chrono::system_clock::now();
  }
};

enum class sync_stream_read_write_status : std::uint8_t { empty, good };

template <std::movable T, std::invocable IDGen = default_id_generator,
          std::invocable TSGen = default_timestamp_generator>
  requires requires(T t) {
    t.serial_number = std::declval<IDGen>()();
    t.timestamp = std::declval<TSGen>()();
  } && std::is_default_constructible_v<IDGen> && std::is_default_constructible_v<TSGen>
class sync_stream {
  IDGen id_gen{};
  TSGen ts_gen{};
  std::mutex mutex;
  stoppable_cv cv;
  std::deque<T> queue;

 public:
  using msg_t = T;
  using status = sync_stream_read_write_status;  // template params independent

  template <typename U>
    requires requires(U&& data) { msg_t{std::forward<U>(data)}; }
  status write_sync(U&& data) {
    msg_t msg{std::forward<U>(data)};
    {
      std::lock_guard lock{mutex};
      msg.serial_number = id_gen();
      msg.timestamp = ts_gen();
      queue.push_back(std::move(msg));
    }
    cv->notify_one();
    return status::good;
  }

  status read_sync(T& data) {
    std::unique_lock lock{mutex};
    cv->wait(lock, [this]() { return !queue.empty() || cv.is_stopped(); });
    if (!queue.empty()) {
      get_front(data);
      return status::good;
    }
    return status::empty;
  }

  template <typename U>
    requires requires(U&& data) { msg_t{std::forward<U>(data)}; }
  status operator<<(U&& data) {
    return write_sync(std::forward<U>(data));
  }

  status operator>>(T& data) {
    return read_sync(data);
  }

  operator bool() const noexcept {
    return !queue.empty() || !cv.is_stopped();
  }

  void stop() {
    cv.stop();
  }

  template <typename Op, typename Executor>
  auto get_dispatcher(Op op, Executor& executor) {
    return async::dispatcher<msg_t, Op, sync_stream, Executor>{*this, executor};
  }

 private:
  void get_front(T& data) {
    if constexpr (std::is_nothrow_move_assignable_v<T>) {  // move assign
      data = std::move(queue.front());
      queue.pop_front();
    } else if constexpr (std::is_nothrow_copy_assignable_v<T>) {  // copy assign
      data = queue.front();
      queue.pop_front();
    } else if constexpr (std::is_copy_constructible_v<T> &&
                         std::is_swappable_v<T>) {  // copy and swap
      T front(queue.front());
      std::swap(data, front);
      queue.pop_front();
    } else {  // data may be invalid when exceptions occur
      data = std::move(queue.front());
      queue.pop_front();
    }
  }
};

template <std::movable F>
class runner {
  toyqueue::fix_cap_queue<F> queue;
  std::stop_source stop_source;
  std::counting_semaphore<> semaphore{0};
  guarded_thread th;

  void stop() {
    stop_source.request_stop();
    semaphore.release();
  }

  void drain() {
    if constexpr (async::cancellable<F>) {
      while (!queue.empty()) {
        auto task = queue.try_pop();
        if (task.has_value()) {
          task.value().cancel();
        }
      }
    } else {
    }
  }

  void run() {
    auto stop_token = stop_source.get_token();
    while (true) {
      semaphore.acquire();
      if (stop_token.stop_requested()) {
        drain();
        return;
      }
      auto task = queue.try_pop();
      if (task.has_value()) {
        task.value()();
      }
    }
  }

 public:
  runner(size_t log_cap = 16) : queue{log_cap}, th{std::thread{[this]() { run(); }}} {}

  ~runner() {
    stop();
  }

  template <typename U>
  void operator()(U&& task) {
    F f{std::forward<U>(task)};
    while (!queue.try_push(std::move(f))) {
      std::this_thread::yield();
    }
    semaphore.release();
  }
};

}  // namespace playground
