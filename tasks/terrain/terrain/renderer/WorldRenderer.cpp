#include "WorldRenderer.hpp"

#include <etna/GlobalContext.hpp>
#include <etna/PipelineManager.hpp>
#include <etna/RenderTargetStates.hpp>
#include <etna/Profiling.hpp>
#include <etna/Etna.hpp>
#include <glm/ext.hpp>
#include <glm/gtc/noise.hpp>
#include <imgui.h>
#include <cstring>

WorldRenderer::WorldRenderer() 
  : oneShotCommands{etna::get_context().createOneShotCmdMgr()}
  , transferHelper{etna::BlockingTransferHelper::CreateInfo{.stagingSize = 4096 * 4096 * 4}}
{
}

void WorldRenderer::allocateResources(glm::uvec2 swapchain_resolution)
{
  resolution = swapchain_resolution;

  auto& ctx = etna::get_context();

  mainViewDepth = ctx.createImage(etna::Image::CreateInfo{
    .extent = vk::Extent3D{resolution.x, resolution.y, 1},
    .name = "main_view_depth",
    .format = vk::Format::eD32Sfloat,
    .imageUsage = vk::ImageUsageFlagBits::eDepthStencilAttachment,
  });

  createTerrainMesh();
}

void WorldRenderer::createTerrainMesh()
{
  const int GRID_SIZE = 512;
  std::vector<glm::vec3> vertices;
  std::vector<uint32_t> indices;

  for (int z = 0; z <= GRID_SIZE; ++z) {
    for (int x = 0; x <= GRID_SIZE; ++x) {
      float fx = float(x) / GRID_SIZE;
      float fz = float(z) / GRID_SIZE;
      
      float px = fx * 2.0f - 1.0f;
      float pz = fz * 2.0f - 1.0f;
      
      float height = 0.0f;
      float amp = 1.0f;
      float freq = 4.0f;
      for (int i = 0; i < 5; ++i) {
        height += amp * glm::simplex(glm::vec2(fx * freq, fz * freq));
        amp *= 0.5f;
        freq *= 2.0f;
      }
      height = (height * 0.5f + 0.5f) * 30.0f;

      vertices.push_back(glm::vec3(px * 100.0f, height, pz * 100.0f));
    }
  }
  
  for (int z = 0; z < GRID_SIZE; ++z) {
    for (int x = 0; x < GRID_SIZE; ++x) {
      uint32_t i = z * (GRID_SIZE + 1) + x;
      indices.push_back(i); indices.push_back(i + GRID_SIZE + 1); indices.push_back(i + 1);
      indices.push_back(i + 1); indices.push_back(i + GRID_SIZE + 1); indices.push_back(i + GRID_SIZE + 2);
    }
  }
  terrainIndexCount = static_cast<uint32_t>(indices.size());

  auto& ctx = etna::get_context();
  terrainVertexBuffer = ctx.createBuffer(etna::Buffer::CreateInfo{
    .size = vertices.size() * sizeof(glm::vec3),
    .bufferUsage = vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer,
    .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    .name = "terrain_vertex_buffer",
  });
  terrainIndexBuffer = ctx.createBuffer(etna::Buffer::CreateInfo{
    .size = indices.size() * sizeof(uint32_t),
    .bufferUsage = vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndexBuffer,
    .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    .name = "terrain_index_buffer",
  });
  
  transferHelper.uploadBuffer<glm::vec3>(*oneShotCommands, terrainVertexBuffer, 0, vertices);
  transferHelper.uploadBuffer<uint32_t>(*oneShotCommands, terrainIndexBuffer, 0, indices);
}

void WorldRenderer::generateHeightmap()
{
  // Заглушка (текстура не грузится из-за ограничений Etna)
}

void WorldRenderer::loadShaders()
{
  etna::create_program(
    "terrain",
    {TERRAIN_RENDERER_SHADERS_ROOT "terrain.frag.spv",
     TERRAIN_RENDERER_SHADERS_ROOT "terrain.vert.spv"});
}

void WorldRenderer::setupPipelines(vk::Format swapchain_format)
{
  etna::VertexShaderInputDescription sceneVertexInputDesc{
    .bindings = {etna::VertexShaderInputDescription::Binding{
      .byteStreamDescription = etna::VertexByteStreamFormatDescription{
        .stride = sizeof(glm::vec3),
        .attributes = {
          etna::VertexByteStreamFormatDescription::Attribute{
            .format = vk::Format::eR32G32B32Sfloat,
            .offset = 0,
          },
        }
      },
    }},
  };

  auto& pipelineManager = etna::get_context().getPipelineManager();

  terrainPipeline = pipelineManager.createGraphicsPipeline(
    "terrain",
    etna::GraphicsPipeline::CreateInfo{
      .vertexShaderInput = sceneVertexInputDesc,
      .rasterizationConfig =
        vk::PipelineRasterizationStateCreateInfo{
          .polygonMode = vk::PolygonMode::eFill,
          .cullMode = vk::CullModeFlagBits::eBack,
          .frontFace = vk::FrontFace::eCounterClockwise,
          .lineWidth = 1.f,
        },
      .fragmentShaderOutput =
        {
          .colorAttachmentFormats = {swapchain_format},
          .depthAttachmentFormat = vk::Format::eD32Sfloat,
        },
    });
}

void WorldRenderer::debugInput(const Keyboard&) {}

void WorldRenderer::update(const FramePacket& packet)
{
  const float aspect = float(resolution.x) / float(resolution.y);
  worldViewProj = packet.mainCam.projTm(aspect) * packet.mainCam.viewTm();
  pushConst.projView = worldViewProj;
  pushConst.cameraPos = packet.mainCam.position;
  pushConst.terrainHeight = 1.0f;
}

void WorldRenderer::drawGui()
{
  if (ImGui::Begin("Terrain")) { ImGui::Text("CPU Noise"); }
  ImGui::End();
}

void WorldRenderer::renderWorld(
  vk::CommandBuffer cmd_buf, vk::Image target_image, vk::ImageView target_image_view)
{
  ETNA_PROFILE_GPU(cmd_buf, renderWorld);

  {
    ETNA_PROFILE_GPU(cmd_buf, renderTerrain);

    etna::RenderTargetState renderTargets(
      cmd_buf,
      {{0, 0}, {resolution.x, resolution.y}},
      {{.image = target_image, .view = target_image_view}},
      {.image = mainViewDepth.get(), .view = mainViewDepth.getView({})});

    cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, terrainPipeline.getVkPipeline());
    
    cmd_buf.pushConstants<PushConstants>(
      terrainPipeline.getVkPipelineLayout(), vk::ShaderStageFlagBits::eVertex, 0, {pushConst});

    cmd_buf.bindVertexBuffers(0, {terrainVertexBuffer.get()}, {0});
    cmd_buf.bindIndexBuffer(terrainIndexBuffer.get(), 0, vk::IndexType::eUint32);
    
    cmd_buf.drawIndexed(terrainIndexCount, 1, 0, 0, 0);
  }
}