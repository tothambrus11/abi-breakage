#include "lib.h"
Counter::Counter() : n(0), step(1) {}
void Counter::bump() { n += step; }
int Counter::value() const { return n; }
