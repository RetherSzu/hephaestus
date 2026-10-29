-- =============================================================================
-- Viking Room (DirectX 12)
-- Build Configuration
-- =============================================================================

set_project("viking-room-dx12")
set_version("0.1.0")
set_description("A DirectX 12 Viking room using GLFW, GLM and C++23")

set_languages("cxx23")

-- See the Vulkan sandbox: keeps third-party headers as '-isystem' in
-- compile_commands.json so clang-tidy does not lint them.
-- Not on Windows: CLion probes the real 'cl.exe' with these flags, and the
-- clangd flavor adds clang-cl-only '-imsvc' that 'cl.exe' rejects.
add_rules("plugin.compile_commands.autoupdate",
          {outputdir = ".", lsp = not is_host("windows") and "clangd" or nil})

add_rules("mode.debug", "mode.release")


-- =============================================================================
-- Compiler Flags

-- DirectX 12 is Windows-only, so MSVC is the toolchain here; the clang flags
-- mirror the Vulkan sandbox for a clang-cl or clangd setup.
rule("dx12.warnings")
    on_load(function (target)
        target:add("cxxflags",
            -- Highest practical warning level.
            "/W4",
            -- Enforce standard C++ conformance.
            "/permissive-",
            {tools = {"cl"}}
        )
        target:add("cxxflags",
            "-Wall", "-Wextra", "-Wpedantic", "-Wshadow", "-Wnon-virtual-dtor",
            "-Wold-style-cast", "-Wcast-align", "-Wunused", "-Woverloaded-virtual",
            "-Wconversion", "-Wsign-conversion",
            {tools = {"clang", "gcc"}}
        )
    end)
rule_end()

-- Errors are values, as in the Vulkan sandbox: no throw, no catch.
rule("dx12.no_exceptions")
    on_load(function (target)
        -- '/EHs-c-' on MSVC, '-fno-exceptions' on clang / gcc.
        target:set("exceptions", "none")
        target:add("defines", "_HAS_EXCEPTIONS=0")
    end)
rule_end()

rule("dx12.werror")
    on_load(function (target)
        target:add("cxxflags", "/WX", {tools = {"cl"}})
        target:add("cxxflags", "-Werror", {tools = {"clang", "gcc"}})
    end)
rule_end()


-- =============================================================================
-- Shader Compilation

-- Compile each Slang shader to DXIL with 'slangc' (Vulkan SDK), which loads
-- the SDK's 'dxcompiler.dll'. A DXIL blob holds one entry point, so the vertex
-- and pixel stages are emitted as two files next to the source:
-- 'shaders/<name>.vs.dxil' and 'shaders/<name>.ps.dxil'.
rule("dx12.slang2dxil")
    set_extensions(".slang")
    on_buildcmd_file(function (target, batchcmds, sourcefile, opt)
        local dir = path.directory(sourcefile)
        local base = path.basename(sourcefile)
        local stages = {
            {entry = "vertMain", stage = "vertex",   out = path.join(dir, base .. ".vs.dxil")},
            {entry = "fragMain", stage = "fragment", out = path.join(dir, base .. ".ps.dxil")},
        }

        batchcmds:show_progress(opt.progress,
            "${color.build.object}compiling.slang %s", sourcefile)
        for _, s in ipairs(stages) do
            batchcmds:vrunv("slangc", {sourcefile,
                "-target", "dxil",
                "-profile", "sm_6_0",
                "-entry", s.entry, "-stage", s.stage,
                "-o", s.out})
        end

        -- Rebuild only when the '.slang' source is newer than its outputs.
        batchcmds:add_depfiles(sourcefile)
        batchcmds:set_depmtime(os.mtime(stages[1].out))
        batchcmds:set_depcache(target:dependfile(stages[1].out))
    end)
rule_end()


-- =============================================================================
-- Package Dependencies

-- Direct3D 12 / DXGI come from the Windows SDK (linked below), not a package.

-- Window creation (https://www.glfw.org); the HWND comes from
-- 'glfwGetWin32Window' with 'GLFW_EXPOSE_NATIVE_WIN32'.
add_requires("glfw")

-- Header-only mathematics (https://github.com/g-truc/glm)
add_requires("glm")

-- Header-only image loading (https://github.com/nothings/stb)
add_requires("stb")

-- Header-only Wavefront '.obj' loader
-- (https://github.com/tinyobjloader/tinyobjloader)
add_requires("tinyobjloader")


-- =============================================================================
-- Target: Viking Room (Executable)

-- Shared assets live at the repository root, two directories up.
local assets_root = path.absolute(path.join(os.scriptdir(), "..", ".."))

target("dx12")
    set_kind("binary")

    -- DirectX 12 exists only on Windows.
    set_enabled(is_plat("windows"))

    add_rules("dx12.warnings", "dx12.werror", "dx12.no_exceptions")

    add_includedirs(os.scriptdir())
    add_files("*.cpp")

    add_rules("dx12.slang2dxil")
    add_files(path.join(assets_root, "shaders", "*.slang"))

    -- Run from the repository root so 'shaders/...', 'models/...' and
    -- 'textures/...' resolve against the shared folders.
    set_rundir(assets_root)

    add_packages("glfw", "glm", "stb", "tinyobjloader")
    add_syslinks("d3d12", "dxgi", "dxguid")

    -- Direct3D clip space depth is [0, 1], not OpenGL's [-1, 1].
    add_defines("GLM_FORCE_DEPTH_ZERO_TO_ONE")
    -- Keep <windows.h> lean and free of the 'min' / 'max' macros.
    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX")
