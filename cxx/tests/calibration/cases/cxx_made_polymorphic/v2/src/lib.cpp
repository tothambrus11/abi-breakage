#include "lib.h"
int Box::area() const { return w * h; }
Box::~Box() {}
Box *make_box() { return new Box(); }
