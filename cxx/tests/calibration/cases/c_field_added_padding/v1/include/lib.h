#ifndef LIB_H
#define LIB_H
struct Pad { char a; /* 3 bytes padding */ int b; };
int pad_get(struct Pad *p);
#endif
