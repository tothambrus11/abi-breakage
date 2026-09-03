#ifndef LIB_H
#define LIB_H
class Shape {
public:
  virtual ~Shape();
  virtual int area() const;
};
Shape *make_shape();
#endif
