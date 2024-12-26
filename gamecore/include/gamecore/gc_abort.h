#pragma once

#include <string_view>

namespace gc {

/* Aborts the program and logs an error message */
/* Should only be used if the error is absolutely non recoverable */
[[noreturn]] void abortGame(std::string_view msg);

}