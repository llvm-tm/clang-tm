// Freestanding variant of tsx_conflict_matrix.c for gem5 SE (no pthreads barrier inside TX).
#include <immintrin.h>
int main(){ unsigned s=_xbegin(); if(s==_XBEGIN_STARTED) _xend(); return 0; }
