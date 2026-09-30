// Shell bridge Capture and Trace domains. Capture copies an independent
// pixel buffer from the renderer at a safe frame boundary, crops it,
// encodes PNG and stores a bounded artifact; the frame is never faked and
// a D3D12 window is never answered with raster pixels. Trace records
// bounded spans around input dispatch, presented frames, capture and wait
// work while a trace session is active.

#include "shell_bridge_impl.h"

#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/encode/SkPngEncoder.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace tabengine::automation {
namespace {

JsonValue object_schema(std::vector<std::pair<std::string, JsonValue>> properties,
                        std::vector<std::string> required = {}) {
    JsonValue schema = JsonValue::object();
    schema.set("type", "object");
    JsonValue props = JsonValue::object();
    for (auto& [name, property] : properties) props.set(std::move(name), std::move(property));
    schema.set("properties", std::move(props));
    if (!required.empty()) {
        JsonValue list = JsonValue::array();
        for (std::string& name : required) list.push_back(JsonValue(std::move(name)));
        schema.set("required", std::move(list));
    }
    return schema;
}

JsonValue string_property(std::string description) {
    JsonValue property = JsonValue::object();
    property.set("type", "string");
    property.set("description", std::move(description));
    return property;
}

JsonValue integer_property(std::string description, std::int64_t minimum = -1,
                           std::int64_t maximum = -1) {
    JsonValue property = JsonValue::object();
    property.set("type", "integer");
    property.set("description", std::move(description));
    if (minimum >= 0) property.set("minimum", minimum);
    if (maximum >= 0) property.set("maximum", maximum);
    return property;
}

JsonValue number_property(std::string description) {
    JsonValue property = JsonValue::object();
    property.set("type", "number");
    property.set("description", std::move(description));
    return property;
}

JsonValue boolean_property(std::string description) {
    JsonValue property = JsonValue::object();
    property.set("type", "boolean");
    property.set("description", std::move(description));
    return property;
}

JsonValue enum_property(std::string description, std::vector<std::string> values) {
    JsonValue property = JsonValue::object();
    property.set("type", "string");
    property.set("description", std::move(description));
    JsonValue list = JsonValue::array();
    for (std::string& value : values) list.push_back(JsonValue(std::move(value)));
    property.set("enum", std::move(list));
    return property;
}

const JsonValue kObjectSchema = [] {
    JsonValue schema = JsonValue::object();
    schema.set("type", "object");
    schema.set("properties", JsonValue::object());
    return schema;
}();

const char* backend_name(RenderBackend backend) {
    switch (backend) {
    case RenderBackend::Raster: return "raster";
    case RenderBackend::D3D12: return "d3d12";
    default: return "none";
    }
}

// Copies `region` (clamped to the frame) out of a full-frame pixel buffer.
bool crop_bgra(const CapturedFrame& frame, Rect region, std::vector<std::uint8_t>& out,
               Size& out_size) {
    Rect clamped{
        std::max(0, region.x),
        std::max(0, region.y),
        std::min(region.width, frame.size.width - std::max(0, region.x)),
        std::min(region.height, frame.size.height - std::max(0, region.y)),
    };
    if (clamped.width <= 0 || clamped.height <= 0) return false;
    const std::size_t source_stride = static_cast<std::size_t>(frame.size.width) * 4;
    const std::size_t target_stride = static_cast<std::size_t>(clamped.width) * 4;
    out.resize(target_stride * static_cast<std::size_t>(clamped.height));
    for (int y = 0; y < clamped.height; ++y) {
        const std::uint8_t* source = frame.pixels.data() +
                                     static_cast<std::size_t>(clamped.y + y) * source_stride +
                                     static_cast<std::size_t>(clamped.x) * 4;
        std::memcpy(out.data() + static_cast<std::size_t>(y) * target_stride, source,
                    target_stride);
    }
    out_size = {clamped.width, clamped.height};
    return true;
}

std::vector<std::uint8_t> encode_png(const std::vector<std::uint8_t>& bgra, Size size) {
    const SkImageInfo info = SkImageInfo::MakeN32Premul(size.width, size.height);
    const SkPixmap pixmap(info, bgra.data(), static_cast<std::size_t>(size.width) * 4);
    const sk_sp<SkData> png = SkPngEncoder::Encode(pixmap, SkPngEncoder::Options{});
    if (!png) return {};
    const auto* begin = static_cast<const std::uint8_t*>(png->data());
    return {begin, begin + png->size()};
}

Rect client_rect(IPlatform& platform, WindowId window) {
    const Size size = platform.client_size(window);
    return {0, 0, size.width, size.height};
}

} // namespace

void ShellAutomationBridge::Impl::trace_expiry_check() {
    if (trace_active && trace_has_deadline && runtime.now() >= trace_deadline) {
        trace_active = false;
    }
}

void ShellAutomationBridge::Impl::record_span(std::string name, std::string kind, double start,
                                              double end, std::string request_id,
                                              WindowId window, JsonValue details) {
    trace_expiry_check();
    if (!trace_active) return;
    if (trace_spans.size() >= trace_max_spans) {
        ++trace_dropped;
        return;
    }
    TraceSpan span;
    span.name = std::move(name);
    span.kind = std::move(kind);
    span.start = start;
    span.end = end;
    span.request_id = std::move(request_id);
    span.window = window;
    span.details = std::move(details);
    trace_spans.push_back(std::move(span));
}

void ShellAutomationBridge::Impl::frame_presented(WindowId window, std::uint64_t serial) {
    // Frame spans are only recorded while tracing: a per-frame event in the
    // ring would evict everything else at 60 fps.
    if (trace_active) {
        JsonValue details = JsonValue::object();
        details.set("frameSerial", static_cast<std::int64_t>(serial));
        const double now = runtime.now();
        record_span("frame.presented", "frame", now, now, {}, window, std::move(details));
    }
}

void ShellAutomationBridge::Impl::register_capture_and_trace_domains() {
    const std::string observe{std::string(kGrantObserve)};

    // ---- Capture.screenshot ----
    {
        std::vector<MethodDescriptor> methods;
        MethodDescriptor descriptor;
        descriptor.name = "Capture.screenshot";
        descriptor.summary =
            "Capture an independent pixel snapshot at a safe frame boundary, encode PNG and "
            "store it as an artifact. frame=fresh paints first; region/node restrict the "
            "area; the backend (raster or d3d12) is reported, never substituted.";
        descriptor.grants = {observe};
        descriptor.params = object_schema({
            {"windowId", string_property("Target window (decimal id string)")},
            {"frame", enum_property("Frame selection", {"current", "fresh"})},
            {"region",
             object_schema({
                 {"x", integer_property("Client x")},
                 {"y", integer_property("Client y")},
                 {"width", integer_property("Width", 1)},
                 {"height", integer_property("Height", 1)},
             })},
            {"node", string_property("Semantic node ref; captures its bounds")},
            {"snapshotId", integer_property("Snapshot layoutRevision for strict node use", 0)},
            {"strict", boolean_property("Fail when layoutRevision moved (default false)")},
        },
                                     {"windowId"});
        descriptor.result = kObjectSchema;
        methods.push_back(std::move(descriptor));

        MethodHandler handler = [this](const RequestContext& context, CompletionSink sink) {
            WindowId window = 0;
            const JsonValue* window_value = context.params.find("windowId");
            if (!window_value || !window_value->is_string() ||
                !id_from_wire(window_value->as_string(), window)) {
                sink(MethodOutcome::failure(
                    Error::invalid_params("params.windowId: not a decimal id string")));
                return;
            }
            if (!shell.model().window(window)) {
                sink(MethodOutcome::failure(
                    Error::target_not_found("window " + id_to_wire(window))));
                return;
            }
            if (!renderer.supports_capture()) {
                sink(MethodOutcome::failure(
                    Error::unsupported("this renderer cannot capture frames")));
                return;
            }
            const double started = runtime.now();
            std::string frame_mode = "current";
            if (const JsonValue* value = context.params.find("frame");
                value && value->is_string()) {
                frame_mode = value->as_string();
            }
            if (frame_mode != "current" && frame_mode != "fresh") {
                sink(MethodOutcome::failure(
                    Error::invalid_params("params.frame: expected current or fresh")));
                return;
            }
            // Region: explicit rect, or a semantic node's current bounds.
            Rect region = client_rect(platform, window);
            const JsonValue* node_value = context.params.find("node");
            if (node_value && node_value->is_string()) {
                std::uint64_t node = 0;
                if (!id_from_wire(node_value->as_string(), node)) {
                    sink(MethodOutcome::failure(
                        Error::invalid_params("params.node: not a decimal id string")));
                    return;
                }
                bool truncated = false;
                const SemanticSnapshot snapshot = build_snapshot(window, kMaxSnapshotNodes,
                                                                 truncated);
                const auto it = std::find_if(
                    snapshot.nodes.begin(), snapshot.nodes.end(),
                    [node](const SemanticNode& candidate) { return candidate.node == node; });
                if (it == snapshot.nodes.end()) {
                    sink(MethodOutcome::failure(Error::stale_node(
                        "node " + node_value->as_string() + " is not in the current tree")));
                    return;
                }
                if (const JsonValue* snapshot_value = context.params.find("snapshotId");
                    snapshot_value && snapshot_value->is_integer()) {
                    const bool strict =
                        context.params.find("strict") &&
                        context.params.find("strict")->is_boolean() &&
                        context.params.find("strict")->as_boolean();
                    if (strict && snapshot_value->as_integer() !=
                                      static_cast<std::int64_t>(snapshot.layout_revision)) {
                        sink(MethodOutcome::failure(Error::stale_node(
                            "layout changed since the snapshot was taken")));
                        return;
                    }
                }
                region = it->bounds;
            } else if (const JsonValue* region_value = context.params.find("region");
                       region_value && region_value->is_object()) {
                if (!rect_from_wire(*region_value, region) || region.width <= 0 ||
                    region.height <= 0) {
                    sink(MethodOutcome::failure(
                        Error::invalid_params("params.region: malformed rectangle")));
                    return;
                }
            }
            // Fresh frames are produced by a real paint through the normal
            // dispatch; the renderer then holds that frame.
            if (frame_mode == "fresh") {
                Event paint;
                paint.type = EventType::Paint;
                paint.window = window;
                shell.on_event(paint);
            }
            const Size client = platform.client_size(window);
            // The budget applies to what the caller asks us to produce (the
            // region); the full-frame copy the renderer makes is bounded by
            // the window surface the host already maintains. The client size
            // is still read so a zero-sized window fails capture below.
            const std::size_t max_pixels = runtime.budgets().screenshot_max_pixels;
            const std::size_t region_pixels =
                static_cast<std::size_t>(std::max(0, region.width)) *
                static_cast<std::size_t>(std::max(0, region.height));
            if (client.width <= 0 || client.height <= 0) {
                sink(MethodOutcome::failure(Error::make(
                    ErrorCode::SurfaceUnavailable, "the window has no client area")));
                return;
            }
            if (region_pixels > max_pixels) {
                sink(MethodOutcome::failure(Error::make(
                    ErrorCode::BudgetExceeded,
                    "capture exceeds the configured pixel budget")));
                return;
            }
            CapturedFrame frame;
            if (!renderer.capture_frame(window, frame) || !frame.valid()) {
                sink(MethodOutcome::failure(Error::make(
                    ErrorCode::SurfaceUnavailable,
                    "the renderer could not provide a capture for this window yet")));
                return;
            }
            std::vector<std::uint8_t> cropped;
            Size cropped_size;
            if (!crop_bgra(frame, region, cropped, cropped_size)) {
                sink(MethodOutcome::failure(Error::make(
                    ErrorCode::InvalidArgument, "the requested region is outside the frame")));
                return;
            }
            std::vector<std::uint8_t> png = encode_png(cropped, cropped_size);
            if (png.empty()) {
                sink(MethodOutcome::failure(
                    Error::make(ErrorCode::InternalError, "PNG encoding failed")));
                return;
            }
            JsonValue target = JsonValue::object();
            target.set("windowId", id_to_wire(window));
            target.set("bounds", rect_to_wire(region));
            target.set("backend", backend_name(frame.backend));
            target.set("frameSerial",
                       static_cast<std::int64_t>(shell.frame_serial(window)));
            const double now = runtime.now();
            const std::string artifact_id = runtime.artifacts().store(
                std::move(png), "image/png", std::move(target), now);
            if (artifact_id.empty()) {
                sink(MethodOutcome::failure(Error::make(
                    ErrorCode::BudgetExceeded,
                    "the artifact store rejected the image (byte budget)")));
                return;
            }
            record_span("Capture.screenshot", "capture", started, runtime.now(),
                        context.request_id, window);
            AutomationEvent event;
            event.name = std::string(event_names::kCaptureCompleted);
            event.window = window;
            event.request_id = context.request_id;
            JsonValue details = JsonValue::object();
            details.set("artifactId", artifact_id);
            details.set("backend", backend_name(frame.backend));
            details.set("width", static_cast<std::int64_t>(cropped_size.width));
            details.set("height", static_cast<std::int64_t>(cropped_size.height));
            event.details = std::move(details);
            runtime.emit_event(std::move(event));

            JsonValue result = JsonValue::object();
            result.set("artifactId", artifact_id);
            result.set("mediaType", "image/png");
            result.set("width", static_cast<std::int64_t>(cropped_size.width));
            result.set("height", static_cast<std::int64_t>(cropped_size.height));
            result.set("backend", backend_name(frame.backend));
            result.set("frameSerial",
                       static_cast<std::int64_t>(shell.frame_serial(window)));
            result.set("bounds", rect_to_wire(region));
            result.set("colorSpace", "sRGB");
            result.set("alpha", "premultiplied");
            sink(MethodOutcome::success(std::move(result)));
        };
        owned_domains.push_back(
            runtime.register_engine_methods("Capture", std::move(methods), std::move(handler)));
    }

    // ---- Trace domain ----
    {
        std::vector<MethodDescriptor> methods;
        MethodDescriptor start;
        start.name = "Trace.start";
        start.summary =
            "Begin recording bounded engine spans (input dispatch, presented frames, "
            "capture, wait evaluation). Restarts an active trace.";
        start.grants = {observe};
        start.mutating = true;
        start.params = object_schema({
            {"maxSpans", integer_property("Ring capacity (default 512)", 1, 8192)},
            {"durationS", number_property("Auto-stop after this many seconds")},
        });
        start.result = kObjectSchema;
        methods.push_back(std::move(start));

        MethodDescriptor stop;
        stop.name = "Trace.stop";
        stop.summary = "End the trace and return its spans.";
        stop.grants = {observe};
        stop.params = kObjectSchema;
        stop.result = kObjectSchema;
        methods.push_back(std::move(stop));

        MethodDescriptor status;
        status.name = "Trace.status";
        status.summary = "Trace activity, span count and dropped count.";
        status.grants = {observe};
        status.params = kObjectSchema;
        status.result = kObjectSchema;
        methods.push_back(std::move(status));

        MethodHandler handler = [this](const RequestContext& context, CompletionSink sink) {
            if (context.method == "Trace.start") {
                trace_spans.clear();
                trace_dropped = 0;
                trace_max_spans = 512;
                if (const JsonValue* value = context.params.find("maxSpans");
                    value && value->is_integer() && value->as_integer() > 0) {
                    trace_max_spans = static_cast<std::size_t>(
                        std::min<std::int64_t>(value->as_integer(), 8192));
                }
                trace_has_deadline = false;
                if (const JsonValue* value = context.params.find("durationS");
                    value && value->is_number() && value->as_number() > 0.0) {
                    trace_has_deadline = true;
                    trace_deadline = runtime.now() +
                                     std::min(value->as_number(),
                                              runtime.budgets().trace_duration_s);
                }
                trace_active = true;
                JsonValue result = JsonValue::object();
                result.set("active", true);
                result.set("maxSpans", static_cast<std::int64_t>(trace_max_spans));
                if (trace_has_deadline) result.set("durationS", trace_deadline - runtime.now());
                sink(MethodOutcome::success(std::move(result)));
                return;
            }
            if (context.method == "Trace.status") {
                trace_expiry_check();
                JsonValue result = JsonValue::object();
                result.set("active", trace_active);
                result.set("spans", static_cast<std::int64_t>(trace_spans.size()));
                result.set("dropped", static_cast<std::int64_t>(trace_dropped));
                sink(MethodOutcome::success(std::move(result)));
                return;
            }
            // Trace.stop
            JsonValue spans = JsonValue::array();
            for (const TraceSpan& span : trace_spans) {
                JsonValue wire = JsonValue::object();
                wire.set("name", span.name);
                wire.set("kind", span.kind);
                wire.set("start", span.start);
                wire.set("end", span.end);
                wire.set("durationS", span.end - span.start);
                if (!span.request_id.empty()) wire.set("requestId", span.request_id);
                if (span.window) wire.set("windowId", id_to_wire(span.window));
                if (!span.details.is_null()) wire.set("details", span.details);
                spans.push_back(std::move(wire));
            }
            JsonValue result = JsonValue::object();
            result.set("active", false);
            result.set("dropped", static_cast<std::int64_t>(trace_dropped));
            result.set("spans", std::move(spans));
            trace_active = false;
            trace_has_deadline = false;
            trace_spans.clear();
            trace_dropped = 0;
            sink(MethodOutcome::success(std::move(result)));
        };
        owned_domains.push_back(
            runtime.register_engine_methods("Trace", std::move(methods), std::move(handler)));
    }
}

} // namespace tabengine::automation
