#pragma once

#include "gx.hpp"

namespace aurora::gx {
ShaderInfo build_shader_info(const ShaderConfig& config) noexcept;
gfx::Range build_uniform(const ShaderInfo& info) noexcept;
void fill_uniform_bytes(ByteBuffer& buf, const ShaderInfo& info) noexcept;
void reset_uniform_dedup() noexcept;
u8 color_channel(GXChannelID id) noexcept;
}; // namespace aurora::gx
