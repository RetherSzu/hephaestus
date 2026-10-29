-- =============================================================================
-- Viking Room (Vulkan)
-- Build Configuration
-- =============================================================================

set_project("viking-room-vulkan")
set_version("0.1.0")
set_description("A Vulkan Viking room using GLFW, GLM and C++23")

set_languages("cxx23")

-- 'lsp = "clangd"' keeps third-party headers (the Vulkan SDK, GLFW, GLM) as
-- '-isystem' in compile_commands.json, as the build uses them. Without it
-- xmake writes them as '-I', and clang-tidy then reports their code as
-- findings in ours. clangd reads '-isystem'.
add_rules("plugin.compile_commands.autoupdate",
          {outputdir = ".", lsp = not is_host("windows") and "clangd" or nil})

add_rules("mode.debug", "mode.release")


-- =============================================================================
-- Compiler Flags

rule("vulkan.warnings")
    on_load(function (target)
        target:add("cxxflags",
            -- Enable common / standard warning diagnostics.
            "-Wall",
            -- Enable additional warning diagnostics not covered by -Wall.
            "-Wextra",
            -- Enforce strict ISO C++ compliance and reject non-standard extensions.
            "-Wpedantic",
            -- Warn when a variable shadows another variable in an outer scope.
            "-Wshadow",
            -- Warn if a class with virtual functions lacks a virtual destructor.
            "-Wnon-virtual-dtor",
            -- Warn on C-style casts in C++.
            "-Wold-style-cast",
            -- Warn when pointer casting increases required memory alignment.
            "-Wcast-align",
            -- Warn on unused variables, functions, parameters, or expressions.
            "-Wunused",
            -- Warn when a function hides a virtual function from a base class.
            "-Woverloaded-virtual",
            -- Warn on implicit type conversions that may alter values or lose precision.
            "-Wconversion",
            -- Warn on implicit conversions between signed and unsigned types.
            "-Wsign-conversion",
            {tools = {"clang", "gcc", "apple_clang"}}
        )
    end)
rule_end()

-- Errors are values. First-party code neither throws nor catches, and
-- '-fno-exceptions' makes that a compile error rather than a convention.
rule("vulkan.no_exceptions")
    on_load(function (target)
        target:add("cxxflags", "-fno-exceptions",
                   {tools = {"clang", "gcc", "apple_clang"}})
    end)
rule_end()

rule("vulkan.werror")
    on_load(function (target)
        target:add("cxxflags",
            -- Treat all warnings as compilation errors.
            "-Werror",
            {tools = {"clang", "gcc", "apple_clang"}}
        )
    end)
rule_end()


-- =============================================================================
-- Shader Compilation

-- Compile each Slang shader to SPIR-V with 'slangc', shipped with the Vulkan
-- SDK. The '.slang' file holds both entry points ('vertMain' as vertex,
-- 'fragMain' as fragment), so they are emitted into a single '.spv' next to
-- the source, where the runtime reads it ('readFile("shaders/shader.spv")').
rule("vulkan.slang2spv")
    set_extensions(".slang")
    on_buildcmd_file(function (target, batchcmds, sourcefile, opt)
        local spvfile = path.join(path.directory(sourcefile),
                                  path.basename(sourcefile) .. ".spv")

        batchcmds:show_progress(opt.progress,
            "${color.build.object}compiling.slang %s", sourcefile)
        batchcmds:mkdir(path.directory(spvfile))
        batchcmds:vrunv("slangc", {sourcefile,
            "-target", "spirv",
            "-emit-spirv-directly",
            "-entry", "vertMain", "-stage", "vertex",
            "-entry", "fragMain", "-stage", "fragment",
            "-o", spvfile})

        -- Rebuild the '.spv' only when the '.slang' source is newer.
        batchcmds:add_depfiles(sourcefile)
        batchcmds:set_depmtime(os.mtime(spvfile))
        batchcmds:set_depcache(target:dependfile(spvfile))
    end)
rule_end()


-- =============================================================================
-- Package Dependencies

-- Vulkan SDK (LunarG / system Vulkan loader & headers)
--
-- Found through $VULKAN_SDK, then pkg-config, then /usr on Linux only. On
-- macOS that means VULKAN_SDK must be set, else xmake reports the package
-- as "not found, please run `xmake repo -u`", which is misleading:
--
--   export VULKAN_SDK=/usr/local                       # system-wide install
--   source ~/VulkanSDK/<version>/setup-env.sh          # or the SDK's own
add_requires("vulkansdk")

-- Window and surface creation (https://www.glfw.org). Not built with
-- 'glfw_include = "vulkan"': the package's install check then includes
-- <vulkan/vulkan.h> without the SDK on its path and fails. Including
-- <vulkan/vulkan.h> before <GLFW/glfw3.h> gives the same declarations.
add_requires("glfw")

-- Header-only mathematics (https://github.com/g-truc/glm)
add_requires("glm")

-- Header-only image loading (https://github.com/nothings/stb). Provides
-- 'stb_image.h'; define 'STB_IMAGE_IMPLEMENTATION' in exactly one '.cpp'
-- before the include to emit the implementation.
add_requires("stb")

-- Header-only Wavefront '.obj' loader (https://github.com/tinyobjloader/
-- tinyobjloader). Provides 'tiny_obj_loader.h'; define
-- 'TINYOBJLOADER_IMPLEMENTATION' in exactly one '.cpp' before the include.
add_requires("tinyobjloader")


-- =============================================================================
-- Target: Viking Room (Executable)

-- Shaders, models and textures are shared at the repository root (two
-- directories up from this sandbox), so every backend sandbox and the final
-- app use a single copy. 'os.scriptdir()' is this file's directory, correct
-- whether built standalone or from the root umbrella project.
local assets_root = path.absolute(path.join(os.scriptdir(), "..", ".."))

target("vlk")
    set_kind("binary")

    add_rules("vulkan.warnings", "vulkan.werror", "vulkan.no_exceptions")

    -- Headers are included with angle brackets, '<viking_room.h>', from this
    -- sandbox's own directory. 'os.scriptdir()' (this xmake.lua's directory)
    -- rather than 'os.projectdir()' so the path stays correct whether this is
    -- built standalone or included from the root 'hephaestus' project, where
    -- 'projectdir' would resolve to the repository root. An absolute path
    -- rather than '.' so the '-I' in compile_commands.json resolves no matter
    -- which directory a tool (clang-tidy, clangd) runs it from.
    add_includedirs(os.scriptdir())

    add_files("*.cpp")

    -- Compile the shared 'shaders/*.slang' (at the repository root) to SPIR-V
    -- as part of the build. The slang rule emits the '.spv' next to its source,
    -- i.e. into the root 'shaders/' where the runtime reads it.
    add_rules("vulkan.slang2spv")
    add_files(path.join(assets_root, "shaders", "*.slang"))

    -- Run from the repository root so the relative asset paths in the code
    -- ('shaders/shader.spv', 'models/...', 'textures/...') resolve against the
    -- shared root folders. 'xmake run' otherwise starts in the binary's output
    -- directory, where those paths do not exist.
    set_rundir(assets_root)

    -- Dependencies
    add_packages("vulkansdk", "glfw", "glm", "stb", "tinyobjloader")

    -- Vulkan clip space depth is [0, 1], not OpenGL's [-1, 1].
    add_defines("GLM_FORCE_DEPTH_ZERO_TO_ONE")

    -- Cross-platform configuration:
    -- 1. macOS (Apple Silicon / arm64) using MoltenVK / Metal
    if is_plat("macosx") then
        add_frameworks("Metal", "Foundation", "QuartzCore", "Cocoa", "IOKit", "CoreVideo")
        add_defines("VK_USE_PLATFORM_METAL_EXT")

        -- The LunarG loader ships with '@rpath/libvulkan.1.dylib' as its install
        -- name, so every executable linking it needs an rpath to the SDK, else
        -- it builds fine but dies at load time with "Library not loaded".
        local vulkan_sdk = os.getenv("VULKAN_SDK")
        if vulkan_sdk then
            add_rpathdirs(path.join(vulkan_sdk, "lib"))
        end
        -- Default location used by the LunarG system-wide installer.
        add_rpathdirs("/usr/local/lib")
    -- 2. Linux (x86_64) using native NVIDIA GPU Vulkan driver
    elseif is_plat("linux") then
        add_syslinks("pthread", "dl", "m")
        add_defines("VK_USE_PLATFORM_XLIB_KHR", "VK_USE_PLATFORM_WAYLAND_KHR")
    end
