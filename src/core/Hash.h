#pragma once
#include <filesystem>
#include <string>

namespace modelhub {

// Lower-case hex SHA-256 of a file's contents. Throws std::runtime_error on I/O or CNG failure.
std::string Sha256File(const std::filesystem::path& p);
std::string Sha256(std::string_view data);

}  // namespace modelhub
