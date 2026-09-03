#include "lib.h"
class Impl {
public:
  int a;
};
Handle::Handle() : p(new Impl()) { p->a = 1; }
Handle::~Handle() { delete p; }
int Handle::get() const { return p->a; }
