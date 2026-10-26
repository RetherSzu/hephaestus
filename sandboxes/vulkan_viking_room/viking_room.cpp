// viking_room.cpp                                                    -*-C++-*-
#include <viking_room.h>

// std
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>
#include <unordered_map>

// third party
#include <vulkan/vulkan_raii.hpp>
#define GLFW_INCLUDE_VULKAN
#define GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

template <> struct std::hash<vkr::Vertex> {
    std::size_t operator()(const vkr::Vertex& vertex) const noexcept
    {
        const std::size_t posHash   = std::hash<glm::vec3>()(vertex.pos);
        const std::size_t colorHash = std::hash<glm::vec3>()(vertex.color);
        const std::size_t texHash   = std::hash<glm::vec2>()(vertex.texCoord);

        return (posHash ^ colorHash << 1U) >> 1U ^ texHash << 1U;
    }
};

constexpr int k_WINDOW_WIDTH         = 800;
constexpr int k_WINDOW_HEIGHT        = 600;
constexpr int k_MAX_FRAMES_IN_FLIGHT = 3;

constexpr auto k_TEXTURE_PATH = "textures/viking_room.png";
constexpr auto k_MODEL_PATH   = "models/viking_room.obj";

namespace vkr {
namespace {

bool checkExtensionsSupport(
    const std::vector<const char*>&             requiredExtensions,
    const std::vector<vk::ExtensionProperties>& availableExtensions)
{
    for (const auto& required : requiredExtensions) {
        const bool found = std::ranges::any_of(
            availableExtensions, [required](const auto& prop) {
                return strcmp(prop.extensionName, required) == 0;
            });

        if (!found) {
            std::cerr << "[Error] Missing required extension: " << required
                      << '\n';
            return false;
        }
    }
    return true;
}

bool checkLayersSupport(
    const std::vector<const char*>&         requiredLayers,
    const std::vector<vk::LayerProperties>& availableLayers)
{
    for (const auto& required : requiredLayers) {
        const bool found = std::ranges::any_of(
            availableLayers, [required](const auto& prop) {
                return strcmp(prop.layerName, required) == 0;
            });
        if (!found) {
            std::cerr << "[Error] Missing required layer: " << required
                      << '\n';
            return false;
        }
    }
    return true;
}

const std::vector k_deviceExtensions = {
    vk::KHRSwapchainExtensionName,
    vk::KHRSpirv14ExtensionName,
#ifdef __APPLE__
    // Required for MoltenVK (Metal translation) on macOS
    "VK_KHR_portability_subset",
#endif
};

bool checkDeviceExtensions(const vk::PhysicalDevice        device,
                           const std::vector<const char*>& requiredExtensions)
{
    const auto availableExtensions = device
                                         .enumerateDeviceExtensionProperties();
    std::set<std::string> required(requiredExtensions.begin(),
                                   requiredExtensions.end());

    for (const auto& extension : availableExtensions) {
        required.erase(extension.extensionName);
    }
    return required.empty();
}

bool checkDeviceFeatures(const vk::PhysicalDevice device)
{
    // We chain structures to request features from different Vulkan versions
    auto features =
        device
            .getFeatures2<vk::PhysicalDeviceFeatures2,
                          vk::PhysicalDeviceVulkan13Features,
                          vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>();

    const auto& basic = features.get<vk::PhysicalDeviceFeatures2>().features;
    const auto& v13   = features.get<vk::PhysicalDeviceVulkan13Features>();
    const auto& ext =
        features.get<vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>();

    return basic.samplerAnisotropy != 0u &&  // Sharper textures at angles
           v13.dynamicRendering != 0u &&  // Render without RenderPass objects
           ext.extendedDynamicState != 0u;  // More dynamic pipeline states
}

bool checkDeviceQueues(const vk::PhysicalDevice device,
                       const vk::SurfaceKHR     surface)
{
    const auto queueFamilies = device.getQueueFamilyProperties();

    for (size_t i = 0; i < queueFamilies.size(); ++i) {
        // Check for Drawing capability
        const bool supportsGraphics = static_cast<bool>(
            queueFamilies[i].queueFlags & vk::QueueFlagBits::eGraphics);

        // Check for Screen Presentation capability
        const bool supportsPresent = device.getSurfaceSupportKHR(
                                         static_cast<uint32_t>(i), surface) !=
                                     0u;

        if (supportsGraphics && supportsPresent) {
            return true;
        }
    }
    return false;
}

vk::SurfaceFormatKHR
chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& formats)
{
    for (const auto& fmt : formats) {
        // Prefer SRGB for automatic gamma-correct rendering
        if (fmt.format == vk::Format::eB8G8R8A8Srgb &&
            fmt.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
            return fmt;
        }
    }
    return formats[0];  // Fallback to the first available format
}

vk::PresentModeKHR
chooseSwapPresentMode(const std::vector<vk::PresentModeKHR>& presentModes)
{
    for (const auto& mode : presentModes) {
        // Mailbox: low latency and no tearing (triple buffering)
        if (mode == vk::PresentModeKHR::eMailbox) {
            return mode;
        }
    }
    // Guaranteed to be available (standard V-Sync)
    return vk::PresentModeKHR::eFifo;
}

vk::Extent2D chooseSwapExtent(const vk::SurfaceCapabilitiesKHR& caps,
                              const uint32_t                    width,
                              const uint32_t                    height)
{
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return caps.currentExtent;
    }

    vk::Extent2D actualExtent = {width, height};
    actualExtent.width        = std::clamp(actualExtent.width,
                                    caps.minImageExtent.width,
                                    caps.maxImageExtent.width);
    actualExtent.height       = std::clamp(actualExtent.height,
                                     caps.minImageExtent.height,
                                     caps.maxImageExtent.height);
    return actualExtent;
}

std::vector<char> readFile(const std::string& filename)
{
    std::ifstream file(filename, std::ios::ate | std::ios::binary);

    if (!file.is_open()) {
        std::cout << "failed to open file!\n";
        return {};
    }

    const auto        fileSize = static_cast<std::size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0, std::ios::beg);
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    file.close();

    return buffer;
}

void framebufferResizeCallback(GLFWwindow* window,
                               int /*width*/,
                               int /*height*/)
{
    auto* app = static_cast<VikingRoom*>(glfwGetWindowUserPointer(window));
    app->setFrameBufferResied(true);
}

uint32_t
findMemoryType(const uint32_t                            typeFilter,
               vk::MemoryPropertyFlags                   properties,
               const vk::PhysicalDeviceMemoryProperties& memProperties)
{
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        const auto maskedFlags = memProperties.memoryTypes[i].propertyFlags &
                                 properties;
        if ((typeFilter & (1U << i)) != 0u && maskedFlags == properties) {
            return i;
        }
    }

    std::cout << "failed to find suitable memory type!";
    return static_cast<uint32_t>(-1);
}

struct UniformBufferObject {
    alignas(16) glm::mat4 model;
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
};

}  // close unnamed namespace

VikingRoom::VikingRoom()
: d_instance(nullptr)
, d_surface(nullptr)
, d_physicalDevice(nullptr)
, d_graphicQueueFamilyIndex(static_cast<std::uint32_t>(-1))
, d_device(nullptr)
, d_graphicQueue(nullptr)
, d_swapChain(nullptr)
, d_format(vk::Format::eB8G8R8A8Srgb)
, d_extent({})
, d_depthImage(nullptr)
, d_depthImageMemory(nullptr)
, d_depthImageView(nullptr)
, d_textureImage(nullptr)
, d_textureImageMemory(nullptr)
, d_textureImageView(nullptr)
, d_textureSampler(nullptr)
, d_descriptorSetLayout(nullptr)
, d_pipelineLayout(nullptr)
, d_graphicsPipeline(nullptr)
, d_commandPool(nullptr)
, d_frameIndex(0)
, d_vertexBuffer(nullptr)
, d_vertexBufferMemory(nullptr)
, d_indexBuffer(nullptr)
, d_indexBufferMemory(nullptr)
, d_descriptorPool(nullptr)
, d_framebufferResized(false)
, d_window(nullptr)
{
}

void VikingRoom::run()
{
    initWindow();
    initVulkan();
    createSwapChain();
    createDescriptorSetLayout();
    createGraphicsPipeline();
    createDepthResources();
    createTextureImage();
    createTextureImageView();
    createTextureSampler();
    loadModel();
    createVertexBuffer();
    createIndexBuffer();
    createUniformBuffers();
    createDescriptorPool();
    createDescriptorSets();
    createSyncObjects();
    mainLoop();
    cleanUp();
}

void VikingRoom::initWindow()
{
    if (glfwInit() != GLFW_TRUE) {
        std::cout << "Fail to initialize the GLFW library";
    }

    // Tell GLFW to not create the OpenGL context
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    d_window = glfwCreateWindow(
        800, 600, "Viking Room - Vulkan", nullptr, nullptr);
    glfwSetWindowUserPointer(d_window, this);
    glfwSetFramebufferSizeCallback(d_window, framebufferResizeCallback);
}

void VikingRoom::initVulkan()
{
    vk::ApplicationInfo appInfo{};
    appInfo.pApplicationName   = "Viking Room";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion         = vk::ApiVersion14;

    std::vector<const char*> extensions;
    uint32_t                 glfwExtensionCount = 0;
    const char**             glfwExt = glfwGetRequiredInstanceExtensions(
        &glfwExtensionCount);
    extensions.insert(extensions.end(), glfwExt, glfwExt + glfwExtensionCount);
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#ifdef __APPLE__
    extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif

    if (!checkExtensionsSupport(
            extensions, d_context.enumerateInstanceExtensionProperties())) {
        std::cerr << "Failed to find all required extensions!\n";
        return;
    }

    std::vector<const char*> layers;
    layers.push_back("VK_LAYER_KHRONOS_validation");
    if (!checkLayersSupport(layers,
                            d_context.enumerateInstanceLayerProperties())) {
        std::cerr << "Failed to find all required layers!\n";
    }

    vk::InstanceCreateInfo createInfo{};
#ifdef __APPLE__
    // MoltenVK is a portability driver; the loader hides it without this flag
    createInfo.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
#endif
    createInfo.pApplicationInfo      = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(
        extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    createInfo.enabledLayerCount       = static_cast<uint32_t>(layers.size());
    createInfo.ppEnabledLayerNames     = layers.data();

    d_instance = vk::raii::Instance(d_context, createInfo);

    VkSurfaceKHR surface;
    if (glfwCreateWindowSurface(*d_instance, d_window, nullptr, &surface) !=
        VK_SUCCESS) {
        std::cerr << "Failed to create window surface.\n";
        return;
    }
    d_surface = vk::raii::SurfaceKHR(d_instance, surface);

    std::vector<vk::raii::PhysicalDevice> devices =
        d_instance.enumeratePhysicalDevices();

    const auto devIter = std::ranges::find_if(
        devices, [&](auto const& device) {
            // Check if the device supports the Vulkan 1.3 API version.
            if (device.getProperties().apiVersion < VK_API_VERSION_1_3) {
                return false;
            }

            // Check if all required device extensions are available.
            if (!checkDeviceExtensions(device, k_deviceExtensions)) {
                return false;
            }

            // Check if any of the queue families support graphics operations.
            if (!checkDeviceQueues(device, *d_surface)) {
                return false;
            }

            // Check if the device supports rendering features we plan to use.
            if (!checkDeviceFeatures(device)) {
                return false;
            }

            return true;
        });

    if (devIter != devices.end()) {
        d_physicalDevice = *devIter;
    } else {
        std::cerr << "Failed to find a supported physical device\n";
        return;
    }

    // Enumerate all the properties of the queue family for the selected GPU.
    const auto queueFamilies = d_physicalDevice.getQueueFamilyProperties();
    for (uint32_t i = 0; i < queueFamilies.size(); i++) {
        // Check whether the family supports graphics operations.
        const vk::Flags<vk::QueueFlagBits> supportsGraphics =
            queueFamilies[i].queueFlags & vk::QueueFlagBits::eGraphics;

        // Check whether the family supports the display on our surface.
        const bool supportsPresent = d_physicalDevice.getSurfaceSupportKHR(
                                         i, *d_surface) != 0u;

        if (supportsGraphics && supportsPresent) {
            d_graphicQueueFamilyIndex = i;
            break;
        }
    }

    if (d_graphicQueueFamilyIndex == static_cast<std::uint32_t>(-1)) {
        std::cerr << "No queue family found that supports both Graphics and "
                     "Presentation!\n";
        return;
    }

    constexpr float                 queuePriority = 1.0f;
    const vk::DeviceQueueCreateInfo queueCreateInfo({},
                                                    d_graphicQueueFamilyIndex,
                                                    1,
                                                    &queuePriority);

    // Query physical device features first.
    auto supportedFeatures =
        d_physicalDevice
            .getFeatures2<vk::PhysicalDeviceFeatures2,
                          vk::PhysicalDeviceVulkan11Features,
                          vk::PhysicalDeviceVulkan13Features,
                          vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>();

    const auto& supportedVk13Features =
        supportedFeatures.get<vk::PhysicalDeviceVulkan13Features>();

    // Setup Features - explicitly enable features if supported.
    vk::PhysicalDeviceVulkan13Features vk13Features{};

    if (supportedVk13Features.dynamicRendering != 0u) {
        vk13Features.dynamicRendering = VK_TRUE;
    }

    if (supportedVk13Features.synchronization2 != 0u) {
        vk13Features.synchronization2 = VK_TRUE;
    }

    vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT extendedDynamicState{};
    const auto& supportedExtendedDynamicState =
        supportedFeatures
            .get<vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>();
    if (supportedExtendedDynamicState.extendedDynamicState != 0u) {
        extendedDynamicState.extendedDynamicState = VK_TRUE;
    }

    // Setup base features (samplerAnisotropy)
    vk::PhysicalDeviceFeatures2 baseFeatures{};
    const auto&                 supportedFeatures2 = supportedFeatures
                                         .get<vk::PhysicalDeviceFeatures2>();

    if (supportedFeatures2.features.samplerAnisotropy != 0u) {
        baseFeatures.features.samplerAnisotropy = VK_TRUE;
    }

    if (supportedFeatures2.features.largePoints != 0u) {
        baseFeatures.features.largePoints = VK_TRUE;
    }

    // Enable non-solid fill mode to support WireFrame rendering.
    if (supportedFeatures2.features.fillModeNonSolid != 0u) {
        baseFeatures.features.fillModeNonSolid = VK_TRUE;
    }

    vk::PhysicalDeviceVulkan11Features vk11Features{};
    const auto&                        supportedVk11Features =
        supportedFeatures.get<vk::PhysicalDeviceVulkan11Features>();
    if (supportedVk11Features.shaderDrawParameters != 0u) {
        vk11Features.shaderDrawParameters = VK_TRUE;
    }

    // Manually set up the pNext chain to ensure it's correct.
    extendedDynamicState.pNext = nullptr;
    vk13Features.pNext         = &extendedDynamicState;
    vk11Features.pNext         = &vk13Features;
    baseFeatures.pNext         = &vk11Features;

    // Setup Device Info
    vk::DeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.pNext                 = &baseFeatures;
    deviceCreateInfo.queueCreateInfoCount  = 1;
    deviceCreateInfo.pQueueCreateInfos     = &queueCreateInfo;
    deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(
        k_deviceExtensions.size());
    deviceCreateInfo.ppEnabledExtensionNames = k_deviceExtensions.data();

    // Create the Logical Device.
    d_device = vk::raii::Device(d_physicalDevice, deviceCreateInfo);

    // Retrieve the handle to the Graphics Queue.
    d_graphicQueue = vk::raii::Queue(d_device, d_graphicQueueFamilyIndex, 0);

    vk::CommandPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    poolInfo.queueFamilyIndex = d_graphicQueueFamilyIndex;

    d_commandPool = vk::raii::CommandPool(d_device, poolInfo);

    vk::CommandBufferAllocateInfo allocInfo{};
    allocInfo.commandPool        = d_commandPool;
    allocInfo.level              = vk::CommandBufferLevel::ePrimary;
    allocInfo.commandBufferCount = k_MAX_FRAMES_IN_FLIGHT;

    d_commandBuffers = vk::raii::CommandBuffers(d_device, allocInfo);
}

void VikingRoom::createSwapChain()
{
    const auto caps = d_physicalDevice.getSurfaceCapabilitiesKHR(*d_surface);
    const auto formats = d_physicalDevice.getSurfaceFormatsKHR(*d_surface);
    const auto presentModes = d_physicalDevice.getSurfacePresentModesKHR(
        *d_surface);

    const auto surfaceFormat = chooseSwapSurfaceFormat(formats);
    const auto presentMode   = chooseSwapPresentMode(presentModes);
    const auto extent        = chooseSwapExtent(caps,
                                         k_WINDOW_WIDTH,
                                         k_WINDOW_HEIGHT);

    // One more than the minimum so the driver never stalls (triple buffering
    // when the hardware allows it).
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    vk::SwapchainCreateInfoKHR createSwapchainInfo(
        {},
        *d_surface,
        imageCount,
        surfaceFormat.format,
        surfaceFormat.colorSpace,
        extent,
        1,  // imageArrayLayers (1 for standard 2D, 2 for VR stereo).
        vk::ImageUsageFlagBits::eColorAttachment);

    createSwapchainInfo.imageSharingMode = vk::SharingMode::eExclusive;
    createSwapchainInfo.preTransform     = caps.currentTransform;
    createSwapchainInfo.compositeAlpha =
        vk::CompositeAlphaFlagBitsKHR::eOpaque;
    createSwapchainInfo.presentMode  = presentMode;
    createSwapchainInfo.clipped      = VK_TRUE;
    createSwapchainInfo.oldSwapchain = *d_swapChain;  // Recycle on resize.

    d_swapChain = vk::raii::SwapchainKHR(d_device, createSwapchainInfo);

    // Cache for later use (pipelines, camera, recreation).
    d_format = surfaceFormat.format;
    d_extent = extent;

    d_swapChainImages = d_swapChain.getImages();

    d_swapChainImageViews.clear();
    d_swapChainImageViews.reserve(d_swapChainImages.size());

    for (const auto& image : d_swapChainImages) {
        d_swapChainImageViews.emplace_back(
            createImageView(image, d_format, vk::ImageAspectFlagBits::eColor));
    }
}


void VikingRoom::recreateSwapChain()
{
    int width  = 0;
    int height = 0;

    glfwGetFramebufferSize(d_window, &width, &height);

    while ((width == 0 || height == 0) &&
           glfwWindowShouldClose(d_window) == 0) {
        glfwGetFramebufferSize(d_window, &width, &height);
        glfwWaitEvents();
    }

    if (glfwWindowShouldClose(d_window) != 0) {
        return;
    }

    d_device.waitIdle();

    d_swapChainImageViews.clear();
    d_swapChain = nullptr;
    createSwapChain();
    createDepthResources();
}

void VikingRoom::createDescriptorSetLayout()
{
    vk::DescriptorSetLayoutBinding uboLayoutBinding{};
    uboLayoutBinding.binding         = 0;
    uboLayoutBinding.descriptorType  = vk::DescriptorType::eUniformBuffer;
    uboLayoutBinding.descriptorCount = 1;
    uboLayoutBinding.stageFlags      = vk::ShaderStageFlagBits::eVertex;

    vk::DescriptorSetLayoutBinding samplerLayoutBinding{};
    samplerLayoutBinding.binding = 1;
    samplerLayoutBinding.descriptorType =
        vk::DescriptorType::eCombinedImageSampler;
    samplerLayoutBinding.descriptorCount = 1;
    samplerLayoutBinding.stageFlags      = vk::ShaderStageFlagBits::eFragment;

    const std::array bindings = {uboLayoutBinding, samplerLayoutBinding};

    vk::DescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();

    d_descriptorSetLayout = vk::raii::DescriptorSetLayout(d_device,
                                                          layoutInfo);
}

void VikingRoom::createGraphicsPipeline()
{
    // load the bytecode of the two shaders
    const auto shaderCode = readFile("shaders/shader.spv");

    // create shader module wrapper
    vk::ShaderModuleCreateInfo createInfo{};
    createInfo.codeSize = shaderCode.size() * sizeof(char);
    createInfo.pCode    = reinterpret_cast<const uint32_t*>(shaderCode.data());

    const vk::raii::ShaderModule shaderModule = {d_device, createInfo};

    // assign our shader to a stage of the pipeline
    vk::PipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.stage  = vk::ShaderStageFlagBits::eVertex;
    vertShaderStageInfo.module = shaderModule;
    vertShaderStageInfo.pName  = "vertMain";

    vk::PipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.stage  = vk::ShaderStageFlagBits::eFragment;
    fragShaderStageInfo.module = shaderModule;
    fragShaderStageInfo.pName  = "fragMain";

    // save our assignement for later usage on the pipeline creation
    const vk::PipelineShaderStageCreateInfo shaderStages[] =
        {vertShaderStageInfo, fragShaderStageInfo};

    auto bindingDescription    = Vertex::getBindingDescription();
    auto attributeDescriptions = Vertex::getAttributeDescriptions();

    vk::PipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.vertexBindingDescriptionCount   = 1;
    vertexInputInfo.pVertexBindingDescriptions      = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(
        attributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions
                                                       .data();

    const std::vector dynamicStates = {
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
    };

    vk::PipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.dynamicStateCount = static_cast<uint32_t>(
        dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    const vk::Viewport viewport{0.0f,
                                0.0f,
                                static_cast<float>(d_extent.width),
                                static_cast<float>(d_extent.height),
                                0.0f,
                                1.0f};
    const vk::Rect2D   scissor = {vk::Offset2D{0, 0}, d_extent};

    vk::PipelineViewportStateCreateInfo viewportState{};
    viewportState.viewportCount = 1;
    viewportState.pViewports    = &viewport;
    viewportState.scissorCount  = 1;
    viewportState.pScissors     = &scissor;

    vk::PipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.topology = vk::PrimitiveTopology::eTriangleList;

    vk::PipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.depthClampEnable        = vk::False;
    rasterizer.rasterizerDiscardEnable = vk::False;
    rasterizer.polygonMode             = vk::PolygonMode::eFill;
    rasterizer.cullMode                = vk::CullModeFlagBits::eBack;
    rasterizer.frontFace               = vk::FrontFace::eCounterClockwise;
    rasterizer.depthBiasEnable         = vk::False;
    rasterizer.lineWidth               = 1.0f;

    vk::PipelineMultisampleStateCreateInfo multisampling{};
    multisampling.rasterizationSamples = vk::SampleCountFlagBits::e1;
    multisampling.sampleShadingEnable  = vk::False;

    vk::PipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.depthTestEnable       = vk::True;
    depthStencil.depthWriteEnable      = vk::True;
    depthStencil.depthCompareOp        = vk::CompareOp::eLess;
    depthStencil.depthBoundsTestEnable = vk::False;
    depthStencil.stencilTestEnable     = vk::False;

    vk::PipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.blendEnable    = vk::False;
    colorBlendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR |
                                          vk::ColorComponentFlagBits::eG |
                                          vk::ColorComponentFlagBits::eB |
                                          vk::ColorComponentFlagBits::eA;

    vk::PipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.logicOpEnable   = vk::False;
    colorBlending.logicOp         = vk::LogicOp::eCopy;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments    = &colorBlendAttachment;

    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.setLayoutCount         = 1;
    pipelineLayoutInfo.pSetLayouts            = &*d_descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 0;

    d_pipelineLayout = vk::raii::PipelineLayout(d_device, pipelineLayoutInfo);

    const vk::Format depthFormat = findDepthFormat();

    vk::GraphicsPipelineCreateInfo graphicsPipelineCreateInfo{};
    graphicsPipelineCreateInfo.stageCount          = 2;
    graphicsPipelineCreateInfo.pStages             = shaderStages;
    graphicsPipelineCreateInfo.pVertexInputState   = &vertexInputInfo;
    graphicsPipelineCreateInfo.pInputAssemblyState = &inputAssembly;
    graphicsPipelineCreateInfo.pViewportState      = &viewportState;
    graphicsPipelineCreateInfo.pRasterizationState = &rasterizer;
    graphicsPipelineCreateInfo.pMultisampleState   = &multisampling;
    graphicsPipelineCreateInfo.pDepthStencilState  = &depthStencil;
    graphicsPipelineCreateInfo.pColorBlendState    = &colorBlending;
    graphicsPipelineCreateInfo.pDynamicState       = &dynamicState;
    graphicsPipelineCreateInfo.layout              = d_pipelineLayout;
    graphicsPipelineCreateInfo.renderPass          = nullptr;

    vk::PipelineRenderingCreateInfo pipelineRenderingCreateInfo{};
    pipelineRenderingCreateInfo.colorAttachmentCount    = 1;
    pipelineRenderingCreateInfo.pColorAttachmentFormats = &d_format;
    pipelineRenderingCreateInfo.depthAttachmentFormat   = depthFormat;

    const vk::StructureChain
        pipelineCreateInfoChain{graphicsPipelineCreateInfo,
                                pipelineRenderingCreateInfo};
    d_graphicsPipeline = vk::raii::Pipeline(
        d_device,
        nullptr,
        pipelineCreateInfoChain.get<vk::GraphicsPipelineCreateInfo>());
}

void VikingRoom::createDepthResources()
{
    const vk::Format depthFormat = findDepthFormat();

    std::tie(d_depthImage, d_depthImageMemory) = createImage(
        d_extent.width,
        d_extent.height,
        depthFormat,
        vk::ImageTiling::eOptimal,
        vk::ImageUsageFlagBits::eDepthStencilAttachment,
        vk::MemoryPropertyFlagBits::eDeviceLocal);

    d_depthImageView = createImageView(*d_depthImage,
                                       depthFormat,
                                       vk::ImageAspectFlagBits::eDepth);
}

vk::Format
VikingRoom::findSupportedFormat(const std::vector<vk::Format>& candidates,
                                const vk::ImageTiling          tiling,
                                const vk::FormatFeatureFlags   features) const
{
    for (const auto format : candidates) {
        const vk::FormatProperties props = d_physicalDevice
                                               .getFormatProperties(format);
        const auto linearFeatures  = props.linearTilingFeatures & features;
        const auto optimalFeatures = props.optimalTilingFeatures & features;
        if ((tiling == vk::ImageTiling::eLinear &&
             linearFeatures == features) ||
            (tiling == vk::ImageTiling::eOptimal &&
             optimalFeatures == features)) {
            return format;
        }
    }

    std::cout << "failed to find supported format!\n";
    return vk::Format::eUndefined;
}

vk::Format VikingRoom::findDepthFormat() const
{
    return findSupportedFormat(
        {
            vk::Format::eD32Sfloat,
            vk::Format::eD32SfloatS8Uint,
            vk::Format::eD24UnormS8Uint,
        },
        vk::ImageTiling::eOptimal,
        vk::FormatFeatureFlagBits::eDepthStencilAttachment);
}

void VikingRoom::createTextureImage()
{
    int texWidth    = 0;
    int texHeight   = 0;
    int texChannels = 0;

    stbi_uc* pixels = stbi_load(
        k_TEXTURE_PATH, &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
    if (pixels == nullptr) {
        std::cout << "failed to load texture image!\n";
        return;
    }

    const auto imageSize = static_cast<vk::DeviceSize>(texWidth) *
                           static_cast<vk::DeviceSize>(texHeight) * 4u;

    auto [stagingBuffer, stagingBufferMemory] = createBuffer(
        imageSize,
        vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent);

    void* data = stagingBufferMemory.mapMemory(0, imageSize);
    memcpy(data, pixels, imageSize);
    stagingBufferMemory.unmapMemory();

    stbi_image_free(pixels);

    std::tie(d_textureImage, d_textureImageMemory) = createImage(
        static_cast<uint32_t>(texWidth),
        static_cast<uint32_t>(texHeight),
        vk::Format::eR8G8B8A8Srgb,
        vk::ImageTiling::eOptimal,
        vk::ImageUsageFlagBits::eTransferDst |
            vk::ImageUsageFlagBits::eSampled,
        vk::MemoryPropertyFlagBits::eDeviceLocal);

    const vk::raii::CommandBuffer commandBuffer = beginSingleTimeCommands();
    transitionImageLayout(commandBuffer,
                          d_textureImage,
                          vk::ImageLayout::eUndefined,
                          vk::ImageLayout::eTransferDstOptimal);
    copyBufferToImage(commandBuffer,
                      stagingBuffer,
                      d_textureImage,
                      static_cast<uint32_t>(texWidth),
                      static_cast<uint32_t>(texHeight));
    transitionImageLayout(commandBuffer,
                          d_textureImage,
                          vk::ImageLayout::eTransferDstOptimal,
                          vk::ImageLayout::eShaderReadOnlyOptimal);
    endSingleTimeCommands(std::move(commandBuffer));
}

void VikingRoom::createTextureImageView()
{
    d_textureImageView = createImageView(*d_textureImage,
                                         vk::Format::eR8G8B8A8Srgb,
                                         vk::ImageAspectFlagBits::eColor);
}

void VikingRoom::createTextureSampler()
{
    const vk::PhysicalDeviceProperties properties = d_physicalDevice
                                                        .getProperties();

    vk::SamplerCreateInfo samplerInfo{};
    samplerInfo.magFilter        = vk::Filter::eLinear;
    samplerInfo.minFilter        = vk::Filter::eLinear;
    samplerInfo.mipmapMode       = vk::SamplerMipmapMode::eLinear;
    samplerInfo.addressModeU     = vk::SamplerAddressMode::eRepeat;
    samplerInfo.addressModeV     = vk::SamplerAddressMode::eRepeat;
    samplerInfo.addressModeW     = vk::SamplerAddressMode::eRepeat;
    samplerInfo.mipLodBias       = 0.0f;
    samplerInfo.anisotropyEnable = vk::True;
    samplerInfo.maxAnisotropy    = properties.limits.maxSamplerAnisotropy;
    samplerInfo.compareEnable    = vk::False;
    samplerInfo.compareOp        = vk::CompareOp::eAlways;

    d_textureSampler = vk::raii::Sampler(d_device, samplerInfo);
}

void VikingRoom::loadModel()
{
    tinyobj::attrib_t                attrib;
    std::vector<tinyobj::shape_t>    shapes;
    std::vector<tinyobj::material_t> materials;
    std::string                      warn;
    std::string                      err;

    if (!tinyobj::LoadObj(
            &attrib, &shapes, &materials, &warn, &err, k_MODEL_PATH)) {
        std::cout << "failed to load model: " << warn << err << '\n';
        return;
    }

    std::unordered_map<Vertex, uint32_t> uniqueVertices;

    for (const auto& shape : shapes) {
        for (const auto& index : shape.mesh.indices) {
            const auto vi = static_cast<size_t>(index.vertex_index);
            const auto ti = static_cast<size_t>(index.texcoord_index);

            Vertex vertex{};
            vertex.pos      = {attrib.vertices[(3u * vi) + 0u],
                               attrib.vertices[(3u * vi) + 1u],
                               attrib.vertices[(3u * vi) + 2u]};
            vertex.texCoord = {attrib.texcoords[(2u * ti) + 0u],
                               1.0f - attrib.texcoords[(2u * ti) + 1u]};
            vertex.color    = {1.0f, 1.0f, 1.0f};

            // Reuse a vertex index when the exact same vertex was seen before.
            auto [it, inserted] = uniqueVertices.try_emplace(
                vertex, static_cast<uint32_t>(d_vertices.size()));
            if (inserted) {
                d_vertices.push_back(vertex);
            }
            d_indices.push_back(it->second);
        }
    }
}

void VikingRoom::createVertexBuffer()
{
    const vk::DeviceSize bufferSize = sizeof(Vertex) * d_vertices.size();

    auto [stagingBuffer, stagingBufferMemory] = createBuffer(
        bufferSize,
        vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent);

    void* data = stagingBufferMemory.mapMemory(0, bufferSize);
    memcpy(data, d_vertices.data(), bufferSize);
    stagingBufferMemory.unmapMemory();

    std::tie(d_vertexBuffer, d_vertexBufferMemory) = createBuffer(
        bufferSize,
        vk::BufferUsageFlagBits::eVertexBuffer |
            vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal);

    copyBuffer(stagingBuffer, d_vertexBuffer, bufferSize);
}

void VikingRoom::createIndexBuffer()
{
    const vk::DeviceSize bufferSize = sizeof(d_indices[0]) * d_indices.size();

    auto [stagingBuffer, stagingBufferMemory] = createBuffer(
        bufferSize,
        vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent);

    void* data = stagingBufferMemory.mapMemory(0, bufferSize);
    memcpy(data, d_indices.data(), bufferSize);
    stagingBufferMemory.unmapMemory();

    std::tie(d_indexBuffer, d_indexBufferMemory) = createBuffer(
        bufferSize,
        vk::BufferUsageFlagBits::eIndexBuffer |
            vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal);

    copyBuffer(stagingBuffer, d_indexBuffer, bufferSize);
}

void VikingRoom::createUniformBuffers()
{
    for (size_t i = 0; i < k_MAX_FRAMES_IN_FLIGHT; i++) {
        constexpr vk::DeviceSize uboBufferSize = sizeof(UniformBufferObject);
        auto [buffer, bufferMem]               = createBuffer(
            uboBufferSize,
            vk::BufferUsageFlagBits::eUniformBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent);
        d_uniformBuffers.emplace_back(std::move(buffer));
        d_uniformBuffersMemory.emplace_back(std::move(bufferMem));
        d_uniformBuffersMapped.emplace_back(
            d_uniformBuffersMemory.back().mapMemory(0, uboBufferSize));
    }
}

void VikingRoom::createDescriptorPool()
{
    std::array<vk::DescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type            = vk::DescriptorType::eUniformBuffer;
    poolSizes[0].descriptorCount = k_MAX_FRAMES_IN_FLIGHT;
    poolSizes[1].type            = vk::DescriptorType::eCombinedImageSampler;
    poolSizes[1].descriptorCount = k_MAX_FRAMES_IN_FLIGHT;

    vk::DescriptorPoolCreateInfo descriptorPoolInfo{};
    descriptorPoolInfo.flags =
        vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    descriptorPoolInfo.maxSets       = k_MAX_FRAMES_IN_FLIGHT;
    descriptorPoolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    descriptorPoolInfo.pPoolSizes    = poolSizes.data();

    d_descriptorPool = vk::raii::DescriptorPool(d_device, descriptorPoolInfo);
}

void VikingRoom::createDescriptorSets()
{
    const std::vector layouts(k_MAX_FRAMES_IN_FLIGHT, *d_descriptorSetLayout);

    vk::DescriptorSetAllocateInfo descriptorSetAllocInfo{};
    descriptorSetAllocInfo.descriptorPool     = d_descriptorPool;
    descriptorSetAllocInfo.descriptorSetCount = static_cast<uint32_t>(
        layouts.size());
    descriptorSetAllocInfo.pSetLayouts = layouts.data();

    d_descriptorSets = d_device.allocateDescriptorSets(descriptorSetAllocInfo);

    for (size_t i = 0; i < k_MAX_FRAMES_IN_FLIGHT; i++) {
        vk::DescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = d_uniformBuffers[i];
        bufferInfo.offset = 0;
        bufferInfo.range  = sizeof(UniformBufferObject);

        vk::DescriptorImageInfo imageInfo{};
        imageInfo.sampler     = d_textureSampler;
        imageInfo.imageView   = d_textureImageView;
        imageInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

        std::array<vk::WriteDescriptorSet, 2> descriptorWrites{};
        descriptorWrites[0].dstSet          = d_descriptorSets[i];
        descriptorWrites[0].dstBinding      = 0;
        descriptorWrites[0].dstArrayElement = 0;
        descriptorWrites[0].descriptorCount = 1;
        descriptorWrites[0].descriptorType =
            vk::DescriptorType::eUniformBuffer;
        descriptorWrites[0].pBufferInfo = &bufferInfo;

        descriptorWrites[1].dstSet          = d_descriptorSets[i];
        descriptorWrites[1].dstBinding      = 1;
        descriptorWrites[1].dstArrayElement = 0;
        descriptorWrites[1].descriptorCount = 1;
        descriptorWrites[1].descriptorType =
            vk::DescriptorType::eCombinedImageSampler;
        descriptorWrites[1].pImageInfo = &imageInfo;

        d_device.updateDescriptorSets(descriptorWrites, {});
    }
}

std::pair<vk::raii::Buffer, vk::raii::DeviceMemory>
VikingRoom::createBuffer(const vk::DeviceSize          size,
                         const vk::BufferUsageFlags    usage,
                         const vk::MemoryPropertyFlags properties) const
{
    vk::BufferCreateInfo bufferInfo{};
    bufferInfo.size        = size;
    bufferInfo.usage       = usage;
    bufferInfo.sharingMode = vk::SharingMode::eExclusive;
    auto buffer            = vk::raii::Buffer(d_device, bufferInfo);

    const vk::MemoryRequirements memRequirements =
        buffer.getMemoryRequirements();

    vk::MemoryAllocateInfo allocInfo{};
    allocInfo.allocationSize  = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(
        memRequirements.memoryTypeBits,
        properties,
        d_physicalDevice.getMemoryProperties());

    auto bufferMemory = vk::raii::DeviceMemory(d_device, allocInfo);
    buffer.bindMemory(*bufferMemory, 0);

    return {std::move(buffer), std::move(bufferMemory)};
}

std::pair<vk::raii::Image, vk::raii::DeviceMemory>
VikingRoom::createImage(const std::uint32_t           width,
                        const std::uint32_t           height,
                        const vk::Format              format,
                        const vk::ImageTiling         tiling,
                        const vk::ImageUsageFlags     usage,
                        const vk::MemoryPropertyFlags properties) const
{
    vk::ImageCreateInfo imageInfo{};
    imageInfo.imageType   = vk::ImageType::e2D;
    imageInfo.format      = format;
    imageInfo.extent      = vk::Extent3D{width, height, 1};
    imageInfo.mipLevels   = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples     = vk::SampleCountFlagBits::e1;
    imageInfo.tiling      = tiling;
    imageInfo.usage       = usage;
    imageInfo.sharingMode = vk::SharingMode::eExclusive;

    auto image = vk::raii::Image(d_device, imageInfo);

    const vk::MemoryRequirements memRequirements =
        image.getMemoryRequirements();

    vk::MemoryAllocateInfo allocInfo{};
    allocInfo.allocationSize  = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(
        memRequirements.memoryTypeBits,
        properties,
        d_physicalDevice.getMemoryProperties());

    auto imageMemory = vk::raii::DeviceMemory(d_device, allocInfo);
    image.bindMemory(*imageMemory, 0);

    return {std::move(image), std::move(imageMemory)};
}

vk::raii::ImageView
VikingRoom::createImageView(const vk::Image            image,
                            const vk::Format           format,
                            const vk::ImageAspectFlags aspectFlags) const
{
    vk::ImageViewCreateInfo viewInfo{};
    viewInfo.image            = image;
    viewInfo.viewType         = vk::ImageViewType::e2D;
    viewInfo.format           = format;
    viewInfo.subresourceRange = vk::ImageSubresourceRange{aspectFlags,
                                                          0,
                                                          1,
                                                          0,
                                                          1};

    return {d_device, viewInfo};
}

vk::raii::CommandBuffer VikingRoom::beginSingleTimeCommands() const
{
    vk::CommandBufferAllocateInfo allocInfo{};
    allocInfo.commandPool        = d_commandPool;
    allocInfo.level              = vk::CommandBufferLevel::ePrimary;
    allocInfo.commandBufferCount = 1;

    vk::raii::CommandBuffer commandBuffer = std::move(
        d_device.allocateCommandBuffers(allocInfo).front());

    vk::CommandBufferBeginInfo beginInfo{};
    beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    commandBuffer.begin(beginInfo);

    return commandBuffer;
}

void VikingRoom::endSingleTimeCommands(
    const vk::raii::CommandBuffer& commandBuffer) const
{
    commandBuffer.end();

    vk::SubmitInfo submitInfo{};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &*commandBuffer;

    d_graphicQueue.submit(submitInfo, nullptr);
    d_graphicQueue.waitIdle();
}

void VikingRoom::createSyncObjects()
{
    assert(d_presentCompleteSemaphores.empty() &&
           d_renderFinishedSemaphores.empty() && d_inFlightFences.empty());

    for (size_t i = 0; i < d_swapChainImages.size(); i++) {
        d_renderFinishedSemaphores.emplace_back(d_device,
                                                vk::SemaphoreCreateInfo());
    }

    vk::FenceCreateInfo fenceInfo{};
    fenceInfo.flags = vk::FenceCreateFlagBits::eSignaled;

    for (size_t i = 0; i < k_MAX_FRAMES_IN_FLIGHT; i++) {
        d_presentCompleteSemaphores.emplace_back(d_device,
                                                 vk::SemaphoreCreateInfo());
        d_inFlightFences.emplace_back(d_device, fenceInfo);
    }
}

void VikingRoom::recordCommandBuffer(const uint32_t imageIndex) const
{
    const auto& commandBuffer = d_commandBuffers[d_frameIndex];
    commandBuffer.begin({});

    // Transition the swapchain image for color attachment writes.
    transitionImageLayout(d_swapChainImages[imageIndex],
                          vk::ImageLayout::eUndefined,
                          vk::ImageLayout::eColorAttachmentOptimal,
                          {},
                          vk::AccessFlagBits2::eColorAttachmentWrite,
                          vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                          vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                          vk::ImageAspectFlagBits::eColor);

    // Transition the depth image for depth attachment writes.
    transitionImageLayout(*d_depthImage,
                          vk::ImageLayout::eUndefined,
                          vk::ImageLayout::eDepthAttachmentOptimal,
                          {},
                          vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
                          vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                              vk::PipelineStageFlagBits2::eLateFragmentTests,
                          vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                              vk::PipelineStageFlagBits2::eLateFragmentTests,
                          vk::ImageAspectFlagBits::eDepth);

    constexpr vk::ClearValue clearColor = vk::ClearColorValue(0.0f,
                                                              0.0f,
                                                              0.0f,
                                                              1.0f);
    constexpr vk::ClearValue clearDepth = vk::ClearDepthStencilValue(1.0f, 0);

    vk::RenderingAttachmentInfo colorAttachmentInfo = {};
    colorAttachmentInfo.imageView   = d_swapChainImageViews[imageIndex];
    colorAttachmentInfo.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    colorAttachmentInfo.loadOp      = vk::AttachmentLoadOp::eClear;
    colorAttachmentInfo.storeOp     = vk::AttachmentStoreOp::eStore;
    colorAttachmentInfo.clearValue  = clearColor;

    vk::RenderingAttachmentInfo depthAttachmentInfo = {};
    depthAttachmentInfo.imageView                   = d_depthImageView;
    depthAttachmentInfo.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
    depthAttachmentInfo.loadOp      = vk::AttachmentLoadOp::eClear;
    depthAttachmentInfo.storeOp     = vk::AttachmentStoreOp::eDontCare;
    depthAttachmentInfo.clearValue  = clearDepth;

    vk::RenderingInfo renderingInfo    = {};
    renderingInfo.renderArea.offset.x  = 0;
    renderingInfo.renderArea.offset.y  = 0;
    renderingInfo.renderArea.extent    = d_extent;
    renderingInfo.layerCount           = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments    = &colorAttachmentInfo;
    renderingInfo.pDepthAttachment     = &depthAttachmentInfo;

    commandBuffer.beginRendering(renderingInfo);
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
                               *d_graphicsPipeline);

    commandBuffer.bindVertexBuffers(0, *d_vertexBuffer, {0});
    commandBuffer.bindIndexBuffer(*d_indexBuffer, 0, vk::IndexType::eUint32);

    commandBuffer.setViewport(
        0,
        vk::Viewport(0.0f,
                     static_cast<float>(d_extent.height),
                     static_cast<float>(d_extent.width),
                     -static_cast<float>(d_extent.height),
                     0.0f,
                     1.0f));
    commandBuffer.setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), d_extent));

    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                     d_pipelineLayout,
                                     0,
                                     *d_descriptorSets[d_frameIndex],
                                     nullptr);
    commandBuffer.drawIndexed(
        static_cast<uint32_t>(d_indices.size()), 1, 0, 0, 0);
    commandBuffer.endRendering();

    // Transition the swapchain image back for presentation.
    transitionImageLayout(d_swapChainImages[imageIndex],
                          vk::ImageLayout::eColorAttachmentOptimal,
                          vk::ImageLayout::ePresentSrcKHR,
                          vk::AccessFlagBits2::eColorAttachmentWrite,
                          {},
                          vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                          vk::PipelineStageFlagBits2::eBottomOfPipe,
                          vk::ImageAspectFlagBits::eColor);

    commandBuffer.end();
}

void VikingRoom::updateUniformBuffer(const uint32_t imageIndex) const
{
    static auto startTime = std::chrono::high_resolution_clock::now();

    const auto  currentTime = std::chrono::high_resolution_clock::now();
    const float time = std::chrono::duration<float>(currentTime - startTime)
                           .count();

    UniformBufferObject ubo{};
    ubo.model = rotate(glm::mat4(1.0f),
                       time * glm::radians(90.0f),
                       glm::vec3(0.0f, 0.0f, 1.0f));
    ubo.view  = lookAt(glm::vec3(2.0f, 2.0f, 2.0f),
                      glm::vec3(0.0f, 0.0f, 0.0f),
                      glm::vec3(0.0f, 0.0f, 1.0f));

    ubo.proj = glm::perspective(glm::radians(45.0f),
                                static_cast<float>(d_extent.width) /
                                    static_cast<float>(d_extent.height),
                                0.1f,
                                10.0f);

    memcpy(d_uniformBuffersMapped[imageIndex], &ubo, sizeof(ubo));
}

void VikingRoom::transitionImageLayout(
    const vk::Image               image,
    const vk::ImageLayout         oldLayout,
    const vk::ImageLayout         newLayout,
    const vk::AccessFlags2        srcAccessMask,
    const vk::AccessFlags2        dstAccessMask,
    const vk::PipelineStageFlags2 srcStageMask,
    const vk::PipelineStageFlags2 dstStageMask,
    const vk::ImageAspectFlags    aspectMask) const
{
    vk::ImageMemoryBarrier2 barrier         = {};
    barrier.srcStageMask                    = srcStageMask;
    barrier.srcAccessMask                   = srcAccessMask;
    barrier.dstStageMask                    = dstStageMask;
    barrier.dstAccessMask                   = dstAccessMask;
    barrier.oldLayout                       = oldLayout;
    barrier.newLayout                       = newLayout;
    barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.image                           = image;
    barrier.subresourceRange.aspectMask     = aspectMask;
    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = 1;

    vk::DependencyInfo dependencyInfo      = {};
    dependencyInfo.dependencyFlags         = {};
    dependencyInfo.imageMemoryBarrierCount = 1;
    dependencyInfo.pImageMemoryBarriers    = &barrier;

    d_commandBuffers[d_frameIndex].pipelineBarrier2(dependencyInfo);
}

void VikingRoom::transitionImageLayout(
    const vk::raii::CommandBuffer& commandBuffer,
    const vk::raii::Image&         image,
    const vk::ImageLayout          oldLayout,
    const vk::ImageLayout          newLayout)
{
    vk::ImageMemoryBarrier barrier{};
    barrier.oldLayout                       = oldLayout;
    barrier.newLayout                       = newLayout;
    barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.image                           = *image;
    barrier.subresourceRange.aspectMask     = vk::ImageAspectFlagBits::eColor;
    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = 1;

    vk::PipelineStageFlags sourceStage{};
    vk::PipelineStageFlags destinationStage{};

    if (oldLayout == vk::ImageLayout::eUndefined &&
        newLayout == vk::ImageLayout::eTransferDstOptimal) {
        barrier.srcAccessMask = {};
        barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
        sourceStage           = vk::PipelineStageFlagBits::eTopOfPipe;
        destinationStage      = vk::PipelineStageFlagBits::eTransfer;
    } else if (oldLayout == vk::ImageLayout::eTransferDstOptimal &&
               newLayout == vk::ImageLayout::eShaderReadOnlyOptimal) {
        barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
        barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
        sourceStage           = vk::PipelineStageFlagBits::eTransfer;
        destinationStage      = vk::PipelineStageFlagBits::eFragmentShader;
    } else {
        std::cout << "unsupported layout transition!\n";
        return;
    }

    commandBuffer.pipelineBarrier(
        sourceStage, destinationStage, {}, {}, {}, barrier);
}

void VikingRoom::copyBufferToImage(
    const vk::raii::CommandBuffer& commandBuffer,
    const vk::raii::Buffer&        buffer,
    const vk::raii::Image&         image,
    const std::uint32_t            width,
    const std::uint32_t            height)
{
    vk::BufferImageCopy region{};
    region.bufferOffset                    = 0;
    region.bufferRowLength                 = 0;
    region.bufferImageHeight               = 0;
    region.imageSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
    region.imageSubresource.mipLevel       = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount     = 1;
    region.imageOffset                     = vk::Offset3D{0, 0, 0};
    region.imageExtent                     = vk::Extent3D{width, height, 1};

    commandBuffer.copyBufferToImage(*buffer,
                                    *image,
                                    vk::ImageLayout::eTransferDstOptimal,
                                    region);
}

void VikingRoom::copyBuffer(const vk::raii::Buffer& srcBuffer,
                            const vk::raii::Buffer& dstBuffer,
                            const vk::DeviceSize    size) const
{
    const vk::raii::CommandBuffer commandBuffer = beginSingleTimeCommands();
    commandBuffer.copyBuffer(*srcBuffer,
                             *dstBuffer,
                             vk::BufferCopy(0, 0, size));
    endSingleTimeCommands(std::move(commandBuffer));
}

void VikingRoom::drawFrame()
{
    const auto fenceResult = d_device.waitForFences(
        *d_inFlightFences[d_frameIndex], vk::True, UINT64_MAX);
    if (fenceResult != vk::Result::eSuccess) {
        std::cout << "failed to wait for fence!\n";
    }

    auto [result, imageIndex] = d_swapChain.acquireNextImage(
        UINT64_MAX, *d_presentCompleteSemaphores[d_frameIndex], nullptr);
    if (result == vk::Result::eErrorOutOfDateKHR) {
        recreateSwapChain();
        return;
    }

    if (result != vk::Result::eSuccess &&
        result != vk::Result::eSuboptimalKHR) {
        assert(result == vk::Result::eTimeout ||
               result == vk::Result::eNotReady);
        std::cout << "failed to acquire swap chain image!\n";
        return;
    }

    d_device.resetFences(*d_inFlightFences[d_frameIndex]);

    recordCommandBuffer(imageIndex);
    updateUniformBuffer(imageIndex);

    constexpr vk::PipelineStageFlags waitDestinationStageMask(
        vk::PipelineStageFlagBits::eColorAttachmentOutput);

    vk::SubmitInfo submitInfo{};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores   = &*d_presentCompleteSemaphores[d_frameIndex];
    submitInfo.pWaitDstStageMask = &waitDestinationStageMask;
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = &*d_commandBuffers[d_frameIndex];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores    = &*d_renderFinishedSemaphores[imageIndex];

    d_graphicQueue.submit(submitInfo, *d_inFlightFences[d_frameIndex]);

    vk::PresentInfoKHR presentInfoKHR{};
    presentInfoKHR.waitSemaphoreCount = 1;
    presentInfoKHR.pWaitSemaphores = &*d_renderFinishedSemaphores[imageIndex];
    presentInfoKHR.swapchainCount  = 1;
    presentInfoKHR.pSwapchains     = &*d_swapChain;
    presentInfoKHR.pImageIndices   = &imageIndex;

    result = d_graphicQueue.presentKHR(presentInfoKHR);
    if (result == vk::Result::eSuboptimalKHR ||
        result == vk::Result::eErrorOutOfDateKHR || d_framebufferResized) {
        d_framebufferResized = false;
        recreateSwapChain();
    } else {
        assert(result == vk::Result::eSuccess);
    }

    d_frameIndex = (d_frameIndex + 1) % k_MAX_FRAMES_IN_FLIGHT;
}

void VikingRoom::mainLoop()
{
    while (glfwWindowShouldClose(d_window) == 0) {
        // Check for events like pressing the Z button
        glfwPollEvents();
        drawFrame();
    }

    d_device.waitIdle();
}

void VikingRoom::cleanUp()
{
    d_swapChainImageViews.clear();
    d_swapChain = nullptr;

    glfwDestroyWindow(d_window);
    glfwTerminate();
}

}  // close package namespace
