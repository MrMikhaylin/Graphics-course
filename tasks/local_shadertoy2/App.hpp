#pragma once

#include <etna/Window.hpp>
#include <etna/PerFrameCmdMgr.hpp>
#include <etna/GraphicsPipeline.hpp>
#include <etna/Image.hpp>
#include <etna/Buffer.hpp>
#include <etna/DescriptorSet.hpp>
#include <etna/Sampler.hpp>

#include "wsi/OsWindowingManager.hpp"


class App
{
public:
  App();
  ~App();

  void run();

private:
  void drawFrame();

  void initLoadedTexture();
  void generateMipmaps(vk::CommandBuffer cmd, etna::Image& image, uint32_t mipLevels);

private:
  OsWindowingManager windowing;
  std::unique_ptr<OsWindow> osWindow;

  glm::uvec2 resolution;
  bool useVsync;

  std::unique_ptr<etna::Window> vkWindow;
  std::unique_ptr<etna::PerFrameCmdMgr> commandManager;

  etna::GraphicsPipeline procAlbedoPipeline;
  etna::GraphicsPipeline procMaterialPipeline;
  etna::GraphicsPipeline mainPipeline;

  etna::Image albedoTex;
  etna::Image materialTex;
  etna::Image resultImage;

  etna::Image loadedTex;
  uint32_t loadedTexMipLevels = 1;
  bool loadedTexTrackerSynced = false;

  etna::Sampler linearSampler;
  etna::Sampler mipSampler;

  double startTime = 0.0;
};