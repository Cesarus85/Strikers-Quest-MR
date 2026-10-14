#include "pipeline.hpp"

#include "../gfx/encoding.hpp"
#include "../gfx/resources.hpp"
#include "../gfx/pipeline_cache.hpp"
#include "../gfx/resource_cache.hpp"

#include "gx_fmt.hpp"
#include "shader_info.hpp"

#include <tracy/Tracy.hpp>

namespace aurora::gx {

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config) {
  ZoneScoped;
  const auto shader = build_shader(config.shaderConfig);
  const auto label =
      fmt::format("GX Pipeline {:x} shader {:x}", xxh3_hash(config, static_cast<HashType>(gfx::ShaderType::GX)),
                  xxh3_hash(config.shaderConfig));
  return build_pipeline(config, {}, shader, label.c_str());
}

void render(const DrawData& data, const wgpu::RenderPassEncoder& pass) {
  if (!gfx::bind_pipeline(data.pipeline, pass)) {
    return;
  }

  const auto& resources = gfx::detail::resources();
  pass.SetImmediates(0, &data.immediateData, sizeof(data.immediateData));
  const std::array offsets{data.uniformRange.offset};
  pass.SetBindGroup(1, resources.uniformBindGroup, offsets.size(), offsets.data());
  if (data.bindGroups.textureBindGroup) {
    pass.SetBindGroup(2, gfx::find_bind_group(data.bindGroups.textureBindGroup));
  }
  pass.SetIndexBuffer(resources.indexBuffer, wgpu::IndexFormat::Uint16, data.idxRange.offset, data.idxRange.size);
  if (is_xr_coverage(data.dstAlpha)) {
    const wgpu::Color color{0.f, 0.f, 0.f, 1.f};  // strikers-quest: solid draws are fully covered
    pass.SetBlendConstant(&color);
  } else if (data.dstAlpha != UINT32_MAX) {
    const wgpu::Color color{0.f, 0.f, 0.f, data.dstAlpha / 255.f};
    pass.SetBlendConstant(&color);
  }
  // strikers-quest r43: the stereo shader picks its eye from instance_index.
  const uint32_t firstInstance = data.stereo != 0 ? gfx::current_stereo_eye() : 0;
  if (data.indexCount == 0) {
    pass.Draw(data.vtxCount, data.instanceCount, 0, firstInstance);
  } else {
    pass.DrawIndexed(data.indexCount, data.instanceCount, 0, 0, firstInstance);
  }
}

} // namespace aurora::gx
