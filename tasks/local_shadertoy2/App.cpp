#include "App.hpp"

#include <etna/Etna.hpp>
#include <etna/GlobalContext.hpp>
#include <etna/PipelineManager.hpp>
#include <etna/OneShotCmdMgr.hpp>
#include <etna/RenderTargetStates.hpp>

#include <spdlog/spdlog.h>
#include <GLFW/glfw3.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cmath>
#include <cstring>
#include <vector>


namespace
{
struct PushConstants
{
  glm::vec2 iResolution;
  float iTime;
  float padding = 0.0f;
  glm::vec4 iMouse;
};
}


App::App()
  : resolution{1280, 720}
  , useVsync{true}
{
  {
    auto glfwInstExts = windowing.getRequiredVulkanInstanceExtensions();
    std::vector<const char*> instanceExtensions{glfwInstExts.begin(), glfwInstExts.end()};
    std::vector<const char*> deviceExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    etna::initialize(etna::InitParams{
      .applicationName = "Local Shadertoy 2",
      .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
      .instanceExtensions = instanceExtensions,
      .deviceExtensions = deviceExtensions,
      .features = vk::PhysicalDeviceFeatures2{.features = {}},
      .physicalDeviceIndexOverride = {},
      .numFramesInFlight = 1,
    });
  }

  commandManager = etna::get_context().createPerFrameCmdMgr();

  osWindow = windowing.createWindow(OsWindow::CreateInfo{
    .resolution = resolution,
  });

  {
    auto surface = osWindow->createVkSurface(etna::get_context().getInstance());

    vkWindow = etna::get_context().createWindow(etna::Window::CreateInfo{
      .surface = std::move(surface),
    });

    auto [w, h] = vkWindow->recreateSwapchain(etna::Window::DesiredProperties{
      .resolution = {resolution.x, resolution.y},
      .vsync = useVsync,
      .numFramesInFlight = static_cast<uint32_t>(commandManager->getCmdBufferCount()),
    });

    spdlog::info("recreateSwapchain: {}x{}", w, h);
  }

  etna::create_program(
    "proc_albedo",
    {LOCAL_SHADERTOY2_SHADERS_ROOT "fullscreen.vert.spv",
     LOCAL_SHADERTOY2_SHADERS_ROOT "albedo.frag.spv"});

  etna::create_program(
    "proc_material",
    {LOCAL_SHADERTOY2_SHADERS_ROOT "fullscreen.vert.spv",
     LOCAL_SHADERTOY2_SHADERS_ROOT "material.frag.spv"});

  etna::create_program(
    "main",
    {LOCAL_SHADERTOY2_SHADERS_ROOT "fullscreen.vert.spv",
     LOCAL_SHADERTOY2_SHADERS_ROOT "toy.frag.spv"});

  auto& pm = etna::get_context().getPipelineManager();

  constexpr vk::Format colorFormat = vk::Format::eR8G8B8A8Unorm;

  etna::GraphicsPipeline::CreateInfo gfxInfo{
    .depthConfig = {
      .depthTestEnable = vk::False,
      .depthWriteEnable = vk::False,
      .depthCompareOp = vk::CompareOp::eAlways,
    },
    .fragmentShaderOutput = {
      .colorAttachmentFormats = {colorFormat},
      .depthAttachmentFormat = vk::Format::eUndefined,
      .stencilAttachmentFormat = vk::Format::eUndefined,
    },
    .dynamicStates = {vk::DynamicState::eViewport, vk::DynamicState::eScissor},
  };

  procAlbedoPipeline = pm.createGraphicsPipeline("proc_albedo", gfxInfo);
  procMaterialPipeline = pm.createGraphicsPipeline("proc_material", gfxInfo);
  mainPipeline = pm.createGraphicsPipeline("main", gfxInfo);

  albedoTex = etna::get_context().createImage(etna::Image::CreateInfo{
    .extent = vk::Extent3D{512, 512, 1},
    .name = "albedo_tex",
    .format = colorFormat,
    .imageUsage = vk::ImageUsageFlagBits::eColorAttachment |
                  vk::ImageUsageFlagBits::eSampled |
                  vk::ImageUsageFlagBits::eTransferSrc,
  });

  materialTex = etna::get_context().createImage(etna::Image::CreateInfo{
    .extent = vk::Extent3D{512, 512, 1},
    .name = "material_tex",
    .format = colorFormat,
    .imageUsage = vk::ImageUsageFlagBits::eColorAttachment |
                  vk::ImageUsageFlagBits::eSampled |
                  vk::ImageUsageFlagBits::eTransferSrc,
  });

  resultImage = etna::get_context().createImage(etna::Image::CreateInfo{
    .extent = vk::Extent3D{resolution.x, resolution.y, 1},
    .name = "shadertoy_result",
    .format = colorFormat,
    .imageUsage = vk::ImageUsageFlagBits::eColorAttachment |
                  vk::ImageUsageFlagBits::eTransferSrc,
  });

  linearSampler = etna::Sampler(etna::Sampler::CreateInfo{
    .filter = vk::Filter::eLinear,
    .addressMode = vk::SamplerAddressMode::eRepeat,
    .mipmapMode = vk::SamplerMipmapMode::eNearest,
    .name = "linear_sampler",
    .minLod = 0.0f,
    .maxLod = 0.0f,
  });

  initLoadedTexture();

  mipSampler = etna::Sampler(etna::Sampler::CreateInfo{
    .filter = vk::Filter::eLinear,
    .addressMode = vk::SamplerAddressMode::eRepeat,
    .mipmapMode = vk::SamplerMipmapMode::eLinear,
    .name = "mip_sampler",
    .minLod = 0.0f,
    .maxLod = static_cast<float>(loadedTexMipLevels - 1),
  });

  spdlog::info("App initialized");
  startTime = glfwGetTime();
}

App::~App()
{
  ETNA_CHECK_VK_RESULT(etna::get_context().getDevice().waitIdle());
}

void App::initLoadedTexture()
{
  const char* texPath = GRAPHICS_COURSE_RESOURCES_ROOT "/textures/test_tex_1.png";

  int w = 0, h = 0, channels = 0;
  stbi_uc* data = stbi_load(texPath, &w, &h, &channels, 4);
  if (!data)
  {
    spdlog::error("Failed to load texture: {}", texPath);
    return;
  }

  const uint32_t W = static_cast<uint32_t>(w);
  const uint32_t H = static_cast<uint32_t>(h);
  loadedTexMipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(W, H)))) + 1;

  spdlog::info("Loaded texture {} ({}x{}, {} channels)", texPath, W, H, channels);

  loadedTex = etna::get_context().createImage(etna::Image::CreateInfo{
    .extent = vk::Extent3D{W, H, 1},
    .name = "loaded_tex",
    .format = vk::Format::eR8G8B8A8Unorm,
    .imageUsage = vk::ImageUsageFlagBits::eSampled |
                  vk::ImageUsageFlagBits::eTransferSrc |
                  vk::ImageUsageFlagBits::eTransferDst,
    .mipLevels = loadedTexMipLevels,
  });

  auto staging = etna::get_context().createBuffer(etna::Buffer::CreateInfo{
    .size = static_cast<vk::DeviceSize>(W) * H * 4,
    .bufferUsage = vk::BufferUsageFlagBits::eTransferSrc,
    .memoryUsage = VMA_MEMORY_USAGE_AUTO,
    .allocationCreate = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT,
    .name = "loaded_tex_staging",
  });

  std::memcpy(staging.map(), data, static_cast<size_t>(W) * H * 4);
  staging.unmap();
  stbi_image_free(data);

  auto& ctx = etna::get_context();
  auto cmdMgr = ctx.createOneShotCmdMgr();
  vk::CommandBuffer cmd = cmdMgr->start();
  ETNA_CHECK_VK_RESULT(cmd.begin(vk::CommandBufferBeginInfo{}));

  {
    vk::ImageMemoryBarrier2 b{
      .srcStageMask = vk::PipelineStageFlagBits2::eNone,
      .srcAccessMask = vk::AccessFlagBits2::eNone,
      .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
      .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
      .oldLayout = vk::ImageLayout::eUndefined,
      .newLayout = vk::ImageLayout::eTransferDstOptimal,
      .image = loadedTex.get(),
      .subresourceRange = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
      },
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &b,
    });
  }

  vk::BufferImageCopy copy{};
  copy.bufferOffset = 0;
  copy.bufferRowLength = 0;
  copy.bufferImageHeight = 0;
  copy.imageSubresource = vk::ImageSubresourceLayers{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .mipLevel = 0,
    .baseArrayLayer = 0,
    .layerCount = 1,
  };
  copy.imageOffset = vk::Offset3D{0, 0, 0};
  copy.imageExtent = vk::Extent3D{W, H, 1};

  cmd.copyBufferToImage(
    staging.get(),
    loadedTex.get(),
    vk::ImageLayout::eTransferDstOptimal,
    {copy});

  generateMipmaps(cmd, loadedTex, loadedTexMipLevels);

  ETNA_CHECK_VK_RESULT(cmd.end());
  cmdMgr->submitAndWait(cmd);

  spdlog::info("Loaded texture with {} mip levels", loadedTexMipLevels);
}

void App::generateMipmaps(vk::CommandBuffer cmd, etna::Image& image, uint32_t mipLevels)
{
  if (mipLevels <= 1)
    return;

  {
    std::vector<vk::ImageMemoryBarrier2> barriers;
    barriers.reserve(mipLevels - 1);
    for (uint32_t i = 1; i < mipLevels; ++i)
    {
      barriers.push_back(vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eNone,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = image.get(),
        .subresourceRange = {
          .aspectMask = vk::ImageAspectFlagBits::eColor,
          .baseMipLevel = i,
          .levelCount = 1,
          .baseArrayLayer = 0,
          .layerCount = 1,
        },
      });
    }
    cmd.pipelineBarrier2(vk::DependencyInfo{
      .imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
      .pImageMemoryBarriers = barriers.data(),
    });
  }

  {
    vk::ImageMemoryBarrier2 b{
      .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
      .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
      .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
      .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
      .oldLayout = vk::ImageLayout::eTransferDstOptimal,
      .newLayout = vk::ImageLayout::eTransferSrcOptimal,
      .image = image.get(),
      .subresourceRange = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
      },
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &b,
    });
  }

  int32_t mipW = static_cast<int32_t>(image.getExtent().width);
  int32_t mipH = static_cast<int32_t>(image.getExtent().height);

  for (uint32_t i = 1; i < mipLevels; ++i)
  {
    int32_t nextW = std::max(mipW / 2, 1);
    int32_t nextH = std::max(mipH / 2, 1);

    vk::ImageBlit blit{};
    blit.srcSubresource = vk::ImageSubresourceLayers{
      .aspectMask = vk::ImageAspectFlagBits::eColor,
      .mipLevel = i - 1,
      .baseArrayLayer = 0,
      .layerCount = 1,
    };
    blit.srcOffsets[0] = vk::Offset3D{0, 0, 0};
    blit.srcOffsets[1] = vk::Offset3D{mipW, mipH, 1};

    blit.dstSubresource = vk::ImageSubresourceLayers{
      .aspectMask = vk::ImageAspectFlagBits::eColor,
      .mipLevel = i,
      .baseArrayLayer = 0,
      .layerCount = 1,
    };
    blit.dstOffsets[0] = vk::Offset3D{0, 0, 0};
    blit.dstOffsets[1] = vk::Offset3D{nextW, nextH, 1};

    cmd.blitImage(
      image.get(), vk::ImageLayout::eTransferSrcOptimal,
      image.get(), vk::ImageLayout::eTransferDstOptimal,
      {blit}, vk::Filter::eLinear);

    mipW = nextW;
    mipH = nextH;

    if (i < mipLevels - 1)
    {
      vk::ImageMemoryBarrier2 b{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
        .image = image.get(),
        .subresourceRange = {
          .aspectMask = vk::ImageAspectFlagBits::eColor,
          .baseMipLevel = i,
          .levelCount = 1,
          .baseArrayLayer = 0,
          .layerCount = 1,
        },
      };
      cmd.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &b,
      });
    }
  }

  {
    std::vector<vk::ImageMemoryBarrier2> barriers;
    barriers.reserve(mipLevels);
    for (uint32_t i = 0; i < mipLevels; ++i)
    {
      barriers.push_back(vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eTransferRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = (i < mipLevels - 1) ? vk::ImageLayout::eTransferSrcOptimal
                                         : vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = image.get(),
        .subresourceRange = {
          .aspectMask = vk::ImageAspectFlagBits::eColor,
          .baseMipLevel = i,
          .levelCount = 1,
          .baseArrayLayer = 0,
          .layerCount = 1,
        },
      });
    }
    cmd.pipelineBarrier2(vk::DependencyInfo{
      .imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
      .pImageMemoryBarriers = barriers.data(),
    });
  }
}

void App::run()
{
  bool rWasPressed = false;

  while (!osWindow->isBeingClosed())
  {
    windowing.poll();

    bool rIsPressed = glfwGetKey(osWindow->native(), GLFW_KEY_R) == GLFW_PRESS;
    if (rIsPressed && !rWasPressed)
    {
      ETNA_CHECK_VK_RESULT(etna::get_context().getDevice().waitIdle());
      etna::reload_shaders();
      spdlog::info("Shaders reloaded!");
    }
    rWasPressed = rIsPressed;

    drawFrame();
  }

  ETNA_CHECK_VK_RESULT(etna::get_context().getDevice().waitIdle());
}

void App::drawFrame()
{
  auto currentCmdBuf = commandManager->acquireNext();

  etna::begin_frame();

  auto nextSwapchainImage = vkWindow->acquireNext();

  if (nextSwapchainImage)
  {
    auto [backbuffer, backbufferView, backbufferAvailableSem, backbufferReadyForPresentSem] =
      *nextSwapchainImage;

    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(osWindow->native(), &mx, &my);
    bool leftDown =
      glfwGetMouseButton(osWindow->native(), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

    PushConstants pc{
      glm::vec2(static_cast<float>(resolution.x), static_cast<float>(resolution.y)),
      static_cast<float>(glfwGetTime() - startTime),
      0.0f,
      glm::vec4(
        static_cast<float>(mx),
        static_cast<float>(my),
        leftDown ? 1.0f : 0.0f,
        0.0f)
    };

    ETNA_CHECK_VK_RESULT(currentCmdBuf.begin(vk::CommandBufferBeginInfo{}));
    {
      etna::set_state(
        currentCmdBuf,
        backbuffer,
        vk::PipelineStageFlagBits2::eTransfer,
        vk::AccessFlagBits2::eTransferWrite,
        vk::ImageLayout::eTransferDstOptimal,
        vk::ImageAspectFlagBits::eColor);

      if (!loadedTexTrackerSynced)
      {
        etna::set_state(
          currentCmdBuf,
          loadedTex.get(),
          vk::PipelineStageFlagBits2::eFragmentShader,
          vk::AccessFlagBits2::eShaderRead,
          vk::ImageLayout::eShaderReadOnlyOptimal,
          vk::ImageAspectFlagBits::eColor);
        etna::flush_barriers(currentCmdBuf);
        loadedTexTrackerSynced = true;
      }

      {
        etna::set_state(
          currentCmdBuf,
          albedoTex.get(),
          vk::PipelineStageFlagBits2::eColorAttachmentOutput,
          vk::AccessFlagBits2::eColorAttachmentWrite,
          vk::ImageLayout::eColorAttachmentOptimal,
          vk::ImageAspectFlagBits::eColor);
        etna::flush_barriers(currentCmdBuf);

        currentCmdBuf.bindPipeline(
          vk::PipelineBindPoint::eGraphics,
          procAlbedoPipeline.getVkPipeline());

        currentCmdBuf.pushConstants(
          procAlbedoPipeline.getVkPipelineLayout(),
          vk::ShaderStageFlagBits::eFragment,
          0, sizeof(pc), &pc);

        {
          vk::Viewport viewport{0.f, 0.f, 512.f, 512.f, 0.f, 1.f};
          currentCmdBuf.setViewport(0, 1, &viewport);
          vk::Rect2D scissor{{0, 0}, {512, 512}};
          currentCmdBuf.setScissor(0, 1, &scissor);
        }

        {
          etna::RenderTargetState rtState(
            currentCmdBuf,
            vk::Rect2D{{0, 0}, {512, 512}},
            {
              etna::RenderTargetState::AttachmentParams{
                .image = albedoTex.get(),
                .view = albedoTex.getView({}),
                .loadOp = vk::AttachmentLoadOp::eClear,
                .storeOp = vk::AttachmentStoreOp::eStore,
                .clearColorValue = vk::ClearColorValue(std::array<float,4>{0.f,0.f,0.f,1.f}),
              },
            },
            {});
          currentCmdBuf.draw(3, 1, 0, 0);
        }
      }

      {
        etna::set_state(
          currentCmdBuf,
          materialTex.get(),
          vk::PipelineStageFlagBits2::eColorAttachmentOutput,
          vk::AccessFlagBits2::eColorAttachmentWrite,
          vk::ImageLayout::eColorAttachmentOptimal,
          vk::ImageAspectFlagBits::eColor);
        etna::flush_barriers(currentCmdBuf);

        currentCmdBuf.bindPipeline(
          vk::PipelineBindPoint::eGraphics,
          procMaterialPipeline.getVkPipeline());

        currentCmdBuf.pushConstants(
          procMaterialPipeline.getVkPipelineLayout(),
          vk::ShaderStageFlagBits::eFragment,
          0, sizeof(pc), &pc);

        {
          vk::Viewport viewport{0.f, 0.f, 512.f, 512.f, 0.f, 1.f};
          currentCmdBuf.setViewport(0, 1, &viewport);
          vk::Rect2D scissor{{0, 0}, {512, 512}};
          currentCmdBuf.setScissor(0, 1, &scissor);
        }

        {
          etna::RenderTargetState rtState(
            currentCmdBuf,
            vk::Rect2D{{0, 0}, {512, 512}},
            {
              etna::RenderTargetState::AttachmentParams{
                .image = materialTex.get(),
                .view = materialTex.getView({}),
                .loadOp = vk::AttachmentLoadOp::eClear,
                .storeOp = vk::AttachmentStoreOp::eStore,
                .clearColorValue = vk::ClearColorValue(std::array<float,4>{0.f,0.f,0.f,1.f}),
              },
            },
            {});
          currentCmdBuf.draw(3, 1, 0, 0);
        }
      }

      etna::set_state(
        currentCmdBuf,
        albedoTex.get(),
        vk::PipelineStageFlagBits2::eFragmentShader,
        vk::AccessFlagBits2::eShaderRead,
        vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::ImageAspectFlagBits::eColor);

      etna::set_state(
        currentCmdBuf,
        materialTex.get(),
        vk::PipelineStageFlagBits2::eFragmentShader,
        vk::AccessFlagBits2::eShaderRead,
        vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::ImageAspectFlagBits::eColor);

      etna::set_state(
        currentCmdBuf,
        resultImage.get(),
        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentWrite,
        vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageAspectFlagBits::eColor);
      etna::flush_barriers(currentCmdBuf);

      {
        auto prog = etna::get_shader_program("main");
        auto set = etna::create_descriptor_set(
          prog.getDescriptorLayoutId(0),
          currentCmdBuf,
          {
            etna::Binding{0, albedoTex.genBinding(
              linearSampler.get(), vk::ImageLayout::eShaderReadOnlyOptimal)},
            etna::Binding{1, materialTex.genBinding(
              linearSampler.get(), vk::ImageLayout::eShaderReadOnlyOptimal)},
            etna::Binding{2, loadedTex.genBinding(
              mipSampler.get(), vk::ImageLayout::eShaderReadOnlyOptimal)},
          });

        currentCmdBuf.bindPipeline(
          vk::PipelineBindPoint::eGraphics,
          mainPipeline.getVkPipeline());

        currentCmdBuf.pushConstants(
          mainPipeline.getVkPipelineLayout(),
          vk::ShaderStageFlagBits::eFragment,
          0, sizeof(pc), &pc);

        vk::DescriptorSet vkSet = set.getVkSet();
        currentCmdBuf.bindDescriptorSets(
          vk::PipelineBindPoint::eGraphics,
          mainPipeline.getVkPipelineLayout(),
          0, 1, &vkSet, 0, nullptr);

        {
          vk::Viewport viewport{
            0.f, 0.f,
            static_cast<float>(resolution.x), static_cast<float>(resolution.y),
            0.f, 1.f};
          currentCmdBuf.setViewport(0, 1, &viewport);
          vk::Rect2D scissor{{0, 0}, {resolution.x, resolution.y}};
          currentCmdBuf.setScissor(0, 1, &scissor);
        }

        {
          etna::RenderTargetState rtState(
            currentCmdBuf,
            vk::Rect2D{{0, 0}, {resolution.x, resolution.y}},
            {
              etna::RenderTargetState::AttachmentParams{
                .image = resultImage.get(),
                .view = resultImage.getView({}),
                .loadOp = vk::AttachmentLoadOp::eClear,
                .storeOp = vk::AttachmentStoreOp::eStore,
                .clearColorValue = vk::ClearColorValue(std::array<float,4>{0.f,0.f,0.f,1.f}),
              },
            },
            {});
          currentCmdBuf.draw(3, 1, 0, 0);
        }
      }

      etna::set_state(
        currentCmdBuf,
        resultImage.get(),
        vk::PipelineStageFlagBits2::eTransfer,
        vk::AccessFlagBits2::eTransferRead,
        vk::ImageLayout::eTransferSrcOptimal,
        vk::ImageAspectFlagBits::eColor);
      etna::flush_barriers(currentCmdBuf);

      vk::ImageBlit blitRegion{};
      blitRegion.srcSubresource = vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
      };
      blitRegion.srcOffsets[0] = vk::Offset3D{0, 0, 0};
      blitRegion.srcOffsets[1] = vk::Offset3D{
        static_cast<int32_t>(resolution.x),
        static_cast<int32_t>(resolution.y),
        1};

      blitRegion.dstSubresource = vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
      };
      blitRegion.dstOffsets[0] = vk::Offset3D{0, 0, 0};
      blitRegion.dstOffsets[1] = vk::Offset3D{
        static_cast<int32_t>(resolution.x),
        static_cast<int32_t>(resolution.y),
        1};

      currentCmdBuf.blitImage(
        resultImage.get(),
        vk::ImageLayout::eTransferSrcOptimal,
        backbuffer,
        vk::ImageLayout::eTransferDstOptimal,
        {blitRegion},
        vk::Filter::eLinear);

      etna::set_state(
        currentCmdBuf,
        backbuffer,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        {},
        vk::ImageLayout::ePresentSrcKHR,
        vk::ImageAspectFlagBits::eColor);

      etna::flush_barriers(currentCmdBuf);
    }
    ETNA_CHECK_VK_RESULT(currentCmdBuf.end());

    auto renderingDone = commandManager->submit(
      std::move(currentCmdBuf),
      std::move(backbufferAvailableSem),
      std::move(backbufferReadyForPresentSem));

    const bool presented = vkWindow->present(std::move(renderingDone), backbufferView);

    if (!presented)
      nextSwapchainImage = std::nullopt;
  }

  etna::end_frame();

  if (!nextSwapchainImage && osWindow->getResolution() != glm::uvec2{0, 0})
  {
    auto [w, h] = vkWindow->recreateSwapchain(etna::Window::DesiredProperties{
      .resolution = {resolution.x, resolution.y},
      .vsync = useVsync,
      .numFramesInFlight = static_cast<uint32_t>(commandManager->getCmdBufferCount()),
    });
    (void)w;
    (void)h;
  }
}