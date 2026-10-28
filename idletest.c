// Set this sysctl: sudo sysctl -w kernel.sched_rt_runtime_us=1000000
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

static void spin_loop_hint(void) {
    // Attempt at preventing optimization steps.
    asm("nop");
}

static void pin_to_cpu0(void) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);

    int ret = pthread_setaffinity_np(pthread_self(),
                                     sizeof(cpu_set_t),
                                     &cpuset);
    if (ret != 0) {
        fprintf(stderr, "pthread_setaffinity_np failed: %d\n", ret);
    }
}

static void set_sched_idle(void) {
    struct sched_param param;
    param.sched_priority = 0;  /* must be 0 for SCHED_IDLE */

    int ret = sched_setscheduler(0, SCHED_IDLE, &param);
    if (ret != 0) {
        int err = errno;
        fprintf(stderr,
                "sched_setscheduler(SCHED_IDLE) failed: ret=%d, errno=%d\n",
                ret, err);
    }
}

static int lock_all_memory(void) {
    int flags = MCL_CURRENT | MCL_FUTURE;
    int ret = mlockall(flags);
    if (ret != 0) {
        /* mlockall returns -1 on error and sets errno */
        perror("mlockall");
        return -1;
    }
    return 0;
}

static void set_sched_fifo(void) {
    struct sched_param param;
    param.sched_priority = 99;

    int ret = sched_setscheduler(0, SCHED_FIFO, &param);
    if (ret != 0) {
        int err = errno;
        fprintf(stderr,
                "sched_setscheduler(SCHED_FIFO) failed: ret=%d, errno=%d\n",
                ret, err);
    }
}

static void idle_loop() {
    unsigned long long i = 0;
    for (;;) {
        spin_loop_hint();
        i++;
        if (i % 100000ULL == 0ULL) {
            printf("Why is SCHED_IDLE ever given any CPU? %lu\n", i);
        }
    }
}

static void *idle_thread_func(void *arg) {
    (void)arg;
/*
    if (lock_all_memory() != 0) {
        fprintf(stderr, "lock_all_memory() failed in idle thread\n");
        exit(EXIT_FAILURE);
    }
*/

    pin_to_cpu0();
    set_sched_idle();

    printf("SCHED_IDLE thread running on CPU 0\n");
    idle_loop();


    /* Unreachable, but required for pthread signature */
    return NULL;
}

void infinite_loop() {
    for (;;) {
        spin_loop_hint();
    }
}

int main(void) {
/*
    if (lock_all_memory() != 0) {
        fprintf(stderr, "lock_all_memory() failed in main\n");
        return EXIT_FAILURE;
    }
*/

    /* SCHED_IDLE spinning thread, also on CPU 0 */
    pthread_t idle;
    int ret = pthread_create(&idle, NULL, idle_thread_func, NULL);
    if (ret != 0) {
        fprintf(stderr, "pthread_create failed: %d\n", ret);
        return EXIT_FAILURE;
    }

    /* "Normal" spinning thread (actually SCHED_FIFO here), pinned to CPU 0 */
    pin_to_cpu0();
    //set_sched_fifo();
    printf("Normal thread running on CPU 0\n");
    infinite_loop();

    /* Unreachable */
    return 0;
}
