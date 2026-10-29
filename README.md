# Hephaestus

Graphics RHI learning sandboxes and final app.

Hephaestus is a workspace for learning low-level graphics programming across
multiple backends, with the goal of building a Render Hardware Interface (RHI)
— a thin abstraction over Vulkan, DirectX 12 and Metal — and a final application on top of
it. It starts as a set of isolated, single-backend sandboxes that each render
the classic *Viking Room* scene, so the same problem can be studied one API at
a time before the common abstraction is extracted.

## Layout

```
hephaestus/
├── sandboxes/                 Isolated single-backend loaders
│   └── vulkan_viking_room/    Viking Room rendered with Vulkan
├── app/                       Final application on top of the RHI (WIP)
├── shaders/                   Shared Slang shaders (shader.slang)
├── models/                    Shared meshes (viking_room.obj)
├── textures/                  Shared textures (viking_room.png)
└── xmake.lua                  Umbrella build tying every target together
```

Assets in `shaders/`, `models/`, and `textures/` are shared by every sandbox.
Each sandbox also keeps its own self-contained `xmake.lua` and stays buildable
on its own from inside its directory; the umbrella `xmake.lua` at the root pulls
those target definitions into a single project.

## Requirements

- [xmake](https://xmake.io) as the build system
- A C++23 toolchain
- [Slang](https://github.com/shader-slang/slang) (`slangc`) to compile shaders
- The Vulkan SDK (for the Vulkan sandbox)
- macOS with the Metal toolchain (for the Metal sandbox)

All other dependencies — GLFW, GLM, stb, tinyobjloader, `vulkansdk` — are resolved automatically by xmake.

## Build & run

From the repository root:

```sh
xmake            # build every target
xmake build vlk  # build one target
xmake run   vlk  # build and run the Vulkan sandbox
xmake run   dx12 # build and run the DirectX 12 sandbox
```

Switch build mode with `xmake f -m debug` or `xmake f -m release`.

Slang shaders are compiled as part of the build: to SPIR-V (`.spv`) for Vulkan
and to a Metal library (`.metallib`) for Metal, emitted next to their source in
`shaders/` where each runtime loads them.
