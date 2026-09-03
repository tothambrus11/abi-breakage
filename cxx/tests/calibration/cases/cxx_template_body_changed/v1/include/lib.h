#ifndef LIB_H
#define LIB_H
template <class T> T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
int use_clamp(int v);
#endif
