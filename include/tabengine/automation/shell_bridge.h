#pragma once

// ShellAutomationBridge: binds one Shell (with its platform and renderer)
// to an AutomationRuntime. The bridge registers the engine domains
// (Window, Tab, Input, UI), converts shell observation into automation
// events, evaluates the wait environment hooks, and assembles the engine's
// part of every semantic snapshot. Application providers registered on the
// runtime contribute their own subtrees under the body node.
//
// The bridge owns none of the objects it binds. Create it after the Shell
// exists, destroy it before the Shell, and drive runtime.drain() from the
// UI thread (see UiTaskPump).

#include "tabengine/automation/runtime.h"
#include "tabengine/shell.h"

#include <memory>
#include <string>

namespace tabengine::automation {

class ShellAutomationBridge {
public:
    struct Dependencies {
        Shell& shell;
        IPlatform& platform;
        IRenderer& renderer;
        AutomationRuntime& runtime;
    };

    explicit ShellAutomationBridge(Dependencies dependencies);
    ~ShellAutomationBridge();
    ShellAutomationBridge(const ShellAutomationBridge&) = delete;
    ShellAutomationBridge& operator=(const ShellAutomationBridge&) = delete;

    // Wire label for the renderer backend reported in capabilities
    // ("raster", "d3d12", ...). Call once after binding; UI thread.
    void set_renderer_backend(std::string backend);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tabengine::automation
