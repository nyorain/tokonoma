#pragma once

#include <string>
#include <utility>
#include <memory>
#include <nytl/fwd.hpp>

namespace tkn::use {

using std::string;
using std::move;
using std::unique_ptr;
using std::byte;
using Bytes = nytl::Span<std::byte>;

} // namespace tkn::use
