#include "App.hpp"

#include <etna/Etna.hpp>
#include <etna/GlobalContext.hpp>
#include <etna/PipelineManager.hpp>

#include <spdlog/spdlog.h>
#include <GLFW/glfw3.h>


App::App()
  : resolution{1280, 720}
  , useVsync{true}
{
  {
    auto glfwInstExts = windowing.getRequiredVulkanInstanceExtensions();

    std::vector<const char*> instanceExtensions{glfwInstExts.begin(), glfwInstExts.end()};

    std::vector<const char*> deviceExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    etna::initialize(etna::InitParams{
      .applicationName = "Local Shadertoy",
      .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
      .instanceExtensions = instanceExtensions,
      .deviceExtensions = deviceExtensions,
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

    spdlog::info("recreateSwapchain returned: {}x{}, using resolution: {}x{}",
        w, h, resolution.x, resolution.y);
  }

  // === Создаём compute pipeline и изображение для результата ===

  etna::create_program(
    "toy_compute",
    {LOCAL_SHADERTOY1_SHADERS_ROOT "toy.comp.spv"});

  pipeline = etna::get_context().getPipelineManager().createComputePipeline("toy_compute", {});

  resultImage = etna::get_context().createImage(etna::Image::CreateInfo{
    .extent = vk::Extent3D{resolution.x, resolution.y, 1},
    .name = "shadertoy_result",
    .format = vk::Format::eR8G8B8A8Unorm,
    .imageUsage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc,
  });

  spdlog::info("resultImage created: {}x{}", resolution.x, resolution.y);

  startTime = glfwGetTime();
}

App::~App()
{
  ETNA_CHECK_VK_RESULT(etna::get_context().getDevice().waitIdle());
}

void App::run()
{
  bool rWasPressed = false;

  while (!osWindow->isBeingClosed())
  {
    windowing.poll();

    // === Hot-reload шейдера по кнопке R ===
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

    ETNA_CHECK_VK_RESULT(currentCmdBuf.begin(vk::CommandBufferBeginInfo{}));
    {
      // Переводим swapchain-изображение в TRANSFER_DST_OPTIMAL
      etna::set_state(
        currentCmdBuf,
        backbuffer,
        vk::PipelineStageFlagBits2::eTransfer,
        vk::AccessFlagBits2::eTransferWrite,
        vk::ImageLayout::eTransferDstOptimal,
        vk::ImageAspectFlagBits::eColor);

      // Переводим resultImage в GENERAL для записи из compute
      etna::set_state(
        currentCmdBuf,
        resultImage.get(),
        vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderWrite,
        vk::ImageLayout::eGeneral,
        vk::ImageAspectFlagBits::eColor);

      etna::flush_barriers(currentCmdBuf);

      // === Dispatch compute-шейдера ===
      auto toyInfo = etna::get_shader_program("toy_compute");

      auto set = etna::create_descriptor_set(
        toyInfo.getDescriptorLayoutId(0),
        currentCmdBuf,
        {
          etna::Binding{0, resultImage.genBinding(vk::Sampler{}, vk::ImageLayout::eGeneral)},
        });

      vk::DescriptorSet vkSet = set.getVkSet();

      currentCmdBuf.bindPipeline(
        vk::PipelineBindPoint::eCompute,
        pipeline.getVkPipeline());

      double mx = 0.0, my = 0.0;
      glfwGetCursorPos(osWindow->native(), &mx, &my);

      bool leftDown =
        glfwGetMouseButton(osWindow->native(), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

      // Push constants: iResolution, iTime, padding, iMouse
      struct PushConstants {
        glm::vec2 iResolution;   // offset 0
        float iTime;             // offset 8
        float padding = 0.0f;    // offset 12 (выравнивание)
        glm::vec4 iMouse;        // offset 16
      } pc{
        glm::vec2(static_cast<float>(resolution.x), static_cast<float>(resolution.y)),
        static_cast<float>(glfwGetTime() - startTime),
        0.0f,
        glm::vec4(
          static_cast<float>(mx),
          static_cast<float>(my),
          leftDown ? 1.0f : 0.0f,
          0.0f)
      };

      currentCmdBuf.pushConstants(
        pipeline.getVkPipelineLayout(),
        vk::ShaderStageFlagBits::eCompute,
        0, sizeof(pc), &pc);

      currentCmdBuf.bindDescriptorSets(
        vk::PipelineBindPoint::eCompute,
        pipeline.getVkPipelineLayout(),
        0, 1, &vkSet, 0, nullptr);

      currentCmdBuf.dispatch(
        (resolution.x + 31) / 32,
        (resolution.y + 31) / 32,
        1);

      // === Blit resultImage -> backbuffer ===
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

      // Переводим swapchain-изображение в PRESENT_SRC_KHR
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