#ifndef LIB_H
#define LIB_H
class Vec {
  int x, y;
public:
  Vec(int a, int b) : x(a), y(b) {}
  int norm1() const { int s = 0; s += (x<0?-x:x); s += (y<0?-y:y); return s + 1; }
};
int use_vec(int a, int b);
#endif
