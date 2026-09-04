#ifndef LIB_H
#define LIB_H
class Widget {
public:
  virtual ~Widget() {}
  virtual int area() const { return 1; }
};
Widget *make_widget();
#endif
