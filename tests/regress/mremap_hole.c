// Growing a mapping whose old range has a hole in it is EFAULT, and must not
// dereference the missing page table entry on the way to saying so.
#include "sys.h"

void _start(void) {
    char *a = mmap_(0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(!is_err((slong) a), "mmap failed");
    check(sys2(NR_munmap, a + 4096, 4096) == 0, "munmap of the middle page failed");
    slong r = sys4(NR_mremap, a, 3 * 4096, 4 * 4096, 0);
    check(r == -EFAULT, "mremap over a hole did not answer EFAULT");
    finish(1, 0);
}
