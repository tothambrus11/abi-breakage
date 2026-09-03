#include "lib.h"
Counter::Counter() : n(0) {}
void Counter::bump(int by) { n += by; }
int Counter::value() const { return n; }
