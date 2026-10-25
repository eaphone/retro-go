/* Adapted from jysdl-bh (scarsty) PotConv.cpp for retro-go: iconv passthrough. */
#include "PotConv.h"

std::string PotConv::conv(const std::string& src, const char* from, const char* to)
{
    (void)from;
    (void)to;
    return src;
}

std::string PotConv::conv(const std::string& src, const std::string& from, const std::string& to)
{
    return conv(src, from.c_str(), to.c_str());
}

std::string PotConv::to_read(const std::string& src)
{
    return src;
}
