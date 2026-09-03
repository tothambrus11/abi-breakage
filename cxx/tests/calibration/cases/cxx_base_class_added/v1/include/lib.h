#ifndef LIB_H
#define LIB_H
class Derived { public: int a; virtual ~Derived(); };
Derived *make_d();
#endif
