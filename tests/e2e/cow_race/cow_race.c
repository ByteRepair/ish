// Threads write their own slots of the same copy-on-write pages at once, right
// after a fork. Two threads copying one page used to keep only one's writes.
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define THREADS 6
#define PAGES 64
#define ROUNDS 40
#define STRIDE (4096 / sizeof(long))

static atomic_int go;
static atomic_int done;
static long *pages;

static void *worker(void *arg) {
    long t = (long) arg;
    for (int round = 1; round <= ROUNDS; round++) {
        while (atomic_load(&go) < round)
            sched_yield();
        for (int p = 0; p < PAGES; p++)
            pages[p * STRIDE + t] = round * 100 + t;
        atomic_fetch_add(&done, 1);
    }
    return NULL;
}

int main(void) {
    pages = mmap(NULL, PAGES * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    for (int p = 0; p < PAGES; p++)
        pages[p * STRIDE + THREADS] = 1;
    pthread_t threads[THREADS];
    for (long t = 0; t < THREADS; t++)
        pthread_create(&threads[t], NULL, worker, (void *) t);

    for (int round = 1; round <= ROUNDS; round++) {
        // share every page with a child, which then exits
        pid_t pid = fork();
        if (pid == 0)
            _exit(0);
        waitpid(pid, NULL, 0);

        atomic_store(&go, round);
        while (atomic_load(&done) < round * THREADS)
            sched_yield();
        for (int p = 0; p < PAGES; p++) {
            for (int t = 0; t < THREADS; t++) {
                if (pages[p * STRIDE + t] != round * 100 + t) {
                    printf("round %d: thread %d's write to page %d was lost\n", round, t, p);
                    return 1;
                }
            }
        }
    }
    for (int t = 0; t < THREADS; t++)
        pthread_join(threads[t], NULL);
    puts("ok");
    return 0;
}
