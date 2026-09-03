#ifndef LIB_H
#define LIB_H
class Counter {
  int n;
  int step;
public:
  Counter();
  void bump();
  int value() const;
};
#endif
