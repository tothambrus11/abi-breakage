#ifndef LIB_H
#define LIB_H
class Box { public: int w; int h; virtual int area() const; virtual ~Box(); };
Box *make_box();
#endif
