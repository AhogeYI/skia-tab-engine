#include "tabengine/render.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>
#include <memory>
#include <unordered_map>

namespace tabengine {
namespace {

class RasterRenderer final : public IRenderer {
public:
    bool attach(WindowId id, void*, Size size) override {
        resize(id, size);
        return surfaces_.contains(id);
    }

    void resize(WindowId id, Size size) override {
        if (size.width <= 0 || size.height <= 0) return;
        auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(size.width, size.height));
        if (surface) surfaces_[id] = std::move(surface);
    }

    void detach(WindowId id) override { surfaces_.erase(id); }

    SkCanvas* canvas(WindowId id) override {
        auto it = surfaces_.find(id);
        return it == surfaces_.end() ? nullptr : it->second->getCanvas();
    }

    void present(WindowId id, void* native_handle) override {
        auto it = surfaces_.find(id);
        if (it == surfaces_.end() || !native_handle) return;
        SkPixmap pixels;
        if (!it->second->peekPixels(&pixels)) return;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = pixels.width();
        info.bmiHeader.biHeight = -pixels.height();
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        const HWND hwnd = static_cast<HWND>(native_handle);
        HDC dc = GetDC(hwnd);
        if (!dc) return;
        StretchDIBits(dc, 0, 0, pixels.width(), pixels.height(), 0, 0,
                      pixels.width(), pixels.height(), pixels.addr(), &info,
                      DIB_RGB_COLORS, SRCCOPY);
        ReleaseDC(hwnd, dc);
    }

    RenderInfo info(WindowId id) const override {
        auto it = surfaces_.find(id);
        if (it == surfaces_.end()) return {};
        return {RenderBackend::Raster, {it->second->width(), it->second->height()}};
    }

    bool supports_capture() const override { return true; }

    bool capture_frame(WindowId id, CapturedFrame& out) override {
        auto it = surfaces_.find(id);
        if (it == surfaces_.end()) return false;
        SkPixmap pixels;
        if (!it->second->peekPixels(&pixels)) return false;
        out.backend = RenderBackend::Raster;
        out.size = {pixels.width(), pixels.height()};
        const std::size_t row_bytes = static_cast<std::size_t>(pixels.width()) * 4;
        out.pixels.resize(row_bytes * static_cast<std::size_t>(pixels.height()));
        // The raster surface is plain CPU memory holding the last drawn
        // frame; a row-wise copy keeps the source untouched.
        for (int y = 0; y < pixels.height(); ++y) {
            std::memcpy(out.pixels.data() + static_cast<std::size_t>(y) * row_bytes,
                        pixels.addr32(0, y), row_bytes);
        }
        return true;
    }

private:
    std::unordered_map<WindowId, sk_sp<SkSurface>> surfaces_;
};

} // namespace

std::unique_ptr<IRenderer> make_skia_raster_renderer() {
    return std::make_unique<RasterRenderer>();
}

} // namespace tabengine
