#include "demos/demo_support.h"

#include "backends/opengl/opengl_device.h"
#include "core/containers/array.h"
#include "core/containers/string.h"
#include "core/diagnostics/diagnostics.h"
#include "graphics/hud/hud_overlay.h"
#include "platform/gl_context.h"

namespace demo {

namespace platform = f4::platform;

using f4::gfx::GfxError;
using f4::gfx::CommandBuffer;
using f4::gfx::IDevice;
using f4::gfx::createOpenGLDevice;
using f4::gfx::hud::HudLine;
using f4::gfx::hud::Overlay;
using f4::diag::MetricRegistry;
using f4::diag::Metric;
using f4::diag::formatMetric;
using f4::containers::Array;
using f4::containers::String;

DemoWindow::DemoWindow(const char* title, int width, int height, bool resizable) {
    auto w = platform::Window::create(
        {title, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), resizable});
    if (!w.hasValue()) {
        return;
    }
    window_ = w.value();
    if (!window_->makeCurrent()) {
        return;
    }
    if (!platform::initGl(4, 3).hasValue()) {
        return;
    }
    device_ = createOpenGLDevice();
    if (device_ == nullptr) {
        return;
    }
    ok_ = true;
}

DemoWindow::~DemoWindow() {
    if (device_ != nullptr) {
        device_->waitIdle();
        deleteDevice(device_);
        device_ = nullptr;
    }
    delete window_;
    window_ = nullptr;
}

bool Hud::init(std::uint32_t maxGlyphs, float scale) {
    return overlay_.init(maxGlyphs, scale) == GfxError::None;
}

void Hud::onSample(const f4::diag::MetricRegistry& metrics, void* userData) {
    if (userData == nullptr) {
        return;
    }
    auto* self = static_cast<Hud*>(userData);
    self->rebuild(metrics);
}

void Hud::rebuild(const f4::diag::MetricRegistry& metrics) {
    const std::size_t n = metrics.count();
    texts_.resize(n);
    lines_.resize(n);
    std::size_t out = 0;

    // Group metrics by category
    struct CatLines {
        f4::containers::Array<f4::containers::String> texts;
        f4::containers::Array<f4::gfx::hud::HudLine> lines;
    };

    // Order: Frame, GPU, Memory, Telemetry
    struct Category {
        const char* name;
        const char* prefix;
    };
    constexpr Category cats[] = {
        {"frame", "FRAME"},
        {"gpu", "GPU"},
        {"memory", "MEMORY"},
        {"telemetry", "TELEMETRY"},
        {"profiler", "PROFILER"},
    };

    CatLines catLines[5];

    for (const f4::diag::Metric& m : metrics) {
        int catIdx = -1;
        const char* cat = m.decl().category;
        for (int i = 0; i < 5; ++i) {
            if (cat && strcmp(cat, cats[i].name) == 0) {
                catIdx = i;
                break;
            }
        }
        if (catIdx < 0) catIdx = 0;

        f4::containers::String text;
        const char* category = m.decl().category;
        if (category != nullptr) {
            text.append(cats[catIdx].prefix);
            text.append(": ");
        }
        text.append(f4::diag::formatMetric(m).c_str());
        catLines[catIdx].texts.push(std::move(text));
    }

    // Build lines in category order
    out = 0;
    for (int ci = 0; ci < 5; ++ci) {
        if (catLines[ci].texts.isEmpty()) continue;
        for (std::size_t i = 0; i < catLines[ci].texts.size(); ++i) {
            texts_[out] = catLines[ci].texts[i].c_str();
            lines_[out].x = 12.0f;
            lines_[out].y = 12.0f + static_cast<float>(out) * 14.0f;
            lines_[out].text = texts_[out].c_str();
            lines_[out].r = 0.9f;
            lines_[out].g = 0.95f;
            lines_[out].b = 1.0f;
            lines_[out].a = 1.0f;
            ++out;
        }
    }
    if (out != lines_.size()) {
        lines_.resize(out);
        texts_.resize(out);
    }
    overlay_.setLines(std::span<const f4::gfx::hud::HudLine>(lines_.data(), lines_.size()));
}

void Hud::render(f4::gfx::CommandBuffer& cb, std::uint32_t width, std::uint32_t height) {
    overlay_.render(cb, width, height);
}

} // namespace demo