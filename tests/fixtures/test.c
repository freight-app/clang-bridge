/* test.c — C-language fixture for the clang-bridge API audit.
 *
 * Deliberately C, not C++: no namespaces, classes, templates, overloads, or
 * references. Exercises the constructs an LSP must handle in a .c file —
 * structs, typedefs, enums, function pointers, macros, and static/extern
 * linkage — so the C path can be asserted independently of test.cpp.
 *
 * Positions in tests should be derived by searching this source, never
 * hard-coded, so edits here don't silently break the assertions.
 */

#include <stddef.h>

/* Object-like macro. */
#define MAX_POINTS 16

/* Function-like macro: area of an axis-aligned box. */
#define BOX_AREA(w, h) ((w) * (h))

/* A 2-D integer point. */
typedef struct Point {
    int x; /*!< horizontal coordinate */
    int y; /*!< vertical coordinate */
} Point;

/* Cardinal directions. */
enum Direction {
    DIR_NORTH,
    DIR_EAST,
    DIR_SOUTH,
    DIR_WEST,
};

/* A callback that maps one int to another. */
typedef int (*IntMapFn)(int);

/* File-scope mutable state (external linkage). */
int point_count = 0;

/* Internal helper (no external linkage). */
static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/*! Manhattan distance between two points. */
int manhattan(Point a, Point b) {
    int dx = a.x - b.x;
    int dy = a.y - b.y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx + dy;
}

/* Apply fn to v, clamping the result into [0, MAX_POINTS]. */
int apply_clamped(IntMapFn fn, int v) {
    return clampi(fn(v), 0, MAX_POINTS);
}

int double_it(int n) { return n * 2; }

int main(void) {
    Point origin = { 0, 0 };
    Point p = { 3, 4 };
    point_count = 2;

    int d = manhattan(origin, p);
    int area = BOX_AREA(p.x, p.y);
    int mapped = apply_clamped(double_it, d);
    enum Direction heading = DIR_EAST;

    return d + area + mapped + (int)heading;
}
