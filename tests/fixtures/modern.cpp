// modern.cpp — exercises modern C++ (C++20) constructs so the bridge's handling
// of newer syntax can be probed: concepts, structured bindings, lambdas with
// captures, `if constexpr`, fold expressions, trailing return types, and an
// enum class. Kept warning-clean under -std=c++20.

#include <utility>

/// A concept: any type supporting a + b.
template <typename T>
concept Addable = requires(T a, T b) {
    a + b;
};

/// Sum a parameter pack with a fold expression.
template <typename... Ts>
auto sum(Ts... xs) -> decltype((xs + ...)) {
    return (xs + ...);
}

/// Constrained function template using the concept.
template <Addable T>
T twice(T value) {
    return value + value;
}

/// A small pair-like aggregate for structured-binding tests.
struct Vec2 {
    double x;
    double y;
};

/// Compile-time branch with `if constexpr`.
template <typename T>
T abs_generic(T v) {
    if constexpr (sizeof(T) >= 4) {
        return v < 0 ? -v : v;
    } else {
        return v;
    }
}

enum class Mode { Fast, Slow };

/// Entry point exercising the helpers above.
int main() {
    Vec2 p{3.0, 4.0};
    auto [px, py] = p;                 // structured binding

    int captured = 10;
    auto adder = [captured](int n) { return n + captured; };

    int total = sum(1, 2, 3, 4);       // fold-expression call
    int doubled = twice(total);        // concept-constrained call
    int a = abs_generic(-7);           // if constexpr call
    int b = adder(5);
    double m = abs_generic(2.5);
    Mode mode = Mode::Fast;

    return total + doubled + a + b + (int)px + (int)py + (int)m + (int)mode;
}
