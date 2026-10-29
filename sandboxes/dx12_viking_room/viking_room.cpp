// viking_room.cpp                                                    -*-C++-*-
#include <viking_room.h>

// std
#include <chrono>
#include <fstream>
#include <iostream>
#include <unordered_map>

// third party
#include <d3d12sdklayers.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
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

constexpr auto k_TEXTURE_PATH = "textures/viking_room.png";
constexpr auto k_MODEL_PATH   = "models/viking_room.obj";

// Flip-model swap chains reject sRGB buffer formats, so the buffers are UNORM
// and the render target views write sRGB, like Vulkan's 'eB8G8R8A8Srgb'.
constexpr DXGI_FORMAT k_SWAP_CHAIN_FORMAT    = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr DXGI_FORMAT k_RENDER_TARGET_FORMAT = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
constexpr DXGI_FORMAT k_DEPTH_FORMAT         = DXGI_FORMAT_D32_FLOAT;
constexpr DXGI_FORMAT k_TEXTURE_FORMAT       = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

namespace vkr {
namespace {

// Errors are values (no exceptions), but a failed D3D12 call leaves nothing
// to continue with, so report it and stop.
void checkResult(const HRESULT hr, const char* what)
{
    if (FAILED(hr)) {
        std::cerr << "[Error] " << what << " failed (HRESULT 0x" << std::hex
                  << static_cast<unsigned long>(hr) << ")\n";
        std::abort();
    }
}

// Print the debug layer's messages to the console, as the Vulkan validation
// layer does; otherwise they only reach an attached debugger.
void CALLBACK debugMessageCallback(D3D12_MESSAGE_CATEGORY /*category*/,
                                   const D3D12_MESSAGE_SEVERITY severity,
                                   D3D12_MESSAGE_ID /*id*/,
                                   const LPCSTR description,
                                   void* /*context*/)
{
    if (severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
        std::cerr << "[D3D12] " << description << '\n';
    }
}

std::vector<char> readFile(const std::string& filename)
{
    std::ifstream file(filename, std::ios::binary);

    if (!file.is_open()) {
        std::cout << "failed to open file!\n";
        return {};
    }

    file.seekg(0, std::ios::end);
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

constexpr D3D12_HEAP_PROPERTIES makeHeapProperties(const D3D12_HEAP_TYPE type)
{
    return {
        .Type                 = type,
        .CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
        .CreationNodeMask     = 0,
        .VisibleNodeMask      = 0,
    };
}

D3D12_RESOURCE_DESC texture2DDesc(const std::uint32_t        width,
                                  const std::uint32_t        height,
                                  const DXGI_FORMAT          format,
                                  const D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = width;
    desc.Height           = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = format;
    desc.SampleDesc.Count = 1;
    desc.Flags            = flags;
    return desc;
}

void transitionImageLayout(ID3D12GraphicsCommandList*  commandList,
                           ID3D12Resource*             resource,
                           const D3D12_RESOURCE_STATES before,
                           const D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter  = after;
    commandList->ResourceBarrier(1, &barrier);
}

struct UniformBufferObject {
    alignas(16) glm::mat4 model;
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
};

}  // close unnamed namespace

VikingRoom::VikingRoom()
: d_width(0)
, d_height(0)
, d_rtvDescriptorSize(0)
, d_frameIndex(0)
, d_vertexBufferView{}
, d_indexBufferView{}
, d_uniformBuffersMapped{}
, d_fenceValue(0)
, d_frameFenceValues{}
, d_fenceEvent(nullptr)
, d_framebufferResized(false)
, d_window(nullptr)
{
}

void VikingRoom::run()
{
    initWindow();
    initDx12();
    createSwapChain();
    createRootSignature();
    createGraphicsPipeline();
    createDepthResources();
    createTextureImage();
    loadModel();
    createVertexBuffer();
    createIndexBuffer();
    createUniformBuffers();
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

    d_window = glfwCreateWindow(k_WINDOW_WIDTH,
                                k_WINDOW_HEIGHT,
                                "Viking Room - DirectX 12",
                                nullptr,
                                nullptr);
    glfwSetWindowUserPointer(d_window, this);
    glfwSetFramebufferSizeCallback(d_window, framebufferResizeCallback);
}

void VikingRoom::initDx12()
{
    ComPtr<ID3D12Debug> debugInterface;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface)))) {
        debugInterface->EnableDebugLayer();
    } else {
        std::cerr << "[Error] Missing D3D12 debug layer (Graphics Tools)\n";
    }

    checkResult(CreateDXGIFactory2(debugInterface != nullptr
                                       ? DXGI_CREATE_FACTORY_DEBUG
                                       : 0u,
                                   IID_PPV_ARGS(&d_factory)),
                "CreateDXGIFactory2");

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIAdapter1> bestAdapter;
    SIZE_T                maxDedicatedVideoMemory = 0;
    for (UINT i = 0;
         d_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc;
        (void)adapter->GetDesc1(&desc);

        const bool isSoftware =
            (desc.Flags & static_cast<UINT>(DXGI_ADAPTER_FLAG_SOFTWARE)) != 0;
        if (isSoftware) {
            continue;
        }

        if (desc.DedicatedVideoMemory <= maxDedicatedVideoMemory) {
            continue;
        }

        const bool supportsD3D12 = SUCCEEDED(
            D3D12CreateDevice(adapter.Get(),
                              D3D_FEATURE_LEVEL_11_0,
                              __uuidof(ID3D12Device),
                              nullptr));
        if (!supportsD3D12) {
            continue;
        }

        maxDedicatedVideoMemory = desc.DedicatedVideoMemory;
        bestAdapter             = adapter;
    }

    checkResult(D3D12CreateDevice(bestAdapter.Get(),
                                  D3D_FEATURE_LEVEL_11_0,
                                  IID_PPV_ARGS(&d_device)),
                "D3D12CreateDevice");

    ComPtr<ID3D12InfoQueue1> infoQueue;
    if (SUCCEEDED(d_device.As(&infoQueue))) {
        DWORD callbackCookie = 0;
        (void)infoQueue->RegisterMessageCallback(
            &debugMessageCallback,
            D3D12_MESSAGE_CALLBACK_FLAG_NONE,
            nullptr,
            &callbackCookie);
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    checkResult(d_device->CreateCommandQueue(&queueDesc,
                                             IID_PPV_ARGS(&d_commandQueue)),
                "CreateCommandQueue");

    for (auto& allocator : d_commandAllocators) {
        checkResult(
            d_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&allocator)),
            "CreateCommandAllocator");
    }

    checkResult(d_device->CreateCommandList(0,
                                            D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            d_commandAllocators[0].Get(),
                                            nullptr,
                                            IID_PPV_ARGS(&d_commandList)),
                "CreateCommandList");
    checkResult(d_commandList->Close(), "Close");

    checkResult(d_device->CreateFence(0,
                                      D3D12_FENCE_FLAG_NONE,
                                      IID_PPV_ARGS(&d_fence)),
                "CreateFence");
    d_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = k_MAX_FRAMES_IN_FLIGHT;
    checkResult(d_device->CreateDescriptorHeap(&rtvHeapDesc,
                                               IID_PPV_ARGS(&d_rtvHeap)),
                "CreateDescriptorHeap (RTV)");
    d_rtvDescriptorSize = d_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.NumDescriptors = 1;
    checkResult(d_device->CreateDescriptorHeap(&dsvHeapDesc,
                                               IID_PPV_ARGS(&d_dsvHeap)),
                "CreateDescriptorHeap (DSV)");

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    checkResult(d_device->CreateDescriptorHeap(&srvHeapDesc,
                                               IID_PPV_ARGS(&d_srvHeap)),
                "CreateDescriptorHeap (SRV)");
}

void VikingRoom::createSwapChain()
{
    int width  = 0;
    int height = 0;
    glfwGetFramebufferSize(d_window, &width, &height);
    d_width  = static_cast<std::uint32_t>(width);
    d_height = static_cast<std::uint32_t>(height);

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.Width            = d_width;
    swapChainDesc.Height           = d_height;
    swapChainDesc.Format           = k_SWAP_CHAIN_FORMAT;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount      = k_MAX_FRAMES_IN_FLIGHT;
    swapChainDesc.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    HWND hwnd = glfwGetWin32Window(d_window);

    ComPtr<IDXGISwapChain1> swapChain1;
    checkResult(d_factory->CreateSwapChainForHwnd(d_commandQueue.Get(),
                                                  hwnd,
                                                  &swapChainDesc,
                                                  nullptr,
                                                  nullptr,
                                                  &swapChain1),
                "CreateSwapChainForHwnd");
    checkResult(d_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER),
                "MakeWindowAssociation");
    checkResult(swapChain1.As(&d_swapChain), "IDXGISwapChain4");

    d_frameIndex = d_swapChain->GetCurrentBackBufferIndex();

    createRenderTargetViews();
}

void VikingRoom::createRenderTargetViews()
{
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format        = k_RENDER_TARGET_FORMAT;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        d_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < std::size(d_backBuffers); ++i) {
        checkResult(d_swapChain->GetBuffer(i, IID_PPV_ARGS(&d_backBuffers[i])),
                    "GetBuffer");
        d_device->CreateRenderTargetView(d_backBuffers[i].Get(),
                                         &rtvDesc,
                                         handle);
        handle.ptr += d_rtvDescriptorSize;
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

    flush();

    for (auto& backBuffer : d_backBuffers) {
        backBuffer.Reset();
    }

    d_width  = static_cast<std::uint32_t>(width);
    d_height = static_cast<std::uint32_t>(height);

    checkResult(d_swapChain->ResizeBuffers(
                    0, d_width, d_height, DXGI_FORMAT_UNKNOWN, 0),
                "ResizeBuffers");
    d_frameIndex = d_swapChain->GetCurrentBackBufferIndex();

    createRenderTargetViews();
    createDepthResources();
}

void VikingRoom::createRootSignature()
{
    D3D12_ROOT_PARAMETER rootParameters[2]{};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[0].Descriptor.ShaderRegister = 0;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors     = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParameters[1].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[1].DescriptorTable.pDescriptorRanges   = &srvRange;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    constexpr D3D12_STATIC_SAMPLER_DESC sampler{
        .Filter           = D3D12_FILTER_ANISOTROPIC,
        .AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        .AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        .AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        .MipLODBias       = 0.0f,
        .MaxAnisotropy    = D3D12_REQ_MAXANISOTROPY,
        .ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS,
        .BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK,
        .MinLOD           = 0.0f,
        .MaxLOD           = D3D12_FLOAT32_MAX,
        .ShaderRegister   = 0,
        .RegisterSpace    = 0,
        .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
    };

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
    rootSignatureDesc.NumParameters = static_cast<UINT>(
        std::size(rootParameters));
    rootSignatureDesc.pParameters       = rootParameters;
    rootSignatureDesc.NumStaticSamplers = 1;
    rootSignatureDesc.pStaticSamplers   = &sampler;
    rootSignatureDesc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    const HRESULT    hr = D3D12SerializeRootSignature(
        &rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr) && error != nullptr) {
        std::cerr << static_cast<const char*>(error->GetBufferPointer())
                  << '\n';
    }
    checkResult(hr, "D3D12SerializeRootSignature");

    checkResult(d_device->CreateRootSignature(0,
                                              signature->GetBufferPointer(),
                                              signature->GetBufferSize(),
                                              IID_PPV_ARGS(&d_rootSignature)),
                "CreateRootSignature");
}

void VikingRoom::createGraphicsPipeline()
{
    const auto vertShaderCode = readFile("shaders/shader.vs.dxil");
    const auto fragShaderCode = readFile("shaders/shader.ps.dxil");

    const auto inputElementDescs = Vertex::getInputElementDescs();

    constexpr D3D12_RENDER_TARGET_BLEND_DESC opaqueBlend{
        .BlendEnable           = FALSE,
        .LogicOpEnable         = FALSE,
        .SrcBlend              = D3D12_BLEND_ONE,
        .DestBlend             = D3D12_BLEND_ZERO,
        .BlendOp               = D3D12_BLEND_OP_ADD,
        .SrcBlendAlpha         = D3D12_BLEND_ONE,
        .DestBlendAlpha        = D3D12_BLEND_ZERO,
        .BlendOpAlpha          = D3D12_BLEND_OP_ADD,
        .LogicOp               = D3D12_LOGIC_OP_NOOP,
        .RenderTargetWriteMask = static_cast<UINT8>(
            D3D12_COLOR_WRITE_ENABLE_ALL),
    };

    constexpr D3D12_DEPTH_STENCILOP_DESC keepStencil{
        .StencilFailOp      = D3D12_STENCIL_OP_KEEP,
        .StencilDepthFailOp = D3D12_STENCIL_OP_KEEP,
        .StencilPassOp      = D3D12_STENCIL_OP_KEEP,
        .StencilFunc        = D3D12_COMPARISON_FUNC_ALWAYS,
    };

    constexpr D3D12_SHADER_BYTECODE noShader{
        .pShaderBytecode = nullptr,
        .BytecodeLength  = 0,
    };

    const D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{
        .pRootSignature = d_rootSignature.Get(),
        .VS =
            {
                .pShaderBytecode = vertShaderCode.data(),
                .BytecodeLength  = vertShaderCode.size(),
            },
        .PS =
            {
                .pShaderBytecode = fragShaderCode.data(),
                .BytecodeLength  = fragShaderCode.size(),
            },
        .DS = noShader,
        .HS = noShader,
        .GS = noShader,
        .StreamOutput =
            {
                .pSODeclaration   = nullptr,
                .NumEntries       = 0,
                .pBufferStrides   = nullptr,
                .NumStrides       = 0,
                .RasterizedStream = 0,
            },
        .BlendState =
            {
                .AlphaToCoverageEnable  = FALSE,
                .IndependentBlendEnable = FALSE,
                .RenderTarget =
                    {
                        opaqueBlend,
                        opaqueBlend,
                        opaqueBlend,
                        opaqueBlend,
                        opaqueBlend,
                        opaqueBlend,
                        opaqueBlend,
                        opaqueBlend,
                    },
            },
        .SampleMask = UINT_MAX,
        .RasterizerState =
            {
                .FillMode              = D3D12_FILL_MODE_SOLID,
                .CullMode              = D3D12_CULL_MODE_BACK,
                .FrontCounterClockwise = TRUE,
                .DepthBias             = D3D12_DEFAULT_DEPTH_BIAS,
                .DepthBiasClamp        = D3D12_DEFAULT_DEPTH_BIAS_CLAMP,
                .SlopeScaledDepthBias  = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS,
                .DepthClipEnable       = TRUE,
                .MultisampleEnable     = FALSE,
                .AntialiasedLineEnable = FALSE,
                .ForcedSampleCount     = 0,
                .ConservativeRaster =
                    D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF,
            },
        .DepthStencilState =
            {
                .DepthEnable      = TRUE,
                .DepthWriteMask   = D3D12_DEPTH_WRITE_MASK_ALL,
                .DepthFunc        = D3D12_COMPARISON_FUNC_LESS,
                .StencilEnable    = FALSE,
                .StencilReadMask  = D3D12_DEFAULT_STENCIL_READ_MASK,
                .StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK,
                .FrontFace        = keepStencil,
                .BackFace         = keepStencil,
            },
        .InputLayout =
            {
                .pInputElementDescs = inputElementDescs.data(),
                .NumElements = static_cast<UINT>(inputElementDescs.size()),
            },
        .IBStripCutValue       = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED,
        .PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
        .NumRenderTargets      = 1,
        .RTVFormats            = {k_RENDER_TARGET_FORMAT},
        .DSVFormat             = k_DEPTH_FORMAT,
        .SampleDesc            = {.Count = 1, .Quality = 0},
        .NodeMask              = 0,
        .CachedPSO = {.pCachedBlob = nullptr, .CachedBlobSizeInBytes = 0},
        .Flags     = D3D12_PIPELINE_STATE_FLAG_NONE,
    };

    checkResult(d_device->CreateGraphicsPipelineState(
                    &pipelineDesc, IID_PPV_ARGS(&d_pipelineState)),
                "CreateGraphicsPipelineState");
}

void VikingRoom::createDepthResources()
{
    const D3D12_RESOURCE_DESC depthDesc = texture2DDesc(
        d_width,
        d_height,
        k_DEPTH_FORMAT,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

    D3D12_CLEAR_VALUE clearDepth{};
    clearDepth.Format             = k_DEPTH_FORMAT;
    clearDepth.DepthStencil.Depth = 1.0f;

    constexpr D3D12_HEAP_PROPERTIES heapProperties = makeHeapProperties(
        D3D12_HEAP_TYPE_DEFAULT);

    checkResult(
        d_device->CreateCommittedResource(&heapProperties,
                                          D3D12_HEAP_FLAG_NONE,
                                          &depthDesc,
                                          D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                          &clearDepth,
                                          IID_PPV_ARGS(&d_depthBuffer)),
        "CreateCommittedResource (depth)");

    d_device->CreateDepthStencilView(
        d_depthBuffer.Get(),
        nullptr,
        d_dsvHeap->GetCPUDescriptorHandleForHeapStart());
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

    const D3D12_RESOURCE_DESC textureDesc = texture2DDesc(
        static_cast<std::uint32_t>(texWidth),
        static_cast<std::uint32_t>(texHeight),
        k_TEXTURE_FORMAT,
        D3D12_RESOURCE_FLAG_NONE);

    constexpr D3D12_HEAP_PROPERTIES heapProperties = makeHeapProperties(
        D3D12_HEAP_TYPE_DEFAULT);

    checkResult(
        d_device->CreateCommittedResource(&heapProperties,
                                          D3D12_HEAP_FLAG_NONE,
                                          &textureDesc,
                                          D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr,
                                          IID_PPV_ARGS(&d_texture)),
        "CreateCommittedResource (texture)");

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64                             stagingSize = 0;
    d_device->GetCopyableFootprints(
        &textureDesc, 0, 1, 0, &footprint, nullptr, nullptr, &stagingSize);

    const ComPtr<ID3D12Resource> stagingBuffer = createBuffer(
        stagingSize, D3D12_HEAP_TYPE_UPLOAD);

    std::uint8_t* data = nullptr;
    checkResult(stagingBuffer->Map(0,
                                   nullptr,
                                   reinterpret_cast<void**>(&data)),
                "Map");
    const auto rowSize = static_cast<std::size_t>(texWidth) * 4u;
    for (std::size_t y = 0; y < static_cast<std::size_t>(texHeight); ++y) {
        memcpy(data + footprint.Offset + (y * footprint.Footprint.RowPitch),
               pixels + (y * rowSize),
               rowSize);
    }
    stagingBuffer->Unmap(0, nullptr);

    stbi_image_free(pixels);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource        = d_texture.Get();
    dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource       = stagingBuffer.Get();
    src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;

    beginSingleTimeCommands();
    d_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transitionImageLayout(d_commandList.Get(),
                          d_texture.Get(),
                          D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    endSingleTimeCommands();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                  = k_TEXTURE_FORMAT;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels     = 1;

    d_device->CreateShaderResourceView(
        d_texture.Get(),
        &srvDesc,
        d_srvHeap->GetCPUDescriptorHandleForHeapStart());
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
    const std::uint64_t bufferSize = sizeof(Vertex) * d_vertices.size();

    d_vertexBuffer = createDefaultBuffer(d_vertices.data(), bufferSize);

    d_vertexBufferView.BufferLocation = d_vertexBuffer->GetGPUVirtualAddress();
    d_vertexBufferView.SizeInBytes    = static_cast<UINT>(bufferSize);
    d_vertexBufferView.StrideInBytes  = sizeof(Vertex);
}

void VikingRoom::createIndexBuffer()
{
    const std::uint64_t bufferSize = sizeof(d_indices[0]) * d_indices.size();

    d_indexBuffer = createDefaultBuffer(d_indices.data(), bufferSize);

    d_indexBufferView.BufferLocation = d_indexBuffer->GetGPUVirtualAddress();
    d_indexBufferView.SizeInBytes    = static_cast<UINT>(bufferSize);
    d_indexBufferView.Format         = DXGI_FORMAT_R32_UINT;
}

void VikingRoom::createUniformBuffers()
{
    for (size_t i = 0; i < k_MAX_FRAMES_IN_FLIGHT; i++) {
        d_uniformBuffers[i] = createBuffer(sizeof(UniformBufferObject),
                                           D3D12_HEAP_TYPE_UPLOAD);
        checkResult(d_uniformBuffers[i]->Map(0,
                                             nullptr,
                                             &d_uniformBuffersMapped[i]),
                    "Map");
    }
}

ComPtr<ID3D12Resource>
VikingRoom::createBuffer(const std::uint64_t   size,
                         const D3D12_HEAP_TYPE heapType) const
{
    const D3D12_HEAP_PROPERTIES heapProperties = makeHeapProperties(heapType);

    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width            = size;
    bufferDesc.Height           = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels        = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    const D3D12_RESOURCE_STATES initialState =
        heapType == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                           : D3D12_RESOURCE_STATE_COMMON;

    ComPtr<ID3D12Resource> buffer;
    checkResult(d_device->CreateCommittedResource(&heapProperties,
                                                  D3D12_HEAP_FLAG_NONE,
                                                  &bufferDesc,
                                                  initialState,
                                                  nullptr,
                                                  IID_PPV_ARGS(&buffer)),
                "CreateCommittedResource (buffer)");
    return buffer;
}

ComPtr<ID3D12Resource>
VikingRoom::createDefaultBuffer(const void* data, const std::uint64_t size)
{
    const ComPtr<ID3D12Resource> stagingBuffer = createBuffer(
        size, D3D12_HEAP_TYPE_UPLOAD);

    void* mapped = nullptr;
    checkResult(stagingBuffer->Map(0, nullptr, &mapped), "Map");
    memcpy(mapped, data, size);
    stagingBuffer->Unmap(0, nullptr);

    ComPtr<ID3D12Resource> buffer = createBuffer(size,
                                                 D3D12_HEAP_TYPE_DEFAULT);

    beginSingleTimeCommands();
    d_commandList->CopyBufferRegion(
        buffer.Get(), 0, stagingBuffer.Get(), 0, size);
    endSingleTimeCommands();

    return buffer;
}

void VikingRoom::beginSingleTimeCommands() const
{
    ID3D12CommandAllocator* allocator = d_commandAllocators[d_frameIndex]
                                            .Get();
    checkResult(allocator->Reset(), "Reset (allocator)");
    checkResult(d_commandList->Reset(allocator, nullptr), "Reset (list)");
}

void VikingRoom::endSingleTimeCommands()
{
    checkResult(d_commandList->Close(), "Close");

    ID3D12CommandList* const commandLists[] = {d_commandList.Get()};
    d_commandQueue->ExecuteCommandLists(1, commandLists);
    flush();
}

void VikingRoom::recordCommandBuffer() const
{
    ID3D12CommandAllocator* allocator = d_commandAllocators[d_frameIndex]
                                            .Get();
    checkResult(allocator->Reset(), "Reset (allocator)");
    checkResult(d_commandList->Reset(allocator, d_pipelineState.Get()),
                "Reset (list)");

    ID3D12Resource* backBuffer = d_backBuffers[d_frameIndex].Get();

    transitionImageLayout(d_commandList.Get(),
                          backBuffer,
                          D3D12_RESOURCE_STATE_PRESENT,
                          D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        d_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(d_frameIndex) * d_rtvDescriptorSize;
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv =
        d_dsvHeap->GetCPUDescriptorHandleForHeapStart();

    constexpr float clearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};
    d_commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    d_commandList->ClearDepthStencilView(
        dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    d_commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    d_commandList->SetGraphicsRootSignature(d_rootSignature.Get());
    ID3D12DescriptorHeap* const heaps[] = {d_srvHeap.Get()};
    d_commandList->SetDescriptorHeaps(1, heaps);
    d_commandList->SetGraphicsRootConstantBufferView(
        0, d_uniformBuffers[d_frameIndex]->GetGPUVirtualAddress());
    d_commandList->SetGraphicsRootDescriptorTable(
        1, d_srvHeap->GetGPUDescriptorHandleForHeapStart());

    d_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d_commandList->IASetVertexBuffers(0, 1, &d_vertexBufferView);
    d_commandList->IASetIndexBuffer(&d_indexBufferView);

    const D3D12_VIEWPORT viewport{
        .TopLeftX = 0.0f,
        .TopLeftY = 0.0f,
        .Width    = static_cast<float>(d_width),
        .Height   = static_cast<float>(d_height),
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f,
    };
    const D3D12_RECT scissor{
        .left   = 0,
        .top    = 0,
        .right  = static_cast<LONG>(d_width),
        .bottom = static_cast<LONG>(d_height),
    };
    d_commandList->RSSetViewports(1, &viewport);
    d_commandList->RSSetScissorRects(1, &scissor);

    d_commandList->DrawIndexedInstanced(
        static_cast<UINT>(d_indices.size()), 1, 0, 0, 0);

    transitionImageLayout(d_commandList.Get(),
                          backBuffer,
                          D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_PRESENT);

    checkResult(d_commandList->Close(), "Close");
}

void VikingRoom::updateUniformBuffer() const
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
                                static_cast<float>(d_width) /
                                    static_cast<float>(d_height),
                                0.1f,
                                10.0f);

    memcpy(d_uniformBuffersMapped[d_frameIndex], &ubo, sizeof(ubo));
}

void VikingRoom::waitForFenceValue(const std::uint64_t value) const
{
    if (d_fence->GetCompletedValue() < value) {
        checkResult(d_fence->SetEventOnCompletion(value, d_fenceEvent),
                    "SetEventOnCompletion");
        WaitForSingleObject(d_fenceEvent, INFINITE);
    }
}

void VikingRoom::flush()
{
    ++d_fenceValue;
    checkResult(d_commandQueue->Signal(d_fence.Get(), d_fenceValue), "Signal");
    waitForFenceValue(d_fenceValue);
}

void VikingRoom::drawFrame()
{
    waitForFenceValue(d_frameFenceValues[d_frameIndex]);

    recordCommandBuffer();
    updateUniformBuffer();

    ID3D12CommandList* const commandLists[] = {d_commandList.Get()};
    d_commandQueue->ExecuteCommandLists(1, commandLists);

    const HRESULT presentResult = d_swapChain->Present(1, 0);

    ++d_fenceValue;
    checkResult(d_commandQueue->Signal(d_fence.Get(), d_fenceValue), "Signal");
    d_frameFenceValues[d_frameIndex] = d_fenceValue;

    if (d_framebufferResized) {
        d_framebufferResized = false;
        recreateSwapChain();
    } else {
        checkResult(presentResult, "Present");
    }

    d_frameIndex = d_swapChain->GetCurrentBackBufferIndex();
}

void VikingRoom::mainLoop()
{
    while (glfwWindowShouldClose(d_window) == 0) {
        glfwPollEvents();
        drawFrame();
    }

    flush();
}

void VikingRoom::cleanUp()
{
    d_swapChain.Reset();
    CloseHandle(d_fenceEvent);

    glfwDestroyWindow(d_window);
    glfwTerminate();
}

}  // close package namespace
