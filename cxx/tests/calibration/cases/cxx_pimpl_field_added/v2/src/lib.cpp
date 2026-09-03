#include "lib.h"
class Impl {
public:
  int a;
  int b;
};
Handle::Handle() : p(new Impl()) {
  p->a = 1;
  p->b = 2;
}
Handle::~Handle() { delete p; }
int Handle::get() const { return p->a + p->b; }
