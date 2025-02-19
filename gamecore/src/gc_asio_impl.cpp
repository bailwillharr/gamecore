#include "gamecore/gc_abort.h"

#include <asio/impl/src.hpp>

namespace asio::detail {

template <typename Exception>
void throw_exception(const Exception& e)
{
    gc::abortGame("ASIO exception: {}", e.what());
}

} // namespace asio::internal