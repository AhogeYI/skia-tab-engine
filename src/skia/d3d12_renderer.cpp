#include "tabengine/render.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/GrTypes.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/d3d/GrD3DBackendContext.h"
#include "include/gpu/ganesh/d3d/GrD3DBackendSurface.h"
#include "include/gpu/ganesh/d3d/GrD3DDirectContext.h"
#include "include/gpu/ganesh/d3d/GrD3DTypes.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace tabengine {
namespace {

constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr unsigned kFrameCount = 2;

struct Frame {
    gr_cp<ID3D12Resource> resource;
    sk_sp<SkSurface> surface;
    std::uint64_t fence_value = 0;
};

struct WindowSurface {
    gr_cp<IDXGISwapChain3> swapchain;
    gr_cp<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    std::array<Frame, kFrameCount> frames;
    Size size{};
    unsigned index = 0;
    bool acquired = false;

    ~WindowSurface() {
        if (fence_event) CloseHandle(fence_event);
    }

    bool wait_for(std::uint64_t value) const {
        if (!value || fence->GetCompletedValue() >= value) return true;
        if (FAILED(fence->SetEventOnCompletion(value, fence_event))) return false;
        return WaitForSingleObject(fence_event, INFINITE) == WAIT_OBJECT_0;
    }
};

class D3D12Renderer final : public IRenderer {
public:
    bool initialize() {
        gr_cp<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        for (UINT i = 0;; ++i) {
            gr_cp<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(candidate->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                continue;
            if (SUCCEEDED(D3D12CreateDevice(candidate.get(), D3D_FEATURE_LEVEL_11_0,
                                            IID_PPV_ARGS(&backend_.fDevice)))) {
                backend_.fAdapter = candidate;
                break;
            }
        }
        if (!backend_.fDevice) return false;
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(backend_.fDevice->CreateCommandQueue(
                &queue_desc, IID_PPV_ARGS(&backend_.fQueue)))) return false;
        backend_.fProtectedContext = GrProtected::kNo;
        context_ = GrDirectContexts::MakeD3D(backend_);
        return context_ != nullptr;
    }

    ~D3D12Renderer() override {
        while (!windows_.empty()) detach(windows_.begin()->first);
        if (context_) context_->flushAndSubmit(GrSyncCpu::kYes);
        context_.reset();
    }

    bool attach(WindowId id, void* native_handle, Size size) override {
        if (!native_handle || size.width <= 0 || size.height <= 0 || windows_.contains(id))
            return false;
        gr_cp<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = static_cast<UINT>(size.width);
        desc.Height = static_cast<UINT>(size.height);
        desc.Format = kFormat;
        desc.BufferCount = kFrameCount;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.SampleDesc.Count = 1;
        gr_cp<IDXGISwapChain1> swapchain1;
        HWND hwnd = static_cast<HWND>(native_handle);
        if (FAILED(factory->CreateSwapChainForHwnd(backend_.fQueue.get(), hwnd, &desc,
                                                   nullptr, nullptr, &swapchain1))) return false;
        (void)factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        // Alt+Enter fullscreen is a window association the host never asked
        // for; this chrome owns every frame and input path, so opt out.
        auto window = std::make_unique<WindowSurface>();
        if (FAILED(swapchain1->QueryInterface(IID_PPV_ARGS(&window->swapchain)))) return false;
        if (FAILED(backend_.fDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                 IID_PPV_ARGS(&window->fence)))) return false;
        window->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!window->fence_event) return false;
        if (!wrap(*window, size)) return false;
        windows_.emplace(id, std::move(window));
        return true;
    }

    void resize(WindowId id, Size size) override {
        auto it = windows_.find(id);
        if (it == windows_.end() || size.width <= 0 || size.height <= 0) return;
        WindowSurface& window = *it->second;
        if (window.size.width == size.width && window.size.height == size.height) return;
        if (!release_frames(window)) {
            device_lost_ = true;
            return;
        }
        HRESULT hr = window.swapchain->ResizeBuffers(kFrameCount,
                static_cast<UINT>(size.width), static_cast<UINT>(size.height), kFormat, 0);
        if (FAILED(hr)) {
            // A failed resize usually means outstanding references to the old
            // buffers. Dropping Skia's GPU cache releases them; if the retry
            // also fails, treat the device as lost and let the host fall back.
            context_->freeGpuResources();
            hr = window.swapchain->ResizeBuffers(kFrameCount,
                    static_cast<UINT>(size.width), static_cast<UINT>(size.height), kFormat, 0);
        }
        if (FAILED(hr) || !wrap(window, size)) device_lost_ = true;
    }

    void detach(WindowId id) override {
        auto it = windows_.find(id);
        if (it == windows_.end()) return;
        (void)release_frames(*it->second);
        windows_.erase(it);
    }

    SkCanvas* canvas(WindowId id) override {
        auto it = windows_.find(id);
        if (it == windows_.end()) return nullptr;
        WindowSurface& window = *it->second;
        window.index = window.swapchain->GetCurrentBackBufferIndex();
        Frame& frame = window.frames[window.index];
        if (!frame.surface || !window.wait_for(frame.fence_value)) {
            device_lost_ = true;
            return nullptr;
        }
        window.acquired = true;
        return frame.surface->getCanvas();
    }

    void present(WindowId id, void*) override {
        auto it = windows_.find(id);
        if (it == windows_.end()) return;
        WindowSurface& window = *it->second;
        if (!window.acquired) return;
        Frame& frame = window.frames[window.index];
        GrFlushInfo flush_info{};
        context_->flush(frame.surface.get(), SkSurfaces::BackendSurfaceAccess::kPresent,
                        flush_info);
        context_->submit();
        const HRESULT presented = window.swapchain->Present(1, 0);
        const std::uint64_t fence_value = ++next_fence_;
        if (SUCCEEDED(backend_.fQueue->Signal(window.fence.get(), fence_value)))
            frame.fence_value = fence_value;
        else device_lost_ = true;
        window.acquired = false;
        if (FAILED(presented)) device_lost_ = true;
    }

    [[nodiscard]] bool device_lost() const { return device_lost_; }

    RenderInfo info(WindowId id) const override {
        auto it = windows_.find(id);
        if (it == windows_.end()) return {};
        return {RenderBackend::D3D12, it->second->size};
    }

private:
    bool wrap(WindowSurface& window, Size size) {
        GrD3DTextureResourceInfo resource_info(nullptr, nullptr, D3D12_RESOURCE_STATE_PRESENT,
                                               kFormat, 1, 1, 0);
        for (unsigned i = 0; i < kFrameCount; ++i) {
            Frame& frame = window.frames[i];
            if (FAILED(window.swapchain->GetBuffer(i, IID_PPV_ARGS(&frame.resource)))) return false;
            resource_info.fResource = frame.resource;
            auto target = GrBackendRenderTargets::MakeD3D(size.width, size.height, resource_info);
            frame.surface = SkSurfaces::WrapBackendRenderTarget(
                context_.get(), target, kTopLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType,
                sk_sp<SkColorSpace>(), nullptr);
            if (!frame.surface) return false;
            frame.fence_value = 0;
        }
        window.size = size;
        return true;
    }

    bool release_frames(WindowSurface& window) {
        // ResizeBuffers and swapchain teardown require every buffer reference
        // gone: flush Skia's queue, wait for each frame's fence so the GPU is
        // really done, then drop the surfaces before freeing cached resources.
        context_->flush();
        context_->submit(GrSyncCpu::kYes);
        window.acquired = false;
        bool waited = true;
        for (Frame& frame : window.frames) {
            waited = window.wait_for(frame.fence_value) && waited;
            frame.surface.reset();
            frame.resource.reset();
            frame.fence_value = 0;
        }
        context_->freeGpuResources();
        return waited;
    }

    GrD3DBackendContext backend_{};
    sk_sp<GrDirectContext> context_;
    std::unordered_map<WindowId, std::unique_ptr<WindowSurface>> windows_;
    std::uint64_t next_fence_ = 0;
    // Sticky: once set, every entry point fails fast and WindowsRenderer
    // reroutes the affected windows to the raster fallback permanently.
    bool device_lost_ = false;
};

// GPU-first wrapper: a window renders through D3D12 from attach until its
// first device-side failure, then falls back to raster once and stays there
// for its lifetime. Backend choice is per window and never auto-upgrades.
class WindowsRenderer final : public IRenderer {
public:
    WindowsRenderer() : gpu_(std::make_unique<D3D12Renderer>()),
                        raster_(make_skia_raster_renderer()) {
        if (!gpu_->initialize()) gpu_.reset();
    }

    bool attach(WindowId id, void* native_handle, Size size) override {
        if (gpu_ && !gpu_->device_lost() && gpu_->attach(id, native_handle, size)) {
            backends_[id] = {true, native_handle, size};
            return true;
        }
        if (!raster_->attach(id, native_handle, size)) return false;
        backends_[id] = {false, native_handle, size};
        return true;
    }

    void resize(WindowId id, Size size) override {
        if (auto it = backends_.find(id); it != backends_.end()) {
            it->second.size = size;
            if (it->second.gpu) {
                gpu_->resize(id, size);
                if (gpu_->device_lost()) (void)fall_back(id, it->second);
            }
            else raster_->resize(id, size);
        }
    }

    void detach(WindowId id) override {
        if (auto it = backends_.find(id); it != backends_.end()) {
            if (it->second.gpu) gpu_->detach(id);
            else raster_->detach(id);
            backends_.erase(it);
        }
    }

    SkCanvas* canvas(WindowId id) override {
        auto it = backends_.find(id);
        if (it == backends_.end()) return nullptr;
        SkCanvas* gpu_canvas = nullptr;
        if (it->second.gpu && !gpu_->device_lost()) gpu_canvas = gpu_->canvas(id);
        if (gpu_canvas) return gpu_canvas;
        if (it->second.gpu && gpu_->device_lost() && !fall_back(id, it->second))
            return nullptr;
        return it->second.gpu ? nullptr : raster_->canvas(id);
    }

    void present(WindowId id, void* native_handle) override {
        auto it = backends_.find(id);
        if (it == backends_.end()) return;
        if (it->second.gpu) {
            gpu_->present(id, native_handle);
            if (gpu_->device_lost())
                InvalidateRect(static_cast<HWND>(native_handle), nullptr, FALSE);
        }
        else raster_->present(id, native_handle);
    }

    RenderInfo info(WindowId id) const override {
        auto it = backends_.find(id);
        if (it == backends_.end()) return {};
        return it->second.gpu ? gpu_->info(id) : raster_->info(id);
    }

private:
    struct Backend {
        bool gpu = false;
        void* native_handle = nullptr;
        Size size{};
    };
    bool fall_back(WindowId id, Backend& backend) {
        gpu_->detach(id);
        if (!raster_->attach(id, backend.native_handle, backend.size)) return false;
        backend.gpu = false;
        return true;
    }
    std::unique_ptr<D3D12Renderer> gpu_;
    std::unique_ptr<IRenderer> raster_;
    std::unordered_map<WindowId, Backend> backends_;
};

} // namespace

std::unique_ptr<IRenderer> make_skia_windows_renderer() {
    return std::make_unique<WindowsRenderer>();
}

} // namespace tabengine
