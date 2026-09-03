#ifndef LIB_H
#define LIB_H
class Shape {
public:
  virtual ~Shape();
  virtual int area() const;
  virtual int perimeter() const;
  virtual int centroid() const;
};
Shape *make_shape();
#endif
