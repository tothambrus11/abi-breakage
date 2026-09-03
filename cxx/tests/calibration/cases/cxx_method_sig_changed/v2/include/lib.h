#ifndef LIB_H
#define LIB_H
class Counter {
  int n;
public:
  Counter();
  void bump(int by);
  int value() const;
};
#endif
