#include "lib.h"
int Box::area() const { return w * h; }
Box *make_box() { return new Box(); }
