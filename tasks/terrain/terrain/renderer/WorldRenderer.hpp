#pragma once

#include <etna/Image.hpp>
#include <etna/Buffer.hpp>
#include <etna/GraphicsPipeline.hpp>
#include <etna/BlockingTransferHelper.hpp>
#include <etna/OneShotCmdMgr.hpp>
#include <glm/glm.hpp>
#include <vector>
#include <memory>

#include "FramePacket.hpp"
#include "wsi/Keyboard.hpp"

class WorldRenderer
{
public:
  WorldRenderer();

  void loadShaders();
  void allocateResources(glm::uvec2 swapchain_resolution);
  void setupPipelines(vk::Format swapchain_format);

  void debugInput(const Keyboard& kb);
  void update(const FramePacket& packet);
  void drawGui();
  void renderWorld(
    vk::CommandBuffer cmd_buf, vk::Image target_image, vk::ImageView target_image_view);

private:
  void createTerrainMesh();
  void generateHeightmap();

  etna::Image mainViewDepth;
  etna::Buffer terrainVertexBuffer;
  etna::Buffer terrainIndexBuffer;
  uint32_t terrainIndexCount = 0;

  etna::Image heightmapImage;
  vk::UniqueSampler heightmapSampler;

  std::unique_ptr<etna::OneShotCmdMgr> oneShotCommands;
  etna::BlockingTransferHelper transferHelper;
  etna::GraphicsPipeline terrainPipeline{};

  struct PushConstants
  {
    glm::mat4x4 projView;
    glm::vec3 cameraPos;
    float terrainHeight;
  } pushConst;

  glm::mat4x4 worldViewProj;
  glm::uvec2 resolution;
};