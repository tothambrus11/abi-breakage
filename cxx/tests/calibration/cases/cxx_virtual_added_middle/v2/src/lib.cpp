#include "lib.h"
Shape::~Shape() {}
int Shape::centroid() const { return 3; }
int Shape::area() const { return 1; }
int Shape::perimeter() const { return 2; }
Shape *make_shape() { return new Shape(); }
