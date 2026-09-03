#include "lib.h"
int use_clamp(int v) { return clampv<int>(v, 0, 10); }
