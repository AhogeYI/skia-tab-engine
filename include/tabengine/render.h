#pragma once

#include "tabengine/types.h"

#include <memory>

class SkCanvas;

namespace tabengine {

class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual bool attach(WindowId id, void* native_handle, Size size) = 0;
    virtual void resize(WindowId id, Size size) = 0;
    virtual void detach(WindowId id) = 0;
    [[nodiscard]] virtual SkCanvas* canvas(WindowId id) = 0;
    virtual void present(WindowId id, void* native_handle) = 0;
};

// Windows reference renderer. All artwork is drawn by Skia; this backend
// copies Skia's raster pixels into a Win32 window for presentation.
[[nodiscard]] std::unique_ptr<IRenderer> make_skia_raster_renderer();

} // namespace tabengine

