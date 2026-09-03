#ifndef LIB_H
#define LIB_H
class Vec {
  int x, y;
public:
  Vec(int a, int b) : x(a), y(b) {}
  int norm1() const { return (x < 0 ? -x : x) + (y < 0 ? -y : y); }
};
int use_vec(int a, int b);
#endif
