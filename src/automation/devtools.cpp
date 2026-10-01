#include "tabengine/automation/devtools.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <utility>
#include <vector>

namespace tabengine::automation {
namespace {

// The panel ships as a fixed set of hand-written files; serving anything
// else from the root (a stray README, a build script) is a bug, not a
// feature. Content types are explicit so nosniff never has to guess.
bool allowed_asset(const std::string& name, const char** content_type) {
    static const struct {
        const char* extension;
        const char* type;
    } kTypes[] = {
        {".html", "text/html; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".svg", "image/svg+xml"},
        {".png", "image/png"},
        {".ico", "image/x-icon"},
    };
    for (const auto& entry : kTypes) {
        const std::size_t length = std::strlen(entry.extension);
        if (name.size() > length &&
            name.compare(name.size() - length, length, entry.extension) == 0) {
            *content_type = entry.type;
            return true;
        }
    }
    return false;
}

bool safe_segment(const std::string& name) {
    // Exactly one path segment: no separators, no traversal, no dotfiles.
    if (name.empty() || name.size() > 96) return false;
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        return false;
    }
    if (name == "." || name == ".." || name.front() == '.') return false;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) return false;
    }
    return name.find("..") == std::string::npos;
}

bool read_file_bytes(const std::filesystem::path& path, std::string& bytes) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    // Hard budget enforced DURING the read: a file that grows between the
    // size check and the read can never make this loop unbounded.
    std::vector<char> buffer(8192);
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::size_t got = static_cast<std::size_t>(file.gcount());
        if (got == 0) break;
        if (bytes.size() + got > DevtoolsPanel::kMaxFileBytes) {
            return false; // over budget, whatever the metadata claimed
        }
        bytes.append(buffer.data(), got);
    }
    return file.eof() || file.good();
}

} // namespace

bool DevtoolsPanel::mount(AutomationTransport& transport, const Options& options) {
    std::filesystem::path root = options.resources_root;
    std::string panel_file;
    if (!read_file_bytes(root / "panel.html", panel_file)) {
        return false;
    }
    const std::string entry = options.mount_path;
    const std::string assets_prefix = entry + "/assets/";

    // Panel sources reference assets through a {{MOUNT}} placeholder: the
    // shell substitutes the actual mount path when serving. The page URL
    // has no trailing slash, so plain relative URLs would resolve against
    // the parent directory and miss the assets route entirely; the
    // placeholder keeps every mount path working without hardcoding one.
    const auto substitute = [entry](std::string body) {
        const std::string placeholder = "{{MOUNT}}";
        std::size_t at = body.find(placeholder);
        while (at != std::string::npos) {
            body.replace(at, placeholder.size(), entry);
            at = body.find(placeholder, at + entry.size());
        }
        return body;
    };

    // Keep the entry document alive for the handler's lifetime; asset files
    // are read per request so a dev refresh of the CSS/JS lands without a
    // process restart.
    auto entry_document = std::make_shared<std::string>(substitute(std::move(panel_file)));
    auto assets_root = std::make_shared<std::filesystem::path>(std::move(root));

    transport.set_public_file_handler(
        [entry, assets_prefix, entry_document, assets_root, substitute](
            const std::string& path, AutomationTransport::RouteReply& reply) -> bool {
            if (path != entry && path.rfind(assets_prefix, 0) != 0) {
                return false; // not ours: normal auth processing continues
            }
            if (path == entry) {
                reply.status = 200;
                reply.content_type = "text/html; charset=utf-8";
                reply.body = *entry_document;
                reply.cache_control = "no-store";
                return true;
            }
            const std::string name = path.substr(assets_prefix.size());
            const char* content_type = nullptr;
            if (!safe_segment(name) || !allowed_asset(name, &content_type)) {
                reply.status = 404;
                reply.content_type = "application/json";
                reply.body = R"({"error":"unknown panel asset"})";
                reply.cache_control = "no-store";
                return true;
            }
            std::string bytes;
            if (!read_file_bytes(*assets_root / name, bytes)) {
                reply.status = 404;
                reply.content_type = "application/json";
                reply.body = R"({"error":"panel asset missing"})";
                reply.cache_control = "no-store";
                return true;
            }
            // The JS module imports its sibling core through the same
            // placeholder; text assets get the mount substituted so the
            // served page works under any mount path.
            if (name == "panel.js" || name == "panel_core.js" ||
                name == "panel.css") {
                bytes = substitute(std::move(bytes));
            }
            reply.status = 200;
            reply.content_type = content_type;
            reply.body = std::move(bytes);
            reply.cache_control = "no-store";
            return true;
        });
    return true;
}

} // namespace tabengine::automation
