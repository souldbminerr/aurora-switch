#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

namespace aurora::io {

bool write_file_atomic(const std::filesystem::path& path, std::span<const uint8_t> data) noexcept;

} // namespace aurora::io
