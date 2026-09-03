#include "lib.h"
Counter::Counter() : n(0) {}
void Counter::bump() { n++; }
int Counter::value() const { return n; }
