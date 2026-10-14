#pragma once

#include "frame_packet.hpp"

namespace aurora::gfx {

bool bind_pipeline(PipelineRef ref, const wgpu::RenderPassEncoder& pass);
// strikers-quest r43: the eye (0 left, 1 right) a stereo pass is being encoded for; 0 otherwise.
// Render worker only.
uint32_t current_stereo_eye() noexcept;

namespace detail {
void encode_op(wgpu::CommandEncoder& encoder, FramePacket& frame, const FrameOp& op);
}

} // namespace aurora::gfx
