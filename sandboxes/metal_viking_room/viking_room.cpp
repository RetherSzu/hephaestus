// viking_room.cpp                                                    -*-C++-*-
#include <viking_room.h>

// std
#include <chrono>
#include <iostream>
#include <unordered_map>

// third party
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <GLFW/glfw3.h>
#define GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
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

// Attach a 'CAMetalLayer' to the GLFW window's content view and return it. The
// definition lives in 'metal_layer.mm'; see that file for why it must.
extern "C" void* triAttachMetalLayer(GLFWwindow* window);

constexpr int k_WINDOW_WIDTH         = 800;
constexpr int k_WINDOW_HEIGHT        = 600;
constexpr int k_MAX_FRAMES_IN_FLIGHT = 3;

// Buffer argument slots the Slang shader exposes on the vertex stage. Slang
// assigns the uniform block '[[buffer(0)]]', so the per-vertex data must bind
// to a different slot; '[[stage_in]]' pulls it through the vertex descriptor.
constexpr std::uint32_t k_UNIFORM_BUFFER_INDEX = 0;
constexpr std::uint32_t k_VERTEX_BUFFER_INDEX  = 1;

constexpr auto k_SHADER_PATH  = "shaders/shader.metallib";
constexpr auto k_TEXTURE_PATH = "textures/viking_room.png";
constexpr auto k_MODEL_PATH   = "models/viking_room.obj";

namespace vkr {
namespace {

// Matches the 'UniformBuffer' block in the Slang shader. The 'alignas(16)'
// gives each matrix the 16-byte alignment the shader's layout expects.
struct UniformBufferObject {
    alignas(16) glm::mat4 model;
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
};

// Wrap a C string in an autoreleased 'NS::String' for the metal-cpp calls that
// take one (labels, function names, file paths).
NS::String* makeString(const char* text)
{
    return NS::String::string(text, NS::UTF8StringEncoding);
}

}  // close unnamed namespace

VikingRoom::VikingRoom()
: d_layer(nullptr)
, d_colorFormat(MTL::PixelFormatBGRA8Unorm_sRGB)
, d_depthFormat(MTL::PixelFormatDepth32Float)
, d_frameSemaphore(nullptr)
, d_frameIndex(0)
, d_framebufferResized(false)
, d_window(nullptr)
{
}

VikingRoom::~VikingRoom() = default;

void VikingRoom::run()
{
    initWindow();
    initMetal();
    createGraphicsPipeline();
    createDepthResources();
    createTextureImage();
    createTextureSampler();
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

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    d_window = glfwCreateWindow(k_WINDOW_WIDTH,
                                k_WINDOW_HEIGHT,
                                "Viking Room - Metal",
                                nullptr,
                                nullptr);
    glfwSetWindowUserPointer(d_window, this);
    glfwSetFramebufferSizeCallback(
        d_window, [](GLFWwindow* window, int /*width*/, int /*height*/) {
            auto* app = static_cast<VikingRoom*>(
                glfwGetWindowUserPointer(window));
            app->setFrameBufferResied(true);
        });
}

void VikingRoom::initMetal()
{
    d_device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    if (!d_device) {
        std::cerr << "Failed to create the system default Metal device\n";
        return;
    }

    d_layer = static_cast<CA::MetalLayer*>(triAttachMetalLayer(d_window));
    if (d_layer == nullptr) {
        std::cerr << "Failed to attach a CAMetalLayer to the window\n";
        return;
    }

    d_layer->setDevice(d_device.get());
    d_layer->setPixelFormat(d_colorFormat);

    int width  = 0;
    int height = 0;
    glfwGetFramebufferSize(d_window, &width, &height);
    d_layer->setDrawableSize(CGSizeMake(static_cast<double>(width),
                                        static_cast<double>(height)));
    d_commandQueue   = NS::TransferPtr(d_device->newCommandQueue());
    d_frameSemaphore = dispatch_semaphore_create(k_MAX_FRAMES_IN_FLIGHT);
}

void VikingRoom::createGraphicsPipeline()
{
    NS::Error*     error = nullptr;
    const NS::URL* url   = NS::URL::fileURLWithPath(makeString(k_SHADER_PATH));
    d_library            = NS::TransferPtr(d_device->newLibrary(url, &error));
    if (!d_library) {
        std::cerr << "Failed to load shader library: "
                  << (error != nullptr
                          ? error->localizedDescription()->utf8String()
                          : "unknown error")
                  << '\n';
        return;
    }

    const NS::SharedPtr<MTL::Function> vertFn = NS::TransferPtr(
        d_library->newFunction(makeString("vertMain")));
    const NS::SharedPtr<MTL::Function> fragFn = NS::TransferPtr(
        d_library->newFunction(makeString("fragMain")));

    const MTL::VertexDescriptor* vertexDescriptor =
        MTL::VertexDescriptor::vertexDescriptor();

    MTL::VertexAttributeDescriptor* posAttr = vertexDescriptor->attributes()
                                                  ->object(0);
    posAttr->setFormat(MTL::VertexFormatFloat3);
    posAttr->setOffset(offsetof(Vertex, pos));
    posAttr->setBufferIndex(k_VERTEX_BUFFER_INDEX);

    MTL::VertexAttributeDescriptor* colorAttr = vertexDescriptor->attributes()
                                                    ->object(1);
    colorAttr->setFormat(MTL::VertexFormatFloat3);
    colorAttr->setOffset(offsetof(Vertex, color));
    colorAttr->setBufferIndex(k_VERTEX_BUFFER_INDEX);

    MTL::VertexAttributeDescriptor* texAttr = vertexDescriptor->attributes()
                                                  ->object(2);
    texAttr->setFormat(MTL::VertexFormatFloat2);
    texAttr->setOffset(offsetof(Vertex, texCoord));
    texAttr->setBufferIndex(k_VERTEX_BUFFER_INDEX);

    MTL::VertexBufferLayoutDescriptor* layout =
        vertexDescriptor->layouts()->object(k_VERTEX_BUFFER_INDEX);
    layout->setStride(sizeof(Vertex));
    layout->setStepFunction(MTL::VertexStepFunctionPerVertex);
    layout->setStepRate(1);

    MTL::RenderPipelineDescriptor* pipelineDescriptor =
        MTL::RenderPipelineDescriptor::alloc()->init();
    pipelineDescriptor->setVertexFunction(vertFn.get());
    pipelineDescriptor->setFragmentFunction(fragFn.get());
    pipelineDescriptor->setVertexDescriptor(vertexDescriptor);
    pipelineDescriptor->colorAttachments()->object(0)->setPixelFormat(
        d_colorFormat);
    pipelineDescriptor->setDepthAttachmentPixelFormat(d_depthFormat);
    pipelineDescriptor->setLabel(makeString("viking-room"));

    d_pipelineState = NS::TransferPtr(
        d_device->newRenderPipelineState(pipelineDescriptor, &error));
    pipelineDescriptor->release();
    if (!d_pipelineState) {
        std::cerr << "Failed to create pipeline state: "
                  << (error != nullptr
                          ? error->localizedDescription()->utf8String()
                          : "unknown error")
                  << '\n';
        return;
    }

    MTL::DepthStencilDescriptor* depthDescriptor =
        MTL::DepthStencilDescriptor::alloc()->init();
    depthDescriptor->setDepthCompareFunction(MTL::CompareFunctionLess);
    depthDescriptor->setDepthWriteEnabled(true);
    d_depthStencilState = NS::TransferPtr(
        d_device->newDepthStencilState(depthDescriptor));
    depthDescriptor->release();
}

void VikingRoom::createDepthResources()
{
    const CGSize size = d_layer->drawableSize();

    MTL::TextureDescriptor* descriptor =
        MTL::TextureDescriptor::texture2DDescriptor(
            d_depthFormat,
            static_cast<NS::UInteger>(size.width),
            static_cast<NS::UInteger>(size.height),
            false);
    descriptor->setUsage(MTL::TextureUsageRenderTarget);
    descriptor->setStorageMode(MTL::StorageModePrivate);

    d_depthTexture = NS::TransferPtr(d_device->newTexture(descriptor));
    descriptor->release();
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

    MTL::TextureDescriptor* descriptor =
        MTL::TextureDescriptor::texture2DDescriptor(
            MTL::PixelFormatRGBA8Unorm_sRGB,
            static_cast<NS::UInteger>(texWidth),
            static_cast<NS::UInteger>(texHeight),
            false);
    descriptor->setUsage(MTL::TextureUsageShaderRead);

    d_textureImage = NS::TransferPtr(d_device->newTexture(descriptor));
    descriptor->release();

    const MTL::Region region = MTL::Region::Make2D(
        0,
        0,
        static_cast<NS::UInteger>(texWidth),
        static_cast<NS::UInteger>(texHeight));
    const NS::UInteger bytesPerRow = static_cast<NS::UInteger>(texWidth) * 4u;

    d_textureImage->replaceRegion(region, 0, pixels, bytesPerRow);

    stbi_image_free(pixels);
}

void VikingRoom::createTextureSampler()
{
    MTL::SamplerDescriptor* descriptor = MTL::SamplerDescriptor::alloc()
                                             ->init();
    descriptor->setMinFilter(MTL::SamplerMinMagFilterLinear);
    descriptor->setMagFilter(MTL::SamplerMinMagFilterLinear);
    descriptor->setMipFilter(MTL::SamplerMipFilterLinear);
    descriptor->setSAddressMode(MTL::SamplerAddressModeRepeat);
    descriptor->setTAddressMode(MTL::SamplerAddressModeRepeat);
    descriptor->setRAddressMode(MTL::SamplerAddressModeRepeat);
    descriptor->setMaxAnisotropy(16);

    d_sampler = NS::TransferPtr(d_device->newSamplerState(descriptor));
    descriptor->release();
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

NS::SharedPtr<MTL::Buffer> VikingRoom::createBuffer(const void*       data,
                                                    const std::size_t size,
                                                    const char* label) const
{
    NS::SharedPtr<MTL::Buffer> buffer = NS::TransferPtr(
        d_device->newBuffer(data, size, MTL::ResourceStorageModeShared));
    buffer->setLabel(makeString(label));
    return buffer;
}

void VikingRoom::createVertexBuffer()
{
    d_vertexBuffer = createBuffer(d_vertices.data(),
                                  sizeof(Vertex) * d_vertices.size(),
                                  "vertices");
}

void VikingRoom::createIndexBuffer()
{
    d_indexBuffer = createBuffer(d_indices.data(),
                                 sizeof(d_indices[0]) * d_indices.size(),
                                 "indices");
}

void VikingRoom::createUniformBuffers()
{
    d_uniformBuffers.reserve(k_MAX_FRAMES_IN_FLIGHT);
    for (int i = 0; i < k_MAX_FRAMES_IN_FLIGHT; i++) {
        NS::SharedPtr<MTL::Buffer> buffer = NS::TransferPtr(
            d_device->newBuffer(sizeof(UniformBufferObject),
                                MTL::ResourceStorageModeShared));
        buffer->setLabel(makeString("uniforms"));
        d_uniformBuffers.emplace_back(std::move(buffer));
    }
}

void VikingRoom::updateUniformBuffer(const std::uint32_t frameIndex) const
{
    static auto startTime = std::chrono::high_resolution_clock::now();

    const auto  currentTime = std::chrono::high_resolution_clock::now();
    const float time = std::chrono::duration<float>(currentTime - startTime)
                           .count();

    const CGSize size   = d_layer->drawableSize();
    const float  aspect = static_cast<float>(size.width) /
                         static_cast<float>(size.height);

    UniformBufferObject ubo{};
    ubo.model = rotate(glm::mat4(1.0f),
                       time * glm::radians(90.0f),
                       glm::vec3(0.0f, 0.0f, 1.0f));
    ubo.view  = lookAt(glm::vec3(2.0f, 2.0f, 2.0f),
                      glm::vec3(0.0f, 0.0f, 0.0f),
                      glm::vec3(0.0f, 0.0f, 1.0f));
    ubo.proj  = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 10.0f);

    std::memcpy(d_uniformBuffers[frameIndex]->contents(), &ubo, sizeof(ubo));
}

void VikingRoom::recordCommands(MTL::RenderCommandEncoder* encoder,
                                const CA::MetalDrawable*   drawable) const
{
    const auto width  = drawable->texture()->width();
    const auto height = drawable->texture()->height();

    encoder->setViewport(MTL::Viewport{
        .originX = 0.0,
        .originY = 0.0,
        .width   = static_cast<double>(width),
        .height  = static_cast<double>(height),
        .znear   = 0.0,
        .zfar    = 1.0,
    });

    encoder->setRenderPipelineState(d_pipelineState.get());
    encoder->setDepthStencilState(d_depthStencilState.get());

    encoder->setFrontFacingWinding(MTL::WindingCounterClockwise);
    encoder->setCullMode(MTL::CullModeBack);

    encoder->setVertexBuffer(d_vertexBuffer.get(), 0, k_VERTEX_BUFFER_INDEX);

    const MTL::Buffer* uniforms = d_uniformBuffers[d_frameIndex].get();
    encoder->setVertexBuffer(uniforms, 0, k_UNIFORM_BUFFER_INDEX);
    encoder->setFragmentBuffer(uniforms, 0, k_UNIFORM_BUFFER_INDEX);
    encoder->setVertexTexture(d_textureImage.get(), 0);
    encoder->setFragmentTexture(d_textureImage.get(), 0);
    encoder->setVertexSamplerState(d_sampler.get(), 0);
    encoder->setFragmentSamplerState(d_sampler.get(), 0);

    encoder->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle,
                                   d_indices.size(),
                                   MTL::IndexTypeUInt32,
                                   d_indexBuffer.get(),
                                   0);
}

void VikingRoom::recreateDrawable()
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

    d_layer->setDrawableSize(CGSizeMake(static_cast<double>(width),
                                        static_cast<double>(height)));

    createDepthResources();
}

void VikingRoom::drawFrame()
{
    if (d_framebufferResized) {
        d_framebufferResized = false;
        recreateDrawable();
    }

    dispatch_semaphore_wait(d_frameSemaphore, DISPATCH_TIME_FOREVER);

    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

    const CA::MetalDrawable* drawable = d_layer->nextDrawable();
    if (drawable == nullptr) {
        dispatch_semaphore_signal(d_frameSemaphore);
        pool->release();
        return;
    }

    updateUniformBuffer(d_frameIndex);

    const MTL::RenderPassDescriptor* passDescriptor =
        MTL::RenderPassDescriptor::renderPassDescriptor();

    MTL::RenderPassColorAttachmentDescriptor* colorAttachment =
        passDescriptor->colorAttachments()->object(0);
    colorAttachment->setTexture(drawable->texture());
    colorAttachment->setLoadAction(MTL::LoadActionClear);
    colorAttachment->setStoreAction(MTL::StoreActionStore);
    colorAttachment->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));

    MTL::RenderPassDepthAttachmentDescriptor* depthAttachment =
        passDescriptor->depthAttachment();
    depthAttachment->setTexture(d_depthTexture.get());
    depthAttachment->setLoadAction(MTL::LoadActionClear);
    depthAttachment->setStoreAction(MTL::StoreActionDontCare);
    depthAttachment->setClearDepth(1.0);

    MTL::CommandBuffer*        commandBuffer = d_commandQueue->commandBuffer();
    MTL::RenderCommandEncoder* encoder = commandBuffer->renderCommandEncoder(
        passDescriptor);

    recordCommands(encoder, drawable);

    encoder->endEncoding();
    commandBuffer->presentDrawable(drawable);

    dispatch_semaphore_t semaphore = d_frameSemaphore;
    commandBuffer->addCompletedHandler(
        [semaphore](MTL::CommandBuffer* /*buffer*/) {
            dispatch_semaphore_signal(semaphore);
        });

    commandBuffer->commit();

    d_frameIndex = (d_frameIndex + 1) % k_MAX_FRAMES_IN_FLIGHT;

    pool->release();
}

void VikingRoom::mainLoop()
{
    while (glfwWindowShouldClose(d_window) == 0) {
        // Check for events like pressing the close button.
        glfwPollEvents();
        drawFrame();
    }
}

void VikingRoom::cleanUp()
{
    glfwDestroyWindow(d_window);
    glfwTerminate();

    if (d_layer != nullptr) {
        d_layer->release();
        d_layer = nullptr;
    }
}

}  // close package namespace
