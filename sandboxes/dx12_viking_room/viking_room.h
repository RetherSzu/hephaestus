// viking_room.h                                                      -*-C++-*-
#ifndef INCLUDED_VKR_VIKING_ROOM_H
#define INCLUDED_VKR_VIKING_ROOM_H

// std
#include <array>
#include <cstddef>
#include <vector>

// third party
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

constexpr int k_WINDOW_WIDTH         = 800;
constexpr int k_WINDOW_HEIGHT        = 600;
constexpr int k_MAX_FRAMES_IN_FLIGHT = 3;

namespace vkr {

template <class t_T> using ComPtr = Microsoft::WRL::ComPtr<t_T>;

// A single vertex as uploaded to the vertex buffer; same layout as the Vulkan
// sandbox so both load the model identically.
struct Vertex {
    glm::vec3 pos;
    glm::vec3 color;
    glm::vec2 texCoord;

    // Tell Direct3D how to feed this data to the vertex shader; the semantics
    // match 'VSInput' in 'shaders/shader.slang'.
    static std::array<D3D12_INPUT_ELEMENT_DESC, 3> getInputElementDescs()
    {
        return {
            {
                {
                    .SemanticName      = "POSITION",
                    .SemanticIndex     = 0,
                    .Format            = DXGI_FORMAT_R32G32B32_FLOAT,
                    .InputSlot         = 0,
                    .AlignedByteOffset = offsetof(Vertex, pos),
                    .InputSlotClass =
                        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                    .InstanceDataStepRate = 0,
                },
                {
                    .SemanticName      = "COLOR",
                    .SemanticIndex     = 0,
                    .Format            = DXGI_FORMAT_R32G32B32_FLOAT,
                    .InputSlot         = 0,
                    .AlignedByteOffset = offsetof(Vertex, color),
                    .InputSlotClass =
                        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                    .InstanceDataStepRate = 0,
                },
                {
                    .SemanticName      = "TEXCOORD",
                    .SemanticIndex     = 0,
                    .Format            = DXGI_FORMAT_R32G32_FLOAT,
                    .InputSlot         = 0,
                    .AlignedByteOffset = offsetof(Vertex, texCoord),
                    .InputSlotClass =
                        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                    .InstanceDataStepRate = 0,
                },
            },
        };
    }

    friend bool operator==(const Vertex&, const Vertex&) = default;
};

class VikingRoom {
  private:
    ComPtr<IDXGIFactory4>          d_factory;
    ComPtr<ID3D12Device2>          d_device;
    ComPtr<ID3D12CommandQueue>     d_commandQueue;
    ComPtr<IDXGISwapChain4>        d_swapChain;
    std::uint32_t                  d_width;
    std::uint32_t                  d_height;
    ComPtr<ID3D12Resource>         d_backBuffers[k_MAX_FRAMES_IN_FLIGHT];
    ComPtr<ID3D12DescriptorHeap>   d_rtvHeap;
    UINT                           d_rtvDescriptorSize;
    ComPtr<ID3D12Resource>         d_depthBuffer;
    ComPtr<ID3D12DescriptorHeap>   d_dsvHeap;
    ComPtr<ID3D12Resource>         d_texture;
    ComPtr<ID3D12DescriptorHeap>   d_srvHeap;
    ComPtr<ID3D12RootSignature>    d_rootSignature;
    ComPtr<ID3D12PipelineState>    d_pipelineState;
    ComPtr<ID3D12CommandAllocator> d_commandAllocators[k_MAX_FRAMES_IN_FLIGHT];
    ComPtr<ID3D12GraphicsCommandList> d_commandList;
    UINT                              d_frameIndex;
    std::vector<Vertex>               d_vertices;
    std::vector<std::uint32_t>        d_indices;
    ComPtr<ID3D12Resource>            d_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW          d_vertexBufferView;
    ComPtr<ID3D12Resource>            d_indexBuffer;
    D3D12_INDEX_BUFFER_VIEW           d_indexBufferView;
    ComPtr<ID3D12Resource>            d_uniformBuffers[k_MAX_FRAMES_IN_FLIGHT];
    void* d_uniformBuffersMapped[k_MAX_FRAMES_IN_FLIGHT];

    // Sync: the queue signals 'd_fence' with 'd_fenceValue'; each frame
    // remembers the value that marks its work as done.
    ComPtr<ID3D12Fence> d_fence;
    std::uint64_t       d_fenceValue;
    std::uint64_t       d_frameFenceValues[k_MAX_FRAMES_IN_FLIGHT];
    HANDLE              d_fenceEvent;

    bool d_framebufferResized;

    GLFWwindow* d_window;

    void initWindow();
    void initDx12();
    void createSwapChain();
    void createRenderTargetViews();
    void recreateSwapChain();
    void createRootSignature();
    void createGraphicsPipeline();
    void createDepthResources();
    void createTextureImage();
    void loadModel();
    void createVertexBuffer();
    void createIndexBuffer();
    void createUniformBuffers();
    [[nodiscard]] ComPtr<ID3D12Resource>
    createBuffer(std::uint64_t size, D3D12_HEAP_TYPE heapType) const;
    [[nodiscard]] ComPtr<ID3D12Resource>
         createDefaultBuffer(const void* data, std::uint64_t size);
    void beginSingleTimeCommands() const;
    void endSingleTimeCommands();
    void recordCommandBuffer() const;
    void updateUniformBuffer() const;
    void waitForFenceValue(std::uint64_t value) const;
    void flush();
    void drawFrame();
    void mainLoop();
    void cleanUp();

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

}  // close package namespace

#endif  // INCLUDED_VKR_VIKING_ROOM_H
