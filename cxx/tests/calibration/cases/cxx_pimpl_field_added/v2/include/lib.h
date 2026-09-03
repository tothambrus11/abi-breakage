#ifndef LIB_H
#define LIB_H
class Impl;
class Handle {
  Impl *p;
public:
  Handle();
  ~Handle();
  int get() const;
};
#endif
