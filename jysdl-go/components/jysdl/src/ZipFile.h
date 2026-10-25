/* Adapted from jysdl-bh (scarsty) ZipFile.h for retro-go.
 *
 * The original used libzip. We stub ZIP support for now; a follow-up can use
 * miniz (bundled) to implement readFile/getFileNames/zip/unzip.
 */
#pragma once
#include <string>
#include <vector>

class ZipFile
{
public:
    ZipFile();
    ~ZipFile();

    bool opened() const { return zip_ != nullptr; }

    void open(const std::string& zip_filename);
    void setPassword(const std::string& password) const;
    std::string readFile(const std::string& filename) const;
    std::vector<std::string> getFileNames() const;

    static int zip(std::string zip_file, std::vector<std::string> files);
    static int unzip(std::string zip_file, std::vector<std::string> files);

private:
    void* zip_ = nullptr;
};
