// templates.cpp — template-heavy fixture: variadic templates, partial and full
// specialisation, CRTP, a type trait, and an alias template. Probes how the
// bridge indexes templated entities (symbols, hover, goto, semantic tokens).
// Warning-clean under -std=c++17.

/// Primary template: reports the size of a type in a static member.
template <typename T>
struct TypeSize {
    static constexpr int value = sizeof(T);
};

/// Full specialisation for void (size 0).
template <>
struct TypeSize<void> {
    static constexpr int value = 0;
};

/// Partial specialisation for pointers (size of a pointer).
template <typename T>
struct TypeSize<T*> {
    static constexpr int value = sizeof(T*);
};

/// Variadic: count the number of template arguments.
template <typename... Ts>
struct Count {
    static constexpr int value = sizeof...(Ts);
};

/// Alias template.
template <typename T>
using Ptr = T*;

/// CRTP base providing a polymorphic-by-static-dispatch helper.
template <typename Derived>
struct Shape {
    double scaled_area(double k) const {
        return k * static_cast<const Derived*>(this)->area();
    }
};

/// CRTP derived: a unit square.
struct Square : Shape<Square> {
    double side;
    explicit Square(double s) : side(s) {}
    double area() const { return side * side; }
};

/// Function template with a non-type parameter.
template <int N, typename T>
T repeat_add(T base) {
    T acc = base;
    for (int i = 1; i < N; ++i) acc = acc + base;
    return acc;
}

int main() {
    int s1 = TypeSize<int>::value;
    int s2 = TypeSize<void>::value;
    int s3 = TypeSize<char*>::value;
    int n = Count<int, char, double>::value;
    Ptr<int> pi = nullptr;
    Square sq(3.0);
    double sc = sq.scaled_area(2.0);
    int r = repeat_add<4>(5);
    return s1 + s2 + s3 + n + (pi ? 1 : 0) + (int)sc + r;
}
