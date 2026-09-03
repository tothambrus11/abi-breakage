#ifndef LIB_H
#define LIB_H
class BaseNew { public: int b; virtual ~BaseNew(); };
class Derived : public BaseNew { public: int a; virtual ~Derived(); };
Derived *make_d();
#endif
