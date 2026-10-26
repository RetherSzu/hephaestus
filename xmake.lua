-- =============================================================================
-- Hephaestus
-- Root Build Configuration (umbrella project)
-- =============================================================================
--
-- This file ties the whole repository together so every target can be built
-- and run from the root, e.g.
--
--   xmake                          -- build every target
--   xmake build viking-room-vulkan -- build one target
--   xmake run   viking-room-vulkan -- build and run the Vulkan sandbox
--   xmake run   viking-room-metal  -- build and run the Metal sandbox
--
-- Each sandbox keeps its own self-contained 'xmake.lua' (packages, compiler
-- flags, shader compilation, target definition) and stays buildable on its
-- own from inside its directory. 'includes()' below pulls those target
-- definitions into this single umbrella project; the sandboxes use
-- 'os.scriptdir()' for their include and run directories, so their relative
-- shader / model / texture paths resolve whether built standalone or from here.

set_project("hephaestus")
set_version("0.1.0")
set_description("Hephaestus — graphics RHI learning sandboxes and final app")

set_languages("cxx23")

-- Available to every included target: 'xmake f -m debug' / 'xmake f -m release'.
add_rules("mode.debug", "mode.release")

-- Emit a single 'compile_commands.json' at the repository root covering every
-- target, refreshed on each build, for clangd / CLion opened at the root.
-- 'lsp = "clangd"' keeps third-party headers as '-isystem' (not '-I') so
-- clang-tidy does not lint their code as ours. Each sandbox keeps its own
-- autoupdate rule too, for when it is opened or built on its own.
add_rules("plugin.compile_commands.autoupdate", {outputdir = ".", lsp = "clangd"})

-- =============================================================================
-- Sub-projects

-- Learning sandboxes: isolated single-backend loaders.
includes("sandboxes/vulkan_viking_room")
includes("sandboxes/metal_viking_room")

-- The final, refactored application that will sit on top of the RHI.
-- Uncomment once 'app/' has its own 'xmake.lua'.
-- includes("app")
