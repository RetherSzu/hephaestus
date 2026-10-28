// metal_layer.mm                                                     -*-C++-*-
//
// GLFW creates a plain Cocoa 'NSWindow' with no rendering layer. Metal draws
// into a 'CAMetalLayer', so this translation unit bridges the two: it reaches
// the window's content view through GLFW's native handle and installs a
// freshly created layer on it. The work must live in an Objective-C++ (.mm)
// file because it speaks to AppKit and QuartzCore through the Objective-C
// runtime; the rest of the project is plain C++ and talks to the same layer
// through metal-cpp's 'CA::MetalLayer' wrapper, which is binary compatible
// with the 'CAMetalLayer*' returned here.

// third party
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

// system
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

// Create a 'CAMetalLayer', make it the backing layer of 'window''s content
// view, and return it (retained; the caller owns the returned reference). The
// layer is handed back as a 'void*' and reinterpreted as 'CA::MetalLayer*' on
// the C++ side.
extern "C" void* triAttachMetalLayer(GLFWwindow* window)
{
    NSWindow* nsWindow = glfwGetCocoaWindow(window);
    if (nsWindow == nil) {
        return nullptr;
    }

    CAMetalLayer* layer = [CAMetalLayer layer];

    // Match the layer's point-to-pixel ratio to the window's screen so the
    // drawable is sized in native pixels on Retina displays.
    layer.contentsScale = nsWindow.backingScaleFactor;

    NSView* view     = nsWindow.contentView;
    view.wantsLayer  = YES;
    view.layer       = layer;

    // This file is compiled under ARC. '__bridge_retained' hands an owning
    // (+1) reference out to the C++ caller, which balances it with a single
    // 'release()' through metal-cpp during teardown.
    return (__bridge_retained void*)layer;
}
