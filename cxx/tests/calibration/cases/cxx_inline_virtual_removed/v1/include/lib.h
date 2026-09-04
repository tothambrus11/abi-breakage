#ifndef LIB_H
#define LIB_H
class Widget {
public:
  virtual ~Widget() {}
  virtual int area() const { return 1; }
  virtual int perimeter() const { return 2; }
};
Widget *make_widget();
#endif
