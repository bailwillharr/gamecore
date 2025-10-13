#pragma once

#include <istream>
#include <streambuf>
#include <string_view>

namespace gct {

class sv_streambuf : public std::streambuf {
public:
    sv_streambuf(std::string_view sv)
    {
        char* const begin = const_cast<char*>(sv.data());
        char* const end = begin + sv.size();
        this->setg(begin, begin, end);
    }
};

// Non-owning stream wrapper for a string_view, useful for std::getline
class sv_istream : public std::istream {
    sv_streambuf m_buf;

public:
    sv_istream(std::string_view sv) : std::istream(&m_buf), m_buf(sv) {}
};

} // namespace gct