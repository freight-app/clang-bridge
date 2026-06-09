// test.cpp — comprehensive fixture exercising the clang-bridge LSP API surface.
//
// Every construct here is referenced by tests/fixture_api.rs.  The test locates
// positions by searching this source text for anchor substrings rather than by
// hard-coding line numbers, so this file may be edited freely as long as the
// anchored substrings the test relies on remain present and unique.
//
// The file is deliberately warning-clean under `-std=c++17` so the semantic
// APIs (hover, goto, references, hierarchies) all resolve correctly.

#include "shapes.h"

// ── Macros ──────────────────────────────────────────────────────────────────
#define MAX_ITEMS 128
#define SQUARE(x) ((x) * (x))

namespace geo {

/// A 2D point with integer coordinates.
struct Point {
    int x;  ///< horizontal coordinate
    int y;  ///< vertical coordinate

    /// Manhattan distance from the origin.
    int norm() const {
        return (x < 0 ? -x : x) + (y < 0 ? -y : y);
    }
};

/// Abstract base for every drawable shape.
struct Shape {
    virtual ~Shape() = default;
    /// Area of the shape, in square units.
    virtual double area() const = 0;
};

/// A circle, parameterised by its radius.
struct Circle : Shape {
    double radius;
    explicit Circle(double r) : radius(r) {}
    double area() const override {
        return 3.14159265 * radius * radius;
    }
};

/// An axis-aligned rectangle.
struct Rectangle : Shape {
    double width;
    double height;
    Rectangle(double w, double h) : width(w), height(h) {}
    double area() const override {
        return width * height;
    }
};

}  // namespace geo

/// Selects a single colour channel.
enum class Channel {
    Red,
    Green,
    Blue,
};

/// Add two integers.
///
/// @param a the first addend
/// @param b the second addend
/// @returns the sum of a and b
int add(int a, int b) {
    return a + b;
}

/// Add two doubles (overload of add).
double add(double a, double b) {
    return a + b;
}

/// Clamp value into the inclusive range [lo, hi].
template <typename T>
T clamp(T value, T lo, T hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

/// Sum every integer from 1 to n by repeatedly calling add().
int sum_to(int n) {
    int total = 0;
    for (int i = 1; i <= n; ++i) {
        total = add(total, i);
    }
    return total;
}

/// Program entry point — exercises the helpers above.
int main() {
    auto answer = add(40, 2);
    auto doubled = square(answer);
    geo::Circle circle(2.0);
    double a = circle.area();
    int capped = clamp(answer, 0, MAX_ITEMS);
    int s = sum_to(10);
    int sq = SQUARE(s);
    return answer + doubled + capped + s + sq + (int)a;
}
