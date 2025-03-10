#pragma once

#include <iterator>
#include <type_traits>

namespace arqmamq::detail {

template <typename OutputIt, typename = void>
struct byte_type { using type = char; };

template <typename OutputIt>
struct byte_type<OutputIt, std::void_t<typename OutputIt::container_type>>
{
  using type = typename OutputIt::container_type::value_type;
};

template <typename OutputIt>
struct byte_type<OutputIt, std::enable_if_t<std::is_reference_v<typename std::iterator_traits<OutputIt>::reference>>>
{
  using type = std::remove_reference_t<typename std::iterator_traits<OutputIt>::reference>;
};

template <typename OutputIt>
using byte_type_t = typename byte_type<OutputIt>::type;

}