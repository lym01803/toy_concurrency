#pragma once

#include <chrono>
#include <compare>
#include <concepts>
#include <cstddef>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace msg {

template <typename T>
struct is_variant : public std::false_type {};

template <typename... Ts>
struct is_variant<std::variant<Ts...>> : public std::true_type {};

template <typename T>
constexpr bool is_variant_v = is_variant<T>::value;

template <typename T>
concept some_variant = is_variant_v<T>;

constexpr size_t index_not_found = -1;

template <typename T, typename... Ts>
struct index_of_ {
  static constexpr size_t value = index_not_found;
};

template <typename T, typename... Ts>
struct index_of_<T, T, Ts...> {
  static constexpr size_t value = 0;
};

template <typename T1, typename T2, typename... Ts>
struct index_of_<T1, T2, Ts...> {
  using sub = index_of_<T1, Ts...>;
  static constexpr size_t value =
      (sub::value == index_not_found) ? index_not_found : (sub::value + 1);
};

template <typename T, typename... Ts>
constexpr size_t index_of = index_of_<T, Ts...>::value;

template <typename T, typename Variant>
struct type_in_variant_ {
  static_assert(!std::is_same_v<T, T>, "Not supported template params.");
};

template <typename T, typename... Ts>
struct type_in_variant_<T, std::variant<Ts...>> {
  static constexpr size_t index = index_of<T, Ts...>;
};

template <typename T, typename V>
concept type_in_variant = some_variant<V> && (type_in_variant_<T, V>::index != index_not_found);

template <typename T, typename... Ts>
struct same_t_count {
  // O(n)
  static constexpr size_t value =
      ((size_t)0 + ... + (std::is_same_v<T, Ts> ? (size_t)1 : (size_t)0));
};

template <typename... Ts>
struct no_dup {
  // O(n^2)
  static constexpr bool value = (true && ... && (same_t_count<Ts, Ts...>::value == (size_t)1));
};

template <typename T>
struct no_dup_variant_ : public std::false_type {};

template <typename... Ts>
  requires no_dup<Ts...>::value
struct no_dup_variant_<std::variant<Ts...>> : public std::true_type {};

template <typename T>
concept no_dup_variant = no_dup_variant_<T>::value;

template <typename T>
struct variant_start_with_monostate_ : public std::false_type {};

template <typename... Ts>
struct variant_start_with_monostate_<std::variant<std::monostate, Ts...>> : public std::true_type {
};

template <typename T>
concept variant_start_with_monostate = variant_start_with_monostate_<T>::value;

template <typename T>
concept valid_msg_variant = some_variant<T> && variant_start_with_monostate<T> && no_dup_variant<T>;

template <valid_msg_variant V>
struct message {
  using variant_t = V;
  using timestamp_t = std::chrono::time_point<std::chrono::system_clock>;
  using serial_number_t = size_t;

  V data{std::monostate{}};
  serial_number_t serial_number{};
  std::optional<timestamp_t> timestamp;

  operator V const&() const noexcept {
    return data;
  }

  operator V&() noexcept {
    return data;
  }

  auto operator<=>(const message& other) const noexcept {
    return serial_number <=> other.serial_number;
  }

  operator bool() const noexcept {
    return !std::holds_alternative<std::monostate>(data);
  }

  auto operator-(const message& other) const noexcept {
    using duration_t = decltype(std::declval<timestamp_t>() - std::declval<timestamp_t>());
    return timestamp && other.timestamp ? std::optional<duration_t>{*timestamp - *(other.timestamp)}
                                        : std::optional<duration_t>{};
  }

  template <type_in_variant<V> T>
  bool data_t_same_as() const noexcept {
    return std::holds_alternative<T>(data);
  }

  template <type_in_variant<V>... Ts>
  bool data_t_in() const noexcept {
    return (false || ... || data_t_same_as<Ts>());
  }

  template <type_in_variant<V>... Ts>
  bool data_t_not_in() const noexcept {
    return (true && ... && !data_t_same_as<Ts>());
  }

  V* operator->() noexcept {
    return &data;
  }

  V const* operator->() const noexcept {
    return &data;
  }

  template <type_in_variant<V> T>
  T const& get() const {
    return std::get<T>(data);
  }

  template <type_in_variant<V> T>
  T& get() {
    return std::get<T>(data);
  }
};

}  // namespace msg
