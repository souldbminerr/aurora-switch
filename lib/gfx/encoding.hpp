#pragma once

#include "frame_packet.hpp"

namespace aurora::gfx {

bool bind_pipeline(PipelineRef ref, const wgpu::RenderPassEncoder& pass);

// Skips redundant per-draw binds when the state matches the previous draw on
// the current pass encoder. Reset at each new pass and after out-of-band
// encoder writes (see reset_encoder_bind_cache).
void bind_texture_group(const wgpu::RenderPassEncoder& pass, BindGroupRef ref);
void set_blend_constant_for_dst_alpha(const wgpu::RenderPassEncoder& pass, uint32_t dstAlpha);
void reset_encoder_bind_cache() noexcept;

namespace detail {
void encode_op(wgpu::CommandEncoder& encoder, FramePacket& frame, const FrameOp& op);
}

} // namespace aurora::gfx
