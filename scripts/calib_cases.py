# -*- coding: utf-8 -*-
"""Ground-truth calibration corpus.

Each case is ONE minimal, isolated ABI change. We compile v1 and v2 as shared
libraries with debug info and record exactly what abidiff says, so the
classifier used on the real corpus is grounded in observed output, not guesses.

Every case declares:
  lang    : 'c' or 'cxx'
  truth   : the change kind we believe we injected (ground-truth label)
  breaks  : True if it is a real ABI break for existing clients
  v1, v2  : {relative path: file contents}
"""

# ---------------------------------------------------------------- C helpers

def c_case(hdr1, src1, hdr2, src2):
    return {"include/lib.h": hdr1, "src/lib.c": src1}, {"include/lib.h": hdr2, "src/lib.c": src2}

def cxx_case(hdr1, src1, hdr2, src2):
    return {"include/lib.h": hdr1, "src/lib.cpp": src1}, {"include/lib.h": hdr2, "src/lib.cpp": src2}


CASES = {}

def add(name, lang, truth, breaks, v1, v2, note=""):
    CASES[name] = dict(lang=lang, truth=truth, breaks=breaks, v1=v1, v2=v2, note=note)


# =================================================================== C: structs

_S1 = """#ifndef LIB_H
#define LIB_H
struct Point { int x; int y; };
int  pt_sum(struct Point *p);
void pt_init(struct Point *p);
#endif
"""
_SC = """#include "lib.h"
int  pt_sum(struct Point *p) { return p->x + p->y; }
void pt_init(struct Point *p) { p->x = 0; p->y = 0; }
"""

add("c_field_added_end", "c", "struct_field_added", True,
    *c_case(_S1, _SC,
            _S1.replace("int x; int y;", "int x; int y; int z;"),
            _SC.replace("p->x = 0; p->y = 0;", "p->x = 0; p->y = 0; p->z = 0;")),
    "field appended to a public struct -> size grows")

add("c_field_added_middle", "c", "struct_field_added_middle", True,
    *c_case(_S1, _SC,
            _S1.replace("int x; int y;", "int x; int w; int y;"),
            _SC.replace("p->x = 0; p->y = 0;", "p->x = 0; p->w = 0; p->y = 0;")),
    "field inserted mid-struct -> offsets of later fields shift")

add("c_field_type_changed", "c", "struct_field_type_changed", True,
    *c_case(_S1, _SC, _S1.replace("int y;", "long y;"), _SC),
    "public struct field changes type/width")

add("c_field_removed", "c", "struct_field_removed", True,
    *c_case(_S1.replace("int x; int y;", "int x; int y; int z;"),
            _SC,
            _S1, _SC),
    "field removed from public struct")

add("c_field_added_padding", "c", "struct_field_added_into_padding", True,
    *c_case("""#ifndef LIB_H
#define LIB_H
struct Pad { char a; /* 3 bytes padding */ int b; };
int pad_get(struct Pad *p);
#endif
""", """#include "lib.h"
int pad_get(struct Pad *p) { return p->a + p->b; }
""", """#ifndef LIB_H
#define LIB_H
struct Pad { char a; char c; int b; };
int pad_get(struct Pad *p);
#endif
""", """#include "lib.h"
int pad_get(struct Pad *p) { return p->a + p->c + p->b; }
"""),
    "field added into existing tail padding -> size UNCHANGED, still a break")

# =================================================================== C: enums

_E1 = """#ifndef LIB_H
#define LIB_H
enum Color { RED = 0, GREEN = 1 };
const char *color_name(enum Color c);
#endif
"""
_EC = """#include "lib.h"
const char *color_name(enum Color c) { return c == RED ? "red" : "green"; }
"""

add("c_enum_case_added", "c", "enum_case_added", True,
    *c_case(_E1, _EC, _E1.replace("GREEN = 1", "GREEN = 1, BLUE = 2"), _EC),
    "enumerator added, underlying type size unchanged")

add("c_enum_case_added_widening", "c", "enum_case_added_widening", True,
    *c_case(_E1, _EC,
            _E1.replace("GREEN = 1", "GREEN = 1, HUGE_C = 0x7FFFFFFFFFLL"), _EC),
    "enumerator added that forces a wider underlying type -> size change")

add("c_enum_case_removed", "c", "enum_case_removed", True,
    *c_case(_E1.replace("GREEN = 1", "GREEN = 1, BLUE = 2"), _EC, _E1, _EC),
    "enumerator removed")

# =================================================================== C: functions

_F1 = """#ifndef LIB_H
#define LIB_H
int  f_scale(int v, int factor);
void f_reset(void);
#endif
"""
_FC = """#include "lib.h"
int  f_scale(int v, int factor) { return v * factor; }
void f_reset(void) { }
"""

add("c_param_type_changed", "c", "function_param_type_changed", True,
    *c_case(_F1, _FC,
            _F1.replace("int  f_scale(int v, int factor);", "int  f_scale(int v, long factor);"),
            _FC.replace("int  f_scale(int v, int factor)", "int  f_scale(int v, long factor)")),
    "parameter type widened")

add("c_param_added", "c", "function_param_added", True,
    *c_case(_F1, _FC,
            _F1.replace("int  f_scale(int v, int factor);", "int  f_scale(int v, int factor, int bias);"),
            _FC.replace("int  f_scale(int v, int factor) { return v * factor; }",
                        "int  f_scale(int v, int factor, int bias) { return v * factor + bias; }")),
    "parameter appended to an exported C function")

add("c_return_type_changed", "c", "function_return_type_changed", True,
    *c_case(_F1, _FC,
            _F1.replace("int  f_scale(int v, int factor);", "long f_scale(int v, int factor);"),
            _FC.replace("int  f_scale(int v, int factor) {", "long f_scale(int v, int factor) {")),
    "return type changed")

add("c_func_added", "c", "function_added", False,
    *c_case(_F1, _FC,
            _F1.replace("void f_reset(void);", "void f_reset(void);\nint  f_new(int a);"),
            _FC.replace("void f_reset(void) { }", "void f_reset(void) { }\nint  f_new(int a) { return a; }")),
    "new exported function -> additive, not a break")

add("c_func_removed", "c", "function_removed", True,
    *c_case(_F1.replace("void f_reset(void);", "void f_reset(void);\nint  f_old(int a);"),
            _FC.replace("void f_reset(void) { }", "void f_reset(void) { }\nint  f_old(int a) { return a; }"),
            _F1, _FC),
    "exported function removed")

add("c_param_constness", "c", "function_param_qualifier_changed", False,
    *c_case("""#ifndef LIB_H
#define LIB_H
int cnt(char *s);
#endif
""", """#include "lib.h"
int cnt(char *s) { int n=0; while(*s++) n++; return n; }
""", """#ifndef LIB_H
#define LIB_H
int cnt(const char *s);
#endif
""", """#include "lib.h"
int cnt(const char *s) { int n=0; while(*s++) n++; return n; }
"""),
    "const added to a pointer param -> ABI-compatible in C, mangling-relevant in C++")

# ============================================ C: inline function body (the key case)

_I1 = """#ifndef LIB_H
#define LIB_H
static inline int fast_double(int v) { return v * 2; }
int lib_apply(int v);
#endif
"""
_IC = """#include "lib.h"
int lib_apply(int v) { return fast_double(v) + 1; }
"""

add("c_inline_body_changed", "c", "inline_body_changed", True,
    *c_case(_I1, _IC,
            _I1.replace("return v * 2;", "return v * 2 + 7;"),
            _IC),
    "body of a header inline fn changes; NO type or symbol change. "
    "Breaks any client that already inlined the old body.")

add("c_macro_value_changed", "c", "macro_value_changed", True,
    *c_case("""#ifndef LIB_H
#define LIB_H
#define LIB_BUFSZ 64
int lib_fill(char *b);
#endif
""", """#include "lib.h"
int lib_fill(char *b) { int i; for (i=0;i<LIB_BUFSZ;i++) b[i]=0; return LIB_BUFSZ; }
""", """#ifndef LIB_H
#define LIB_H
#define LIB_BUFSZ 128
int lib_fill(char *b);
#endif
""", """#include "lib.h"
int lib_fill(char *b) { int i; for (i=0;i<LIB_BUFSZ;i++) b[i]=0; return LIB_BUFSZ; }
"""),
    "public macro constant changes; invisible in DWARF types/symbols")

add("c_no_change", "c", "none", False, *c_case(_F1, _FC, _F1, _FC),
    "control: identical sources")

# =================================================================== C++: vtables

_V1H = """#ifndef LIB_H
#define LIB_H
class Shape {
public:
  virtual ~Shape();
  virtual int area() const;
  virtual int perimeter() const;
};
Shape *make_shape();
#endif
"""
_V1C = """#include "lib.h"
Shape::~Shape() {}
int Shape::area() const { return 1; }
int Shape::perimeter() const { return 2; }
Shape *make_shape() { return new Shape(); }
"""

add("cxx_virtual_added_end", "cxx", "vtable_virtual_added_end", True,
    *cxx_case(_V1H, _V1C,
              _V1H.replace("virtual int perimeter() const;",
                           "virtual int perimeter() const;\n  virtual int centroid() const;"),
              _V1C.replace("Shape *make_shape()",
                           "int Shape::centroid() const { return 3; }\nShape *make_shape()")),
    "virtual appended -> vtable grows; breaks subclasses, not plain callers")

add("cxx_virtual_added_middle", "cxx", "vtable_virtual_added_middle", True,
    *cxx_case(_V1H, _V1C,
              _V1H.replace("virtual int area() const;",
                           "virtual int centroid() const;\n  virtual int area() const;"),
              _V1C.replace("int Shape::area() const { return 1; }",
                           "int Shape::centroid() const { return 3; }\nint Shape::area() const { return 1; }")),
    "virtual inserted mid-vtable -> every later slot index shifts")

add("cxx_virtual_removed", "cxx", "vtable_virtual_removed", True,
    *cxx_case(_V1H, _V1C,
              _V1H.replace("  virtual int perimeter() const;\n", ""),
              _V1C.replace("int Shape::perimeter() const { return 2; }\n", "")),
    "virtual removed from vtable")

add("cxx_made_polymorphic", "cxx", "class_made_polymorphic", True,
    *cxx_case("""#ifndef LIB_H
#define LIB_H
class Box { public: int w; int h; int area() const; };
Box *make_box();
#endif
""", """#include "lib.h"
int Box::area() const { return w*h; }
Box *make_box() { return new Box(); }
""", """#ifndef LIB_H
#define LIB_H
class Box { public: int w; int h; virtual int area() const; virtual ~Box(); };
Box *make_box();
#endif
""", """#include "lib.h"
int Box::area() const { return w*h; }
Box::~Box() {}
Box *make_box() { return new Box(); }
"""),
    "non-polymorphic class gains a vptr -> layout shifts by a pointer")

add("cxx_base_class_added", "cxx", "base_class_added", True,
    *cxx_case("""#ifndef LIB_H
#define LIB_H
class Derived { public: int a; virtual ~Derived(); };
Derived *make_d();
#endif
""", """#include "lib.h"
Derived::~Derived() {}
Derived *make_d() { return new Derived(); }
""", """#ifndef LIB_H
#define LIB_H
class BaseNew { public: int b; virtual ~BaseNew(); };
class Derived : public BaseNew { public: int a; virtual ~Derived(); };
Derived *make_d();
#endif
""", """#include "lib.h"
BaseNew::~BaseNew() {}
Derived::~Derived() {}
Derived *make_d() { return new Derived(); }
"""),
    "base class inserted -> subobject layout and vtable change")

# =================================================================== C++: members

_M1H = """#ifndef LIB_H
#define LIB_H
class Counter {
  int n;
public:
  Counter();
  void bump();
  int value() const;
};
#endif
"""
_M1C = """#include "lib.h"
Counter::Counter() : n(0) {}
void Counter::bump() { n++; }
int Counter::value() const { return n; }
"""

add("cxx_data_member_added", "cxx", "struct_field_added", True,
    *cxx_case(_M1H, _M1C,
              _M1H.replace("  int n;", "  int n;\n  int step;"),
              _M1C.replace("Counter::Counter() : n(0) {}", "Counter::Counter() : n(0), step(1) {}")
                  .replace("void Counter::bump() { n++; }", "void Counter::bump() { n += step; }")),
    "private data member added -> class size grows (the classic C++ ABI break)")

add("cxx_method_sig_changed", "cxx", "function_param_type_changed", True,
    *cxx_case(_M1H, _M1C,
              _M1H.replace("void bump();", "void bump(int by);"),
              _M1C.replace("void Counter::bump() { n++; }", "void Counter::bump(int by) { n += by; }")),
    "member function signature change -> mangled name changes")

add("cxx_nonvirtual_method_added", "cxx", "method_added_nonvirtual", False,
    *cxx_case(_M1H, _M1C,
              _M1H.replace("  int value() const;", "  int value() const;\n  void reset();"),
              _M1C + "\nvoid Counter::reset() { n = 0; }\n"),
    "non-virtual method added -> additive, layout untouched")

add("cxx_enum_class_case_added", "cxx", "enum_case_added", True,
    *cxx_case("""#ifndef LIB_H
#define LIB_H
enum class Mode { Read, Write };
int apply(Mode m);
#endif
""", """#include "lib.h"
int apply(Mode m) { return static_cast<int>(m); }
""", """#ifndef LIB_H
#define LIB_H
enum class Mode { Read, Write, Append };
int apply(Mode m);
#endif
""", """#include "lib.h"
int apply(Mode m) { return static_cast<int>(m); }
"""),
    "scoped enum gains a case")

add("cxx_inline_method_body_changed", "cxx", "inline_body_changed", True,
    *cxx_case("""#ifndef LIB_H
#define LIB_H
class Vec {
  int x, y;
public:
  Vec(int a, int b) : x(a), y(b) {}
  int norm1() const { return (x < 0 ? -x : x) + (y < 0 ? -y : y); }
};
int use_vec(int a, int b);
#endif
""", """#include "lib.h"
int use_vec(int a, int b) { return Vec(a,b).norm1(); }
""", """#ifndef LIB_H
#define LIB_H
class Vec {
  int x, y;
public:
  Vec(int a, int b) : x(a), y(b) {}
  int norm1() const { int s = 0; s += (x<0?-x:x); s += (y<0?-y:y); return s + 1; }
};
int use_vec(int a, int b);
#endif
""", """#include "lib.h"
int use_vec(int a, int b) { return Vec(a,b).norm1(); }
"""),
    "in-class method body changes; layout and mangled names identical")

add("cxx_template_body_changed", "cxx", "inline_body_changed", True,
    *cxx_case("""#ifndef LIB_H
#define LIB_H
template <class T> T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
int use_clamp(int v);
#endif
""", """#include "lib.h"
int use_clamp(int v) { return clampv<int>(v, 0, 10); }
""", """#ifndef LIB_H
#define LIB_H
template <class T> T clampv(T v, T lo, T hi) { if (v < lo) return lo; if (v > hi) return hi; return v + T(1); }
int use_clamp(int v);
#endif
""", """#include "lib.h"
int use_clamp(int v) { return clampv<int>(v, 0, 10); }
"""),
    "template body changes; instantiated in every client TU")

add("cxx_pimpl_field_added", "cxx", "opaque_impl_changed", False,
    *cxx_case("""#ifndef LIB_H
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
""", """#include "lib.h"
class Impl { public: int a; };
Handle::Handle() : p(new Impl()) { p->a = 1; }
Handle::~Handle() { delete p; }
int Handle::get() const { return p->a; }
""", """#ifndef LIB_H
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
""", """#include "lib.h"
class Impl { public: int a; int b; };
Handle::Handle() : p(new Impl()) { p->a = 1; p->b = 2; }
Handle::~Handle() { delete p; }
int Handle::get() const { return p->a + p->b; }
"""),
    "pimpl: hidden impl type grows. This is manual resilience -- "
    "the exact thing Hylo's boundary would automate. Expect NO public break.")

add("cxx_no_change", "cxx", "none", False, *cxx_case(_M1H, _M1C, _M1H, _M1C),
    "control: identical sources")
