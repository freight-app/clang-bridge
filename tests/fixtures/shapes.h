// shapes.h — auxiliary header included by test.cpp.
//
// Exists so the fixture exercises the #include graph (cb_inclusions) and
// cross-file go-to-definition (a call in test.cpp resolving here).
#pragma once

/// Square an integer.  Defined in the header, called from test.cpp's main().
inline int square(int n) {
    return n * n;
}
