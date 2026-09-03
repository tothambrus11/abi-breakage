#ifndef LIB_H
#define LIB_H
template <class T> T clampv(T v, T lo, T hi) { if (v < lo) return lo; if (v > hi) return hi; return v + T(1); }
int use_clamp(int v);
#endif
