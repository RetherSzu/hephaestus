// viking_room.h                                                      -*-C++-*-
#ifndef INCLUDED_VKR_VIKING_ROOM_H
#define INCLUDED_VKR_VIKING_ROOM_H

// std
#include <vector>

// third party
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

namespace vkr {

struct Vertex {
    glm::vec3 pos;
    glm::vec3 color;
    glm::vec2 texCoord;

    bool operator==(const Vertex& other) const
    {
        return pos == other.pos && color == other.color &&
               texCoord == other.texCoord;
    }
};

class VikingRoom {
  private:
    NS::SharedPtr<MTL::Device>              d_device;
    CA::MetalLayer*                         d_layer;
    NS::SharedPtr<MTL::CommandQueue>        d_commandQueue;
    NS::SharedPtr<MTL::Library>             d_library;
    NS::SharedPtr<MTL::RenderPipelineState> d_pipelineState;
    NS::SharedPtr<MTL::DepthStencilState>   d_depthStencilState;
    NS::SharedPtr<MTL::Texture>             d_depthTexture;
    NS::SharedPtr<MTL::Texture>             d_textureImage;
    NS::SharedPtr<MTL::SamplerState>        d_sampler;
    NS::SharedPtr<MTL::Buffer>              d_vertexBuffer;
    NS::SharedPtr<MTL::Buffer>              d_indexBuffer;
    std::vector<NS::SharedPtr<MTL::Buffer>> d_uniformBuffers;

    MTL::PixelFormat d_colorFormat;
    MTL::PixelFormat d_depthFormat;

    std::vector<Vertex>        d_vertices;
    std::vector<std::uint32_t> d_indices;

    dispatch_semaphore_t d_frameSemaphore;
    std::uint32_t        d_frameIndex;

    bool d_framebufferResized;

    GLFWwindow* d_window;

    void initWindow();
    void initMetal();
    void createGraphicsPipeline();
    void createDepthResources();
    void createTextureImage();
    void createTextureSampler();
    void loadModel();
    void createVertexBuffer();
    void createIndexBuffer();
    void createUniformBuffers();
    void recreateDrawable();
    [[nodiscard]] NS::SharedPtr<MTL::Buffer>
    createBuffer(const void* data, std::size_t size, const char* label) const;
    void updateUniformBuffer(std::uint32_t frameIndex) const;
    void recordCommands(MTL::RenderCommandEncoder* encoder,
                        const CA::MetalDrawable*   drawable) const;
    void drawFrame();
    void mainLoop();
    void cleanUp();

  public:
    VikingRoom();

    ~VikingRoom();

    VikingRoom(const VikingRoom&)            = delete;
    VikingRoom& operator=(const VikingRoom&) = delete;
    VikingRoom(VikingRoom&&)                 = delete;
    VikingRoom& operator=(VikingRoom&&)      = delete;

    void run();

    void setFrameBufferResied(bool frameBufferResized);
};

inline void VikingRoom::setFrameBufferResied(const bool frameBufferResized)
{
    d_framebufferResized = frameBufferResized;
}

}  // close package namespace

#endif  // INCLUDED_VKR_VIKING_ROOM_H
