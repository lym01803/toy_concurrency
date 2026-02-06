# Concurrency Tools

一个基于 C++20 编写的“玩具”并发与协程工具库。本项目包含了作者在学习 C++ 并发编程过程中实现的若干“玩具”组件，旨在提供一套简单易用的异步编程基础设施。

## 核心特性

- **现代协程支持**：基于 C++20 Coroutines 实现的 Lazy 任务 (`co_task`, `co_task_with`)、Generator (`yield_task`)、异步流读取等。
- **执行器**：利用模板实现执行上下文分发。在 I/O 分发等特定场景下可避免 `std::function` 的开销；在通用任务分发时，也提供了任务包装机制，支持取消任务。
- **并发原语**：包括基于 CAS 的固定容量并发队列、RAII 线程守护者、同步/异步消息流等。
- **消息系统**：标准化的消息包装格式，支持时间戳、序号与 `std::variant` 数据载荷。

## 导入与使用

本项目为 **Header-only** 库，要求支持 **C++20** 的编译器。

### 使用 CMake 导入

推荐使用 **FetchContent** 自动集成：

```cmake
include(FetchContent)
FetchContent_Declare(
    toy_concurrency
    GIT_REPOSITORY https://github.com/lym01803/toy_concurrency.git
    GIT_TAG main
)
FetchContent_MakeAvailable(toy_concurrency)

target_link_libraries(your_target PRIVATE toy_concurrency)
```

### 手动包含

只需将 `include` 目录添加到你的编译器包含路径中：

```bash
g++ -std=c++20 -Ipath/to/concurrency_tools/include your_code.cpp
```

## 组件概览

### 1. 异步协程工具 (`async_tool.h`)
命名空间：`async`

提供协程任务管理与上下文切换功能：
- `co_task` / `co_task_with<T>`: 延迟执行的任务。
- `async_call(func, executor)`: 将普通函数包装为协程，并在指定执行器上异步执行。
- `lift(awaitable/callable)`: 包装一个普通函数或 Awaitable 对象，支持链式调度。
- `.on(executor)`: 指定在特定执行器上启动/执行。
- `.back_to(executor)`: 在等待的操作完成后，切回指定的执行器继续执行。
- `execute_by(executor)`: 手动切换当前协程的执行线程（`co_await async::execute_by(executor);`）。

### 2. 执行器与流 (`concurrency_utils.h`)
命名空间：`playground`

- `runner<F>`: 一个简易的任务运行器（线程池），支持任务取消。
- `sync_stream<T>`: 支持多生产者多消费者的线程安全消息流。
    - **协程支持**：通过 `get_dispatcher(op, executor)` 获取分发器，允许在协程中非阻塞地 `co_await` 流的读写操作。
- `guarded_thread`: RAII 风格的线程包装，析构时自动 `join`。

### 3. 并发队列 (`toyqueue.h`)
命名空间：`toyqueue`

- `fix_cap_queue<T>`: 一个基于原子操作实现的固定容量并发队列，支持 MPMC。

### 4. 消息封装 (`message.h`)
命名空间：`msg`

- `message<Variant>`: 包装了 `std::variant` 的消息结构，自动处理序号分配与时间戳记录。

## 使用示例

`examples/example.cpp`