/* Adapted from jysdl-bh (scarsty) ZipFile.cpp for retro-go: libzip stub. */
#include "ZipFile.h"

ZipFile::ZipFile() = default;
ZipFile::~ZipFile() = default;

void ZipFile::open(const std::string& zip_filename)
{
    (void)zip_filename;
    zip_ = nullptr;
}

void ZipFile::setPassword(const std::string& password) const
{
    (void)password;
}

std::string ZipFile::readFile(const std::string& filename) const
{
    (void)filename;
    return std::string();
}

std::vector<std::string> ZipFile::getFileNames() const
{
    return std::vector<std::string>();
}

int ZipFile::zip(std::string zip_file, std::vector<std::string> files)
{
    (void)zip_file;
    (void)files;
    return 0;
}

int ZipFile::unzip(std::string zip_file, std::vector<std::string> files)
{
    (void)zip_file;
    (void)files;
    return 0;
}
