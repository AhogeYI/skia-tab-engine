#pragma once

#include <cstdint>

namespace tabengine {

using WindowId = std::uint64_t;
using TabId = std::uint64_t;
using ContentId = std::uint64_t;

struct Point {
    int x = 0;
    int y = 0;
};

struct Size {
    int width = 0;
    int height = 0;
};

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] int right() const { return x + width; }
    [[nodiscard]] int bottom() const { return y + height; }
    [[nodiscard]] bool contains(Point p) const {
        return p.x >= x && p.x < right() && p.y >= y && p.y < bottom();
    }
};

} // namespace tabengine

