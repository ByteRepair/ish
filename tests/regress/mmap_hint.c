// A hint that is not free is only a hint: mmap without MAP_FIXED must place
// the mapping elsewhere, not over the pages already at the hint.
#include "sys.h"

void _start(void) {
    char *a = mmap_(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(!is_err((slong) a), "first mmap failed");
    a[0] = 'A';
    char *b = mmap_(a, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(!is_err((slong) b), "hinted mmap failed");
    check(b != a, "hinted mmap returned the taken address");
    check(a[0] == 'A', "hinted mmap replaced the existing mapping");
    finish(1, 0);
}
