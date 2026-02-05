#pragma once

#include <algorithm>
#include <atomic>
#include <concepts>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <semaphore>
#include <thread>
#include <type_traits>
#include <utility>

namespace async {

template <typename Derived>
struct value_storage_base {
  std::exception_ptr e_ptr{nullptr};

  decltype(auto) get() & {
    if (e_ptr) {
      std::rethrow_exception(e_ptr);
    }
    return derived().get_impl();
  }

  decltype(auto) get() && {
    if (e_ptr) {
      std::rethrow_exception(e_ptr);
    }
    return std::move(derived()).get_impl();
  }

  template <typename F>
  void execute(F&& f) noexcept {
    try {
      derived().execute_impl(std::forward<F>(f));
    } catch (...) {
      e_ptr = std::current_exception();
    }
  }

 private:
  Derived& derived() {
    return static_cast<Derived&>(*this);
  }
};

template <typename T = void>
struct value_storage : public value_storage_base<value_storage<T>> {
  static_assert(!std::is_same_v<T, T>, "Invalid value_storage.");
  using type = T;

  void get_impl() noexcept {}
  template <typename F>
  void execute_impl(F&& f) {}
};

template <>
struct value_storage<void> : public value_storage_base<value_storage<void>> {
  using type = void;

  void get_impl() const noexcept {}
  template <std::invocable F>
  void execute_impl(F&& f) {
    std::forward<F>(f)();
  }
};

template <typename T>
struct value_storage<T&> : public value_storage_base<value_storage<T&>> {
  using type = T*;
  type value{nullptr};

  T& get_impl() const noexcept {
    return *value;
  }

  template <std::invocable F>
    requires std::convertible_to<std::invoke_result_t<F>, T&>
  void execute_impl(F&& f) {
    set(std::forward<F>(f)());
  }

  void set(T& _value) noexcept {
    value = std::addressof(_value);
  }
};

template <typename T>
  requires std::is_trivially_constructible_v<T>
struct value_storage<T> : public value_storage_base<value_storage<T>> {
  using type = T;
  type value{};

  T& get_impl() & noexcept {  // return T& instead of T, to avoid copy and support move
    return value;
  }

  T get_impl() && noexcept {
    return std::move(value);
  }

  template <std::invocable F>
    requires std::convertible_to<std::invoke_result_t<F>, T>
  void execute_impl(F&& f) {
    set(std::forward<F>(f)());
  }

  void set(T _value) noexcept(std::is_nothrow_move_assignable_v<T>) {
    value = std::move(_value);
  }
};

template <typename T>
  requires(!std::is_trivially_constructible_v<T>)
struct value_storage<T> : public value_storage_base<value_storage<T>> {
  using type = std::optional<T>;
  type value;

  T& get_impl() & noexcept {
    return value.value();
  }

  T get_impl() && noexcept {
    return std::move(value.value());
  }

  template <std::invocable F>
    requires std::convertible_to<std::invoke_result_t<F>, T>
  void execute_impl(F&& f) {
    set(std::forward<F>(f)());
  }

  void set(T _value) noexcept(std::is_nothrow_move_constructible_v<T> &&
                              std::is_nothrow_move_assignable_v<T>) {
    value = std::move(_value);
  }
};

/**
 * @brief Concept: 约束 Op 是否可以作用于 Stream 和 T
 */
template <typename Stream, typename T, typename Op>
concept stream_with_op = requires(Op op, Stream& s, T& data) { op(s, data); };

/**
 * @brief Concept: 约束 Stream 是否支持读取 T (operator>>)
 */
template <typename Stream, typename T>
concept istream_with = requires(Stream& is, T& data) { is >> data; };

/**
 * @brief Concept: 约束 Stream 是否支持写入 T (operator<<)
 */
template <typename Stream, typename T>
concept ostream_with = requires(Stream& os, T&& data) { os << std::forward<T>(data); };

struct read_stream {
  template <typename T>
  using data_type = T&;

  template <typename T, istream_with<T> Stream>
  auto operator()(Stream& is, T& data) {
    return is >> data;
  }
};

struct write_stream {
  template <typename T>
  using data_type = T;

  template <typename T, ostream_with<T> Stream>
  auto operator()(Stream& os, T&& data) {
    return os << std::forward<T>(data);
  }
};

/**
 * @brief 异步操作分发器
 * * 负责将具体的 IO 操作（流、数据、操作符）绑定到执行器（Executor）上。
 * * 通过生成 awaitable 对象，利用协程机制将操作打包为 `to_execute_t` 投递给 Executor。
 */
template <typename T, typename Op, stream_with_op<T, Op> Stream, typename Executor>
class dispatcher {
  Stream& stream;
  Executor& executor;

 public:
  struct awaitable;

  dispatcher(Stream& _stream, Executor& _executor) : stream{_stream}, executor{_executor} {}

  /**
   * @brief 生成等待对象
   * @return awaitable 当被 co_await 时，会触发异步执行
   */
  template <typename U>
  awaitable operator()(U&& data) {
    return awaitable{.stream = stream, .executor = executor, .data = {std::forward<U>(data)}};
  }
};

/**
 * @brief 内部执行包 (Design Note)
 * * 设计意图：为了提供一种**不做类型擦除**且**零开销**的执行上下文传递机制。
 * 它保留了 Op, Stream, Data 的具体类型信息，直接在 Executor 中通过泛型展开执行，避免了
 * std::function 等带来的虚函数或内存分配开销。
 */
template <typename T, typename Op, stream_with_op<T, Op> Stream>
struct to_execute_t {
  using data_t = Op::template data_type<T>;  // either T or T&

  Op* op;
  Stream* stream;
  T* data;

  using retval_t = decltype((*op)(*stream, *data));
  using storage_t = value_storage<retval_t>;

  storage_t* retval;
  std::coroutine_handle<> h;

  void operator()() {
    retval->execute(
        [this]() -> decltype(auto) { return (*op)(*stream, std::forward<data_t>(*data)); });
    h.resume();
  }

  void cancel() {
    if (h) {
      h.destroy();
    }
  }
};

template <typename T, typename Op, stream_with_op<T, Op> Stream, typename Executor>
struct dispatcher<T, Op, Stream, Executor>::awaitable {
  Stream& stream;
  Executor& executor;
  Op::template data_type<T> data;
  [[no_unique_address]] Op op{};

  using retval_t = decltype(op(stream, data));
  using storage_t = value_storage<retval_t>;
  [[no_unique_address]] storage_t retval;

  using _to_execute_t = to_execute_t<T, Op, Stream>;

  bool await_ready() const noexcept {
    return false;
  }

  void await_suspend(std::coroutine_handle<> h) {
    executor(_to_execute_t{.op = &op, .stream = &stream, .data = &data, .retval = &retval, .h = h});
  }

  retval_t await_resume() {
    return std::move(retval).get();
  }
};

template <typename T, istream_with<T> Stream, typename Executor>
using read_dispatcher = dispatcher<T, read_stream, Stream, Executor>;

template <typename T, ostream_with<T> Stream, typename Executor>
using write_dispatcher = dispatcher<T, write_stream, Stream, Executor>;

template <typename T>
concept cancellable = requires(T obj) { obj.cancel(); };

struct return_void_mixin {
  [[no_unique_address]] value_storage<void> retval;
  void return_void() const {}
};

template <typename T>
struct return_value_mixin {
  [[no_unique_address]] value_storage<T> retval;
  void return_value(T value) {
    retval.set(std::forward<T>(value));
  }
};

template <typename T>
using return_mixin =
    std::conditional_t<std::is_same_v<T, void>, return_void_mixin, return_value_mixin<T>>;

template <typename T>
struct co_task_awaitable;

/**
 * @brief 任务 Future 手柄
 * * 同步等待 task 返回结果: 基于 std::binary_semaphore (counting_semaphore) 实现阻塞/唤醒。
 * @note 仅保证**第一次**调用 wait() 有效（资源移动语义）。
 */
template <typename T>
struct task_future {
  struct state {
    std::atomic<bool> _done{false};
    [[no_unique_address]] value_storage<T> retval;

    void release() {
      _done.store(true, std::memory_order_release);
      _done.notify_all();
    }

    void acquire() {
      bool old = _done.load(std::memory_order_acquire);
      while (!old) {
        _done.wait(old, std::memory_order_relaxed);
        old = _done.load(std::memory_order_acquire);
      }
    }

    bool done() {
      return _done.load(std::memory_order_relaxed);
    }
  };
  std::shared_ptr<state> state_ptr = std::make_shared<state>();

  /**
   * @brief 阻塞当前线程直到任务完成并获取结果, 仅保证第一次调用有效
   */
  decltype(auto) get() {
    state_ptr->acquire();
    return std::move(state_ptr->retval).get();
  }

  /**
   * @brief 获取任务是否完成, 用于决定是否要阻塞获取结果.
   */
  bool done() const {
    return state_ptr->done();
  }
};

struct next_awaitable {
  std::coroutine_handle<>& next;
  bool await_ready() const noexcept {
    return !next;
  }
  auto await_suspend(std::coroutine_handle<> h) const noexcept {
    auto _next = next;
    next = nullptr;
    return _next;
  }
  void await_resume() const noexcept {}
};

/**
 * @brief 通用协程任务 (Lazy 模式)
 * * * **设计模式**：Lazy Execution。协程创建后处于暂停状态，必须显式启动。
 * * * **三种互斥的启动方式**：
 * 1. `detach()`: 分离执行, 无法获取 task 的结果;
 * 2. `get_future()`: 启动并返回 task_future，用于同步等待返回值;
 * 3. `co_await`: 在另一个协程中等待执行器异步执行并返回结果;
 * * * **注意**：这三种方式仅对处在 initial_suspend 之后、首次 resume 之前的对象有效。
 * co_task_with 对象只是启动器, 一旦选定一种方式启动, co_task_with 对象即失效.
 */
template <typename T = void>
struct co_task_with {
  using final_awaitable = next_awaitable;

  struct promise_type : public return_mixin<T> {
    std::coroutine_handle<> next{nullptr};

    ~promise_type() {
      if (next) {
        next.destroy();
      }
    }

    auto get_return_object() {
      return co_task_with{.co_handle = std::coroutine_handle<promise_type>::from_promise(*this),
                          .promise = *this};
    }
    auto initial_suspend() {
      return std::suspend_always{};
    }
    auto final_suspend() noexcept {
      return final_awaitable{next};
    }
    void unhandled_exception() {}
  };

  /**
   * @brief 分离任务: 启动协程
   */
  void detach() & {
    co_handle.resume();
  }

  /**
   * @brief 分离任务: 启动协程
   */
  void detach() && {
    return this->detach();
  }

  co_task_awaitable<T> wait() & {
    return co_task_awaitable<T>{*this};
  }

  co_task_awaitable<T> wait() && {
    return co_task_awaitable<T>{*this};
  }

  /**
   * @brief 协程等待, 支持直接 `co_await task;`。
   * * 行为: 等待执行器异步执行，任务完成后 resume 当前协程并返回结果。
   */
  decltype(auto) operator co_await() & {
    return this->wait();
  }

  /**
   * @brief 协程等待, 支持直接 `co_await task;`。
   * * 行为: 等待执行器异步执行，任务完成后 resume 当前协程并返回结果。
   */
  decltype(auto) operator co_await() && {
    return this->wait();
  }

  void hook_next(std::coroutine_handle<> h) noexcept {
    promise.next = h;
  }

  /**
   * @brief 启动协程并返回一个关联的 future。
   * @return task_future<T> 对象，可用于同步阻塞等待返回值（std::counting_semaphore）。
   */
  task_future<T> get_future() & {
    task_future<T> future;
    [this](task_future<T> future)
        -> co_task_with<> {  // copy a future here, valid until coroutine frame destroyed.
      try {
        if constexpr (std::is_void_v<T>) {
          co_await this->wait();  // this is valid until suspend
          future.state_ptr->release();
        } else {
          future.state_ptr->retval.set(co_await this->wait());  // this is valid until suspend
          future.state_ptr->release();
        }
      } catch (...) {
        future.state_ptr->retval.e_ptr = std::current_exception();
        future.state_ptr->release();
      }
    }(future).detach();
    return future;
  }

  task_future<T> get_future() && {
    return this->get_future();
  }

  std::coroutine_handle<promise_type> co_handle;
  promise_type& promise;
};

template <typename T>
struct co_task_awaitable {
  using task_t = co_task_with<T>;
  task_t::promise_type& promise;
  std::coroutine_handle<> task_co_handle;

  co_task_awaitable(task_t& _task)
      : promise(_task.promise), task_co_handle(_task.co_handle) {}

  bool await_ready() const noexcept {
    return false;
  }
  auto await_suspend(std::coroutine_handle<> h) const noexcept {
    promise.next = h;
    return task_co_handle;  // transfer to run co_handle.resume();
  }
  T await_resume() const noexcept { // task may be invalid here
    value_storage<T> value{std::move(promise.retval)};
    task_co_handle.destroy();
    return std::move(value).get();
  }
};

using co_task = co_task_with<>;

template <typename T>
struct yield_task_awaitable;

template <typename T>
  requires (!std::is_void_v<T>)
struct yield_task : protected co_task_with<T> {
  using base = co_task_with<T>;
  using yield_awaitable = next_awaitable;

  struct promise_type : public base::promise_type {
    bool returned{false};
    yield_task get_return_object() {
      return {base::promise_type::get_return_object(), *this};
    }
    auto yield_value(T value) {
      this->retval.set(std::forward<T>(value));
      return yield_awaitable{this->next};
    }
    void return_value(T value) {
      returned = true;
      base::promise_type::return_value(std::forward<T>(value));
    }
  };

  yield_task(const base& _base, promise_type& _promise) : base{_base}, promise{_promise} {}

  void cancel() {  // coroutine must be suspended when calling cancel
    if (!returned && this->co_handle != nullptr) {
      this->co_handle.destroy();
      this->co_handle = nullptr;
    }
  }

  auto wait() & {
    return yield_task_awaitable<T>{*this};
  }

  decltype(auto) operator co_await() & {
    return this->wait();
  }

  bool returned{false};
  promise_type& promise;

  friend yield_task_awaitable<T>;
};

template <typename T>
struct yield_task_awaitable {
  using task_t = yield_task<T>;
  task_t::promise_type& promise;
  std::coroutine_handle<> task_co_handle;
  bool& returned;

  yield_task_awaitable(task_t& _task)
      : promise(_task.promise), task_co_handle(_task.co_handle), returned{_task.returned} {}

  bool await_ready() const noexcept {
    return false;
  }
  auto await_suspend(std::coroutine_handle<> h) const noexcept {
    promise.next = h;
    return task_co_handle;  // transfer to run co_handle.resume();
  }
  T await_resume() const noexcept {  // task must be valid here
    value_storage<T> value{std::move(promise.retval)};
    if (promise.returned) {  // task_co_handle is final suspended
      returned = true;
      task_co_handle.destroy();
    }
    return std::move(value).get();
  }
};

struct cancel_mixin {
  std::function<void()> _cancel{[]() {}};
  void cancel() const {
    _cancel();
  }
};

template <typename R, typename... Args>
struct call_mixin {
  std::function<R(Args...)> call;
  R operator()(Args... args) {
    return call(std::forward<Args>(args)...);
  }
};

/**
 * @brief 可取消的函数包装器
 * * 封装了“执行逻辑”和“取消逻辑”的函数对象。
 * 用于提交给支持取消操作的 Executor。
 */
template <typename R, typename... Args>
struct cancellable_function : public cancel_mixin, public call_mixin<R, Args...> {
  using cancel_base = cancel_mixin;
  using call_base = call_mixin<R, Args...>;

  template <typename F, typename C>
  cancellable_function(F func, C cancel)
      : cancel_base{std::move(cancel)}, call_base{std::move(func)} {}

  template <typename F>
  cancellable_function(F func) : cancel_base{}, call_base{std::move(func)} {}
};

template <typename T, typename Op, stream_with_op<T, Op> Stream>
auto to_function(to_execute_t<T, Op, Stream> to_execute) {
  std::coroutine_handle<> h = to_execute.h;
  return cancellable_function<void>([f = std::move(to_execute)]() { f(); },  // execute
                                    [h]() {                                  // cancel
                                      if (h) {
                                        h.destroy();
                                      }
                                    });
}

template <typename Executor>
struct execute_by_awaitable {
  Executor& executor;
  bool await_ready() const noexcept {
    return false;
  }
  void await_suspend(std::coroutine_handle<> h) {
    executor(cancellable_function<void>{[=]() { h.resume(); }, [=]() { h.destroy(); }});
  }
  void await_resume() const noexcept {}
};

/**
 * @brief 切换执行上下文 (Awaitable Helper)
 * * 用法: `co_await async::execute_by(executor);`
 * * 行为: 挂起当前协程，将 resume 动作打包提交给目标 executor, 实现线程/上下文切换。
 */
template <typename Executor>
auto execute_by(Executor& executor) {
  return execute_by_awaitable{executor};
}

struct trivial_executor_t {
  void operator()(std::function<void()> f) const {
    std::thread th{[f = std::move(f)]() { f(); }};
    th.detach();
  }
};

/**
 * @brief 全局默认的简易执行器实例
 * * 行为: 对每个提交的任务启动一个 `std::thread` 并 detach，无线程池管理。
 */
constexpr trivial_executor_t trivial_executor{};

struct async_call_t {
  template <std::invocable F, typename Executor>
  struct awaitable {
    F f;
    Executor& executor;
    using ret_t = std::invoke_result_t<F>;
    [[no_unique_address]] value_storage<ret_t> retval;

    bool await_ready() const noexcept {
      return false;
    }
    void await_suspend(std::coroutine_handle<> h) {
      executor(cancellable_function<void>(
          [this, h, f = std::move(f)]() {
            retval.execute([&]() -> decltype(auto) { return f(); });  // sync call
            h.resume();
          },
          [h]() { h.destroy(); }));
    }
    ret_t await_resume() {
      return std::move(retval).get();
    }
  };

  template <std::invocable F, typename Executor>
  auto operator()(F f, Executor& executor) const {
    return awaitable<F, Executor>{.f = std::move(f), .executor = executor};
  }

  template <std::invocable F>
  auto operator()(F f) const {
    return awaitable<F, const trivial_executor_t>{.f = std::move(f), .executor = trivial_executor};
  }
};

/**
 * @brief 异步调用工具实例
 * * 用法: `co_await async::async_call(func, executor);`
 * * 行为: 将普通的可调用对象 func 包装为协程, 由 executor 异步执行.
 */
constexpr async_call_t async_call{};

template <typename A>
concept awaiter = requires(A obj, std::coroutine_handle<> h) {
  { obj.await_ready() } -> std::convertible_to<bool>;
  obj.await_suspend(h);
  obj.await_resume();
};

template <typename A>
concept has_member_co_await_operator = requires(A obj) {
  { std::forward<A>(obj).operator co_await() } -> awaiter;
};

template <typename A>
struct member_co_await_operator_retval : public std::false_type {};

template <has_member_co_await_operator A>
struct member_co_await_operator_retval<A> {
  using type = decltype(std::declval<A>().operator co_await());
};

template <typename A>
concept has_global_co_await_operator = requires(A obj) {
  { operator co_await(std::forward<A>(obj)) } -> awaiter;
};

template <typename A>
struct global_co_await_operator_retval : public std::false_type {};

template <has_global_co_await_operator A>
struct global_co_await_operator_retval<A> {
  using type = decltype(operator co_await(std::declval<A>()));
};

template <typename A>
concept has_promise_type = requires() { typename A::promise_type; };

template <typename A>
concept co_awaitable =
    has_member_co_await_operator<A> || has_global_co_await_operator<A> || awaiter<A>;

struct get_co_await_obj_t {
  template <typename A>
    requires has_member_co_await_operator<A>
  decltype(auto) operator()(A&& awaitable) const {
    return std::forward<A>(awaitable).operator co_await();
  }

  template <typename A>
    requires(!has_member_co_await_operator<A>) && has_global_co_await_operator<A>
  decltype(auto) operator()(A&& awaitable) const {
    return operator co_await(std::forward<A>(awaitable));
  }

  template <typename A>
    requires(!has_member_co_await_operator<A>) && (!has_global_co_await_operator<A>) && awaiter<A>
  decltype(auto) operator()(A&& awaitable) const {
    return std::forward<A>(awaitable);
  }
};

constexpr get_co_await_obj_t get_co_await_obj;

template <co_awaitable A>
struct co_awaitable_trait {
  using awaitable_t = decltype(get_co_await_obj(std::declval<A>()));
  using resume_t = decltype(std::declval<awaitable_t>().await_resume());
};

template <co_awaitable A>
struct pre_schedule_trait {
  using pre_t = std::conditional_t<has_promise_type<A>, A,
                                   co_task_with<typename co_awaitable_trait<A>::resume_t>>;
};

template <co_awaitable A>
struct post_schedule_trait {
  using post_t = std::conditional_t<has_promise_type<A>, A,
                                    co_task_with<typename co_awaitable_trait<A>::resume_t>>;
};

template <co_awaitable A>
struct extended_awaitable_trait : public co_awaitable_trait<A>,
                                  public pre_schedule_trait<A>,
                                  public post_schedule_trait<A> {};

template <typename A>
concept wrappable_awaitable = co_awaitable<A&> || co_awaitable<A&&>;

template <wrappable_awaitable A, typename Executor>
struct pre_scheduled_awaitable;

template <wrappable_awaitable A, typename Executor>
struct post_scheduled_awaitable;

template <typename Derived>
struct extended_base {
  template <typename Executor>
  decltype(auto) on(Executor&& executor) & {
    return pre_scheduled_awaitable<Derived&, Executor>{derived(), std::forward<Executor>(executor)};
  }

  template <typename Executor>
  decltype(auto) on(Executor&& executor) && {
    return pre_scheduled_awaitable<Derived, Executor>{std::move(derived()),
                                                      std::forward<Executor>(executor)};
  }

  template <typename Executor>
  decltype(auto) back_to(Executor&& executor) & {
    return post_scheduled_awaitable<Derived&, Executor>{derived(),
                                                        std::forward<Executor>(executor)};
  }

  template <typename Executor>
  decltype(auto) back_to(Executor&& executor) && {
    return post_scheduled_awaitable<Derived, Executor>{std::move(derived()),
                                                       std::forward<Executor>(executor)};
  }

  Derived& derived() {
    return static_cast<Derived&>(*this);
  }
};

template <wrappable_awaitable A>
struct extended_awaitable : public extended_base<extended_awaitable<A>> {
  using base = extended_base<extended_awaitable<A>>;
  using awaitable_t = A;

  A awaitable;

  extended_awaitable(A awaitable) : awaitable{std::forward<A>(awaitable)} {}

  decltype(auto) operator co_await() &
    requires co_awaitable<A&>
  {
    return get_co_await_obj(awaitable);
  }

  decltype(auto) operator co_await() &&
    requires co_awaitable<A&&>
  {
    return get_co_await_obj(std::forward<A>(awaitable));
  }
};

template <wrappable_awaitable A>
extended_awaitable<A> lift(A&& awaitable) {
  return {std::forward<A>(awaitable)};
}

template <wrappable_awaitable A, typename Executor>
struct pre_scheduled_awaitable : public extended_base<pre_scheduled_awaitable<A, Executor>> {
  using base = extended_base<pre_scheduled_awaitable<A, Executor>>;
  using awaitable_t = A;
  
  A awaitable;
  Executor executor;

  pre_scheduled_awaitable(A awaitable, Executor executor)
      : awaitable{std::forward<A>(awaitable)}, executor{std::forward<Executor>(executor)} {}

  decltype(auto) operator co_await() & {
    return get_co_await_obj(
        [](A& awaitble, Executor& executor) -> extended_awaitable_trait<A&>::pre_t {
          co_await execute_by(executor);
          co_return co_await awaitble;
        }(awaitable, executor));
  }

  decltype(auto) operator co_await() && {
    return get_co_await_obj(
        [](A awaitable, Executor executor) -> extended_awaitable_trait<A&&>::pre_t {
          co_await execute_by(std::forward<Executor>(executor));
          co_return co_await std::forward<A>(awaitable);
        }(std::forward<A>(awaitable), std::forward<Executor>(executor)));
  }
};

template <wrappable_awaitable A, typename Executor>
struct post_scheduled_awaitable : public extended_base<post_scheduled_awaitable<A, Executor>> {
  using base = extended_base<post_scheduled_awaitable<A, Executor>>;
  using awaitable_t = A;
  using retval_t = typename co_awaitable_trait<A>::resume_t;
  
  A awaitable;
  Executor executor;

  post_scheduled_awaitable(A awaitable, Executor executor)
      : awaitable{std::forward<A>(awaitable)}, executor{std::forward<Executor>(executor)} {}

  decltype(auto) operator co_await() & {
    return get_co_await_obj(
        [](A& awaitble, Executor& executor) -> extended_awaitable_trait<A&>::post_t {
          value_storage<retval_t> value;
          if constexpr (std::is_void_v<retval_t>) {
            co_await awaitble;
          } else {
            value.set(co_await awaitble);
          }
          co_await execute_by(executor);
          co_return std::move(value).get();
        }(awaitable, executor));
  }

  decltype(auto) operator co_await() && {
    return get_co_await_obj(
        [](A awaitable, Executor executor) -> extended_awaitable_trait<A&&>::post_t {
          value_storage<retval_t> value;
          if constexpr (std::is_void_v<retval_t>) {
            co_await std::forward<A>(awaitable);
          } else {
            value.set(co_await std::forward<A>(awaitable));
          }
          co_await execute_by(std::forward<Executor>(executor));
          co_return std::move(value).get();
        }(std::forward<A>(awaitable), std::forward<Executor>(executor)));
  }
};

template <std::invocable F>
struct _trivial_call_awaiter {
  F f;
  bool await_ready() const noexcept {  // 不挂起
    return true;
  }
  void await_suspend(std::coroutine_handle<> h) {}
  decltype(auto) await_resume() {
    return std::forward<F>(f)();  // 保持左值/右值调用
  }
};

template <std::invocable F>
  requires(!co_awaitable<F>)
extended_awaitable<_trivial_call_awaiter<F>> lift(F&& callable) {
  return {{std::forward<F>(callable)}};
}

};  // namespace async
