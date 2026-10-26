// viking_room.h -*-C++-*-
#ifndef INCLUDED_VKR_VIKING_ROOM_H
#define INCLUDED_VKR_VIKING_ROOM_H

// std
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// third party
#include <vulkan/vulkan_raii.hpp>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

namespace vkr {

// A single vertex as uploaded to the vertex buffer. 'pos' is 3D so the same
// layout can hold a loaded model; 'texCoord' selects a texel in the sampled
// texture.
struct Vertex {
    glm::vec3 pos;
    glm::vec3 color;
    glm::vec2 texCoord;

    // Tell Vulkan how to feed this data to the vertex shader.
    static vk::VertexInputBindingDescription getBindingDescription()
    {
        vk::VertexInputBindingDescription bindingDescription{};
        bindingDescription.binding   = 0;
        bindingDescription.stride    = sizeof(Vertex);
        bindingDescription.inputRate = vk::VertexInputRate::eVertex;

        return bindingDescription;
    }

    static std::array<vk::VertexInputAttributeDescription, 3>
    getAttributeDescriptions()
    {
        vk::VertexInputAttributeDescription posAttributeDescription{};
        posAttributeDescription.location = 0;
        posAttributeDescription.binding  = 0;
        posAttributeDescription.format   = vk::Format::eR32G32B32Sfloat;
        posAttributeDescription.offset   = offsetof(Vertex, pos);

        vk::VertexInputAttributeDescription colorAttributeDescription{};
        colorAttributeDescription.location = 1;
        colorAttributeDescription.binding  = 0;
        colorAttributeDescription.format   = vk::Format::eR32G32B32Sfloat;
        colorAttributeDescription.offset   = offsetof(Vertex, color);

        vk::VertexInputAttributeDescription texCoordAttributeDescription{};
        texCoordAttributeDescription.location = 2;
        texCoordAttributeDescription.binding  = 0;
        texCoordAttributeDescription.format   = vk::Format::eR32G32Sfloat;
        texCoordAttributeDescription.offset   = offsetof(Vertex, texCoord);

        return {
            posAttributeDescription,
            colorAttributeDescription,
            texCoordAttributeDescription,
        };
    }

    bool operator==(const Vertex& other) const
    {
        return pos == other.pos && color == other.color &&
               texCoord == other.texCoord;
    }
};

class VikingRoom {
  private:
    vk::raii::Context                    d_context;
    vk::raii::Instance                   d_instance;
    vk::raii::SurfaceKHR                 d_surface;
    vk::raii::PhysicalDevice             d_physicalDevice;
    std::uint32_t                        d_graphicQueueFamilyIndex;
    vk::raii::Device                     d_device;
    vk::raii::Queue                      d_graphicQueue;
    vk::raii::SwapchainKHR               d_swapChain;
    vk::Format                           d_format;
    vk::Extent2D                         d_extent;
    std::vector<vk::raii::ImageView>     d_swapChainImageViews;
    std::vector<vk::Image>               d_swapChainImages;
    vk::raii::Image                      d_depthImage;
    vk::raii::DeviceMemory               d_depthImageMemory;
    vk::raii::ImageView                  d_depthImageView;
    vk::raii::Image                      d_textureImage;
    vk::raii::DeviceMemory               d_textureImageMemory;
    vk::raii::ImageView                  d_textureImageView;
    vk::raii::Sampler                    d_textureSampler;
    vk::raii::DescriptorSetLayout        d_descriptorSetLayout;
    vk::raii::PipelineLayout             d_pipelineLayout;
    vk::raii::Pipeline                   d_graphicsPipeline;
    vk::raii::CommandPool                d_commandPool;
    std::vector<vk::raii::CommandBuffer> d_commandBuffers;
    std::vector<vk::raii::Semaphore>     d_presentCompleteSemaphores;
    std::vector<vk::raii::Semaphore>     d_renderFinishedSemaphores;
    std::vector<vk::raii::Fence>         d_inFlightFences;
    uint32_t                             d_frameIndex;
    std::vector<Vertex>                  d_vertices;
    std::vector<std::uint32_t>           d_indices;
    vk::raii::Buffer                     d_vertexBuffer;
    vk::raii::DeviceMemory               d_vertexBufferMemory;
    vk::raii::Buffer                     d_indexBuffer;
    vk::raii::DeviceMemory               d_indexBufferMemory;
    std::vector<vk::raii::Buffer>        d_uniformBuffers;
    std::vector<vk::raii::DeviceMemory>  d_uniformBuffersMemory;
    std::vector<void*>                   d_uniformBuffersMapped;
    vk::raii::DescriptorPool             d_descriptorPool;
    std::vector<vk::raii::DescriptorSet> d_descriptorSets;

    bool d_framebufferResized;


    GLFWwindow* d_window;

    void initWindow();
    void initVulkan();
    void createSwapChain();
    void recreateSwapChain();
    void createDescriptorSetLayout();
    void createGraphicsPipeline();
    void createDepthResources();
    [[nodiscard]] vk::Format
    findSupportedFormat(const std::vector<vk::Format>& candidates,
                        vk::ImageTiling                tiling,
                        vk::FormatFeatureFlags         features) const;
    [[nodiscard]] vk::Format findDepthFormat() const;
    void                     createTextureImage();
    void                     createTextureImageView();
    void                     createTextureSampler();
    void                     loadModel();
    void                     createVertexBuffer();
    void                     createIndexBuffer();
    void                     createUniformBuffers();
    void                     createDescriptorPool();
    void                     createDescriptorSets();
    [[nodiscard]] std::pair<vk::raii::Buffer, vk::raii::DeviceMemory>
    createBuffer(vk::DeviceSize          size,
                 vk::BufferUsageFlags    usage,
                 vk::MemoryPropertyFlags properties) const;
    [[nodiscard]] std::pair<vk::raii::Image, vk::raii::DeviceMemory>
    createImage(std::uint32_t           width,
                std::uint32_t           height,
                vk::Format              format,
                vk::ImageTiling         tiling,
                vk::ImageUsageFlags     usage,
                vk::MemoryPropertyFlags properties) const;
    [[nodiscard]] vk::raii::ImageView
                                          createImageView(vk::Image            image,
                                                          vk::Format           format,
                                                          vk::ImageAspectFlags aspectFlags) const;
    [[nodiscard]] vk::raii::CommandBuffer beginSingleTimeCommands() const;
    void
    endSingleTimeCommands(const vk::raii::CommandBuffer& commandBuffer) const;
    void createSyncObjects();
    void recordCommandBuffer(uint32_t imageIndex) const;
    void updateUniformBuffer(uint32_t imageIndex) const;
    void transitionImageLayout(vk::Image               image,
                               vk::ImageLayout         oldLayout,
                               vk::ImageLayout         newLayout,
                               vk::AccessFlags2        srcAccessMask,
                               vk::AccessFlags2        dstAccessMask,
                               vk::PipelineStageFlags2 srcStageMask,
                               vk::PipelineStageFlags2 dstStageMask,
                               vk::ImageAspectFlags    aspectMask) const;
    static void
    transitionImageLayout(const vk::raii::CommandBuffer& commandBuffer,
                          const vk::raii::Image&         image,
                          vk::ImageLayout                oldLayout,
                          vk::ImageLayout                newLayout);
    static void copyBufferToImage(const vk::raii::CommandBuffer& commandBuffer,
                                  const vk::raii::Buffer&        buffer,
                                  const vk::raii::Image&         image,
                                  std::uint32_t                  width,
                                  std::uint32_t                  height);
    void        copyBuffer(const vk::raii::Buffer& srcBuffer,
                           const vk::raii::Buffer& dstBuffer,
                           vk::DeviceSize          size) const;
    void        drawFrame();
    void        mainLoop();
    void        cleanUp();

  public:
    VikingRoom();

    ~VikingRoom() = default;

    void run();

    void setFrameBufferResied(bool frameBufferResized);
};

inline void VikingRoom::setFrameBufferResied(const bool frameBufferResized)
{
    d_framebufferResized = frameBufferResized;
}

}  // close package names

#endif  // INCLUDED_VKR_VIKING_ROOM_H
