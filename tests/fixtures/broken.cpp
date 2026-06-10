// broken.cpp — intentionally malformed, like a buffer mid-edit. Used to verify
// the bridge degrades gracefully: it must still return a translation unit, still
// report the errors as diagnostics, and still answer queries for the parts that
// did parse, without crashing.

struct Widget {
    int width;
    int height;

    int area() const {
        return width * height;   // valid: referenced by tests
    }
};

int compute(int n) {
    int total = 0;
    for (int i = 0; i < n; ++i) {
        total += i;
    }
    return total              // ERROR: missing semicolon
}

int use_unknown() {
    return mystery_value + 1;  // ERROR: undeclared identifier
}

int bad_assign() {
    int x = ;                  // ERROR: expected expression
    return x;
}

int main() {
    Widget w;
    w.                         // ERROR: incomplete member access
    return w.area();
}
