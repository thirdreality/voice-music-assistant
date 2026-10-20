// SHA-256 of a file, without an OpenSSL dependency.

#pragma once

#include <filesystem>
#include <string>

namespace lva::util {

// Lowercase hex digest, or empty if the file cannot be read.
std::string Sha256FileHex(const std::filesystem::path& path);

}  // namespace lva::util
