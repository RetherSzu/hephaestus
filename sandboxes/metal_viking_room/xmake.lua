-- =============================================================================
-- Viking Room (Metal)
-- Build Configuration
-- =============================================================================

set_project("viking-room-metal")
set_version("0.1.0")
set_description("A Metal Viking room using metal-cpp, GLFW, GLM and C++23")

set_languages("cxx23")

-- 'lsp = "clangd"' keeps third-party headers (metal-cpp, GLFW, GLM) as
-- '-isystem' in compile_commands.json, as the build uses them. Without it
-- xmake writes them as '-I', and clang-tidy then reports their code as
-- findings in ours. clangd reads '-isystem'.
add_rules("plugin.compile_commands.autoupdate",
          {outputdir = ".", lsp = "clangd"})

add_rules("mode.debug", "mode.release")


-- =============================================================================
-- Compiler Flags

rule("metal.warnings")
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
rule("metal.no_exceptions")
    on_load(function (target)
        target:add("cxxflags", "-fno-exceptions",
                   {tools = {"clang", "gcc", "apple_clang"}})
    end)
rule_end()

rule("metal.werror")
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

-- Compile each Slang shader to a Metal library with 'slangc'. The '.slang'
-- file holds both entry points ('vertMain' as vertex, 'fragMain' as
-- fragment), so they are emitted into a single '.metallib' next to the
-- source, where the runtime loads it ('newLibrary("shaders/shader.metallib")').
-- Slang drives the Metal toolchain ('xcrun metal'/'metallib') under the hood,
-- the direct counterpart to the Vulkan build's 'slangc -target spirv'.
rule("metal.slang2metallib")
    set_extensions(".slang")
    on_buildcmd_file(function (target, batchcmds, sourcefile, opt)
        local libfile = path.join(path.directory(sourcefile),
                                  path.basename(sourcefile) .. ".metallib")

        batchcmds:show_progress(opt.progress,
            "${color.build.object}compiling.slang %s", sourcefile)
        batchcmds:mkdir(path.directory(libfile))
        batchcmds:vrunv("slangc", {sourcefile,
            "-target", "metallib",
            "-entry", "vertMain", "-stage", "vertex",
            "-entry", "fragMain", "-stage", "fragment",
            "-o", libfile})

        -- Rebuild the '.metallib' only when the '.slang' source is newer.
        batchcmds:add_depfiles(sourcefile)
        batchcmds:set_depmtime(os.mtime(libfile))
        batchcmds:set_depcache(target:dependfile(libfile))
    end)
rule_end()


-- =============================================================================
-- Package Dependencies

-- Apple's low-overhead C++ bindings for Metal (https://developer.apple.com/
-- metal/cpp). The package defines 'MTL_PRIVATE_IMPLEMENTATION' and its
-- siblings for every translation unit, so 'main.cpp' undefines them to keep
-- 'viking_room.cpp' the single implementation TU (see the comment there).
add_requires("metal-cpp")

-- Window creation (https://www.glfw.org). GLFW makes only a plain Cocoa
-- window; 'metal_layer.mm' attaches the CAMetalLayer that Metal draws into.
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

target("viking-room-metal")
    set_kind("binary")

    add_rules("metal.warnings", "metal.werror", "metal.no_exceptions")

    -- Headers are included with angle brackets, '<viking_room.h>', from this
    -- sandbox's own directory. 'os.scriptdir()' (this xmake.lua's directory)
    -- keeps the path correct whether this is built standalone or included from
    -- the root 'hephaestus' project.
    add_includedirs(os.scriptdir())

    -- First-party C++ sources plus the Objective-C++ bridge that installs the
    -- CAMetalLayer on GLFW's Cocoa window.
    add_files("*.cpp")
    add_files("metal_layer.mm")

    -- Compile the shared 'shaders/*.slang' (at the repository root) to a Metal
    -- library as part of the build. The slang rule emits the '.metallib' next
    -- to its source, i.e. into the root 'shaders/' where the runtime loads it.
    add_rules("metal.slang2metallib")
    add_files(path.join(assets_root, "shaders", "*.slang"))

    -- Run from the repository root so the relative shader, texture and model
    -- paths in the code resolve against the shared root folders. 'xmake run'
    -- otherwise starts in the binary's output directory, where those paths do
    -- not exist.
    set_rundir(assets_root)

    -- Dependencies
    add_packages("metal-cpp", "glfw", "glm", "stb", "tinyobjloader")

    -- Metal clip space depth is [0, 1], not OpenGL's [-1, 1].
    add_defines("GLM_FORCE_DEPTH_ZERO_TO_ONE")

    -- macOS only (Metal is Apple's native API). GLFW needs Cocoa, IOKit and
    -- CoreVideo; the renderer needs QuartzCore (CAMetalLayer) and CoreGraphics
    -- (CGSize). AppKit backs the Objective-C++ bridge.
    if is_plat("macosx") then
        add_frameworks("Metal", "Foundation", "QuartzCore", "CoreGraphics",
                       "AppKit", "Cocoa", "IOKit", "CoreVideo")
    else
        -- Metal is available on Apple platforms only.
        on_load(function (target)
            raise("viking-room-metal targets macOS; Metal is Apple-only.")
        end)
    end
