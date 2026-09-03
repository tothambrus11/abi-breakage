#include "lib.h"
BaseNew::~BaseNew() {}
Derived::~Derived() {}
Derived *make_d() { return new Derived(); }
