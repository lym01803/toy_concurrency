#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

#include <toy_concurrency/async_tool.h>
#include <toy_concurrency/concurrency_utils.h>
#include <toy_concurrency/message.h>
#include <toy_concurrency/toyqueue.h>

namespace {

using namespace playground;

struct uniform_random {
  std::mt19937_64 gen{std::random_device{}()};
  std::uniform_int_distribution<int> dist;
  std::mutex mutex;

  uniform_random(int l, int r) : dist{l, r} {}
  int operator()() {
    std::lock_guard lock{mutex};
    return dist(gen);
  }
};

struct toy_server {
  struct request_t {
    int num_of_char;
  };

  enum class code_t : uint8_t { Done, Streaming };

  struct response_t {
    code_t code;
    std::string content;
  };

  using msg_t = msg::message<std::variant<std::monostate, response_t>>;
  using stream_t = sync_stream<msg_t>;

  uniform_random rd{1, 100};
  std::vector<guarded_thread> tasks;

  void request(request_t req, std::weak_ptr<stream_t> receive) {
    tasks.emplace_back(std::thread{[this, req, receive = std::move(receive)]() {
      for (size_t i = 0; i < req.num_of_char; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(rd()));
        if (auto _receive = receive.lock()) {
          _receive->write_sync(response_t{.code = code_t::Streaming, .content = "x"});
        } else {
          break;
        }
      }
      if (auto _receive = receive.lock()) {
        _receive->write_sync(response_t{.code = code_t::Done});
      }
    }});
  }
};

struct toy_client {
  using request_t = toy_server::request_t;
  using response_t = toy_server::response_t;
  using msg_t = msg::message<std::variant<std::monostate, response_t>>;
  using stream_t = sync_stream<msg_t>;

  struct stream_response_t {
    std::shared_ptr<stream_t> stream;
    std::unique_ptr<runner<async::to_execute_t<msg_t, async::read_stream, stream_t>>> worker{
        new runner<async::to_execute_t<msg_t, async::read_stream, stream_t>>{3}};  // cap = 8
    using dispatcher_t = decltype(stream->get_dispatcher(async::read_stream{}, *worker));
    dispatcher_t dispatcher = stream->get_dispatcher(async::read_stream{}, *worker);

    using chunk_t = msg::message<std::variant<std::monostate, std::string_view>>;

    struct chunk_awaitable : public dispatcher_t::awaitable {
      using base = dispatcher_t::awaitable;

      chunk_t await_resume() {
        auto ret = base::await_resume();
        if (ret == stream_t::status::good) {
          if (data.data_t_same_as<response_t>()) {
            auto& resp = data.get<response_t>();
            if (resp.code == toy_server::code_t::Streaming) {
              return chunk_t{std::string_view{resp.content}};
            }
          }
        }
        return chunk_t{};
      }
    };

    auto get_chunk(msg_t& msg_buffer) {
      return chunk_awaitable{dispatcher(msg_buffer)};
    }
  };

  stream_response_t stream_request(toy_server& server, int num) {
    stream_response_t resp{std::make_shared<stream_t>()};
    server.request(request_t{.num_of_char = num}, resp.stream);
    return resp;
  }
};

}  // namespace

void example_1() {
  using namespace async;
  auto server = toy_server{};
  auto client = toy_client{};
  using msg_t = toy_client::msg_t;
  using response_t = toy_client::response_t;
  auto resp = client.stream_request(server, 100);

  auto executor = runner<cancellable_function<void>>{};

  auto task = []<typename resp_t, typename executor_t>(resp_t& resp,
                                                       executor_t& executor) -> co_task {
    msg_t msg_buffer;
    while (true) {
      auto chunk = co_await lift(resp.get_chunk(msg_buffer)).back_to(executor);
      if (chunk) {
        auto ch = std::get<std::string_view>(chunk.data);
        std::cout << ch << std::flush;
      } else {
        break;
      }
    }
    std::cout << std::endl;
  }(resp, executor).get_future();

  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  task.get();
}

namespace {

struct progress_bar {
  int total{100};
  int current{0};
  std::string message;
  
  void update(int _current = -1, std::string _message = "") {
    current = _current >= 0 ? _current : current + 1;
    message = std::move(_message);
  }

  friend std::ostream& operator << (std::ostream& os, const progress_bar& obj) {
    os << std::format("\033[2K\r[{} / {}], {}", obj.current, obj.total, obj.message);
    return os;
  }
};

class gui_t {
 private:
  runner<async::cancellable_function<void>> sched;
  runner<async::cancellable_function<void>> timer{1};
  std::stop_source stop;
  progress_bar& bar;
  async::task_future<void> task;

 public:
  gui_t(progress_bar& bar)
      : bar{bar},
        task{[](std::stop_token token, auto& sched, auto& timer, auto& bar) -> async::co_task {
          auto output = async::lift([&]() { std::cout << bar << std::flush; }).on(sched);
          auto sleep =
              async::lift([]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));  // about 60 fps
              }).on(timer);
          while (!token.stop_requested()) {
            co_await output;
            co_await sleep;
          }
        }(stop.get_token(), sched, timer, bar).get_future()} {}
  ~gui_t() {
    stop.request_stop();
    task.get();
  }
  auto& scheduler() {
    return sched;
  }
};

struct primes {
 public:
  using end_t = std::monostate;

  template <typename Executor>
  static auto less_than(int n, Executor&& executor) -> async::yield_task<std::variant<end_t, int>> {
    std::vector<bool> is_prime(n, true);
    std::vector<int> primes;
    primes.reserve(n);
    for (int i = 2; i < n; i++) {
      if (is_prime[i]) {
        co_yield i;
        co_await async::execute_by(executor);
        primes.emplace_back(i);
      }
      for (auto p : primes) {
        if (i * p < n) {
          is_prime[i * p] = false;
          if (i % p == 0) {
            break;
          }
        } else {
          break;
        }
      }
    }
    co_return end_t{};
  }
};

} // namespace

void example_2() {
  auto computer = runner<async::cancellable_function<void>>{1};  // cap = 2 ^ 1 = 2
  const int N = 100'000'000;
  progress_bar bar{N};
  auto gui = gui_t(bar);
  auto prime_gen = primes::less_than(N, computer);

  auto prime_count = [](auto& prime_gen, auto& gui, auto& bar) -> async::co_task_with<int> {
    int count = 0;
    while (true) {
      auto item = co_await prime_gen;
      if (std::holds_alternative<primes::end_t>(item)) {
        break;
      }
      count++;
      auto prime = std::get<int>(item);
      co_await async::lift([&]() {
        bar.update(prime, std::format("count: {}", count));
      }).on(gui.scheduler());
    }
    co_return count;
  }(prime_gen, gui, bar).get_future();

  [](int count, int N, auto& scheduler) -> async::co_task {
    co_await async::lift([&]() {
      std::cout << std::endl
                << std::format("There are {} prime numbers less than {}.", count, N) << std::endl;
    }).on(scheduler);
  }(prime_count.get(), N, gui.scheduler()).get_future().get();
}

int main() {
  example_1();
  example_2();
  return 0;
}
