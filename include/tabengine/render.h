#pragma once

#include "tabengine/types.h"

#include <cstdint>
#include <memory>
#include <vector>

class SkCanvas;

namespace tabengine {

enum class RenderBackend { None, Raster, D3D12 };

struct RenderInfo {
    RenderBackend backend = RenderBackend::None;
    Size surface_size{};
};

// One independent pixel copy of a rendered frame, produced by the renderer
// (never by reading the live canvas). Pixels are 32-bit premultiplied BGRA
// (Skia N32) with `size.width * 4` stride, sRGB.
struct CapturedFrame {
    RenderBackend backend = RenderBackend::None;
    Size size{};
    std::vector<std::uint8_t> pixels;
    [[nodiscard]] bool valid() const {
        return size.width > 0 && size.height > 0 &&
               pixels.size() == static_cast<std::size_t>(size.width) * 4 *
                                    static_cast<std::size_t>(size.height);
    }
};

class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual bool attach(WindowId id, void* native_handle, Size size) = 0;
    virtual void resize(WindowId id, Size size) = 0;
    virtual void detach(WindowId id) = 0;
    [[nodiscard]] virtual SkCanvas* canvas(WindowId id) = 0;
    virtual void present(WindowId id, void* native_handle) = 0;
    [[nodiscard]] virtual RenderInfo info(WindowId id) const = 0;

    // Optional capture support. Defaults report unsupported so third-party
    // renderers stay source-compatible. capture_frame copies the window's
    // most recently presented frame synchronously on the UI thread; false
    // means "cannot capture now" (hidden window without a frame, device
    // lost, or backend without support) and callers must not fabricate
    // pixels. Implementations must not hand out the live canvas or a GPU
    // resource.
    [[nodiscard]] virtual bool supports_capture() const { return false; }
    virtual bool capture_frame(WindowId id, CapturedFrame& out) {
        (void)id;
        (void)out;
        return false;
    }
};

// Windows reference renderer. All artwork is drawn by Skia; this backend
// copies Skia's raster pixels into a Win32 window for presentation.
[[nodiscard]] std::unique_ptr<IRenderer> make_skia_raster_renderer();

// Windows host: Skia Ganesh/D3D12 flip swapchain when available, with a
// Skia raster fallback for machines where GPU initialization fails.
[[nodiscard]] std::unique_ptr<IRenderer> make_skia_windows_renderer();

} // namespace tabengine
