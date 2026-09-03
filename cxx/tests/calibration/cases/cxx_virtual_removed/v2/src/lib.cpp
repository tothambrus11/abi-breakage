#include "lib.h"
Shape::~Shape() {}
int Shape::area() const { return 1; }
Shape *make_shape() { return new Shape(); }
