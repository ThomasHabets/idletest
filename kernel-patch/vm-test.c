#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* PID 1 in a disposable VM; never run this program on the host. */
struct state {
    _Atomic unsigned long count[3];
    _Atomic unsigned int ready;
};
static struct state *state;
static int failures;

static void fail(const char *what)
{
    perror(what);
    exit(2);
}

static void write_file(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0 || write(fd, value, strlen(value)) != (ssize_t)strlen(value))
        fail(path);
    close(fd);
}

static void pin(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set))
        fail("sched_setaffinity");
}

static void feature(int on)
{
    write_file("/sys/kernel/debug/sched/features",
               on ? "IDLE_ZERO_WEIGHT" : "NO_IDLE_ZERO_WEIGHT");
}

static void pause_ms(unsigned int ms)
{
    struct timespec t = { .tv_sec = ms / 1000,
                          .tv_nsec = (ms % 1000) * 1000000UL };
    while (nanosleep(&t, &t) && errno == EINTR)
        ;
}

static void check(int good, const char *name, unsigned long normal,
                  unsigned long background)
{
    printf("%s: %s (normal=%lu background=%lu)\n",
           good ? "PASS" : "FAIL", name, normal, background);
    if (!good)
        failures++;
}

static pid_t worker(int slot, int idle, const char *group)
{
    pid_t pid = fork();
    if (pid < 0)
        fail("fork");
    if (pid)
        return pid;

    if (group)
        write_file(group, "0");
    pin(1);
    if (idle) {
        struct sched_param param = {0};
        if (sched_setscheduler(0, SCHED_IDLE, &param))
            fail("SCHED_IDLE");
    }
    atomic_fetch_or(&state->ready, 1U << slot);
    for (;;) {
        for (int i = 0; i < 4096; i++)
            __asm__ volatile("" ::: "memory");
        atomic_fetch_add_explicit(&state->count[slot], 1,
                                  memory_order_relaxed);
    }
}

static void measure_for(const char *name, int expect_background, unsigned int ms)
{
    pause_ms(250);
    unsigned long n = atomic_load(&state->count[0]);
    unsigned long b = atomic_load(&state->count[1]);
    pause_ms(ms);
    n = atomic_load(&state->count[0]) - n;
    b = atomic_load(&state->count[1]) - b;
    check(n > 0 && (expect_background ? b > 0 : b == 0), name, n, b);
}

static void measure(const char *name, int expect_background)
{
    measure_for(name, expect_background, 2000);
}

static void run_pair(const char *normal_group, const char *idle_group,
                     int idle_policy, const char *name)
{
    memset(state, 0, sizeof(*state));
    feature(0);
    pid_t background = worker(1, idle_policy, idle_group);
    pid_t normal = worker(0, 0, normal_group);
    for (int i = 0; atomic_load(&state->ready) != 3; i++) {
        if (i == 1000)
            fail("worker startup timeout");
        pause_ms(10);
    }

    /* A cpu.idle group's tiny share can miss a short measurement window. */
    measure_for("feature disabled: both policies progress", 1,
                idle_policy ? 2000 : 10000);
    feature(1);
    measure(name, 0);
    if (setpriority(PRIO_PROCESS, normal, 19))
        fail("setpriority normal worker");
    measure("nice-19 normal still suppresses background", 0);
    if (normal_group && idle_policy) {
        write_file("/sys/fs/cgroup/normal/cpu.weight", "1");
        write_file("/sys/fs/cgroup/background/cpu.weight", "10000");
        measure("strict precedence overrides competing cgroup weights", 0);
        write_file("/sys/fs/cgroup/normal/cpu.weight", "100");
        write_file("/sys/fs/cgroup/background/cpu.weight", "100");
    }

    kill(normal, SIGSTOP);
    if (waitpid(normal, NULL, WUNTRACED) != normal)
        fail("wait for stopped normal worker");
    pause_ms(250);
    unsigned long b = atomic_load(&state->count[1]);
    pause_ms(500);
    b = atomic_load(&state->count[1]) - b;
    check(b > 0, "background runs when normal blocks", 0, b);

    kill(normal, SIGCONT);
    measure("normal wakeup suppresses background again", 0);
    feature(0);
    measure("runtime toggle restores weighted scheduling", 1);

    kill(normal, SIGKILL);
    kill(background, SIGKILL);
    waitpid(normal, NULL, 0);
    waitpid(background, NULL, 0);
}

static void run_three(void)
{
    memset(state, 0, sizeof(*state));
    feature(0);
    pid_t background = worker(1, 1, NULL);
    pid_t first = worker(0, 0, NULL);
    pid_t second = worker(2, 0, NULL);
    while (atomic_load(&state->ready) != 7)
        pause_ms(10);
    feature(1);
    pause_ms(250);
    unsigned long a = atomic_load(&state->count[0]);
    unsigned long b = atomic_load(&state->count[1]);
    unsigned long c = atomic_load(&state->count[2]);
    pause_ms(2000);
    a = atomic_load(&state->count[0]) - a;
    b = atomic_load(&state->count[1]) - b;
    c = atomic_load(&state->count[2]) - c;
    check(a > 0 && c > 0 && b == 0 && a < 2 * c && c < 2 * a,
          "two normal workers retain fairness; idle starves", a, b);
    kill(first, SIGKILL);
    kill(second, SIGKILL);
    kill(background, SIGKILL);
    waitpid(first, NULL, 0);
    waitpid(second, NULL, 0);
    waitpid(background, NULL, 0);
}

int main(void)
{
    if (getpid() != 1) {
        fprintf(stderr, "This test must run as PID 1 in a disposable VM.\n");
        return 2;
    }
    setbuf(stdout, NULL);
    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    mkdir("/dev", 0755);
    if (mount("proc", "/proc", "proc", 0, NULL) ||
        mount("sysfs", "/sys", "sysfs", 0, NULL) ||
        mount("devtmpfs", "/dev", "devtmpfs", 0, NULL))
        fail("mount base filesystems");
    if (mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL))
        fail("mount debugfs");
    if (mount("cgroup2", "/sys/fs/cgroup", "cgroup2", 0, NULL))
        fail("mount cgroup2");
    pin(0);
    state = mmap(NULL, sizeof(*state), PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (state == MAP_FAILED)
        fail("mmap");

    run_pair(NULL, NULL, 1, "same group: idle has zero service");
    run_three();

    write_file("/sys/fs/cgroup/cgroup.subtree_control", "+cpu");
    mkdir("/sys/fs/cgroup/normal", 0755);
    mkdir("/sys/fs/cgroup/background", 0755);
    write_file("/sys/fs/cgroup/background/cgroup.subtree_control", "+cpu");
    mkdir("/sys/fs/cgroup/background/nested", 0755);
    run_pair("/sys/fs/cgroup/normal/cgroup.procs",
             "/sys/fs/cgroup/background/nested/cgroup.procs", 1,
             "nested groups: idle-only descendants have zero service");

    write_file("/sys/fs/cgroup/background/cpu.idle", "1");
    run_pair("/sys/fs/cgroup/normal/cgroup.procs",
             "/sys/fs/cgroup/background/nested/cgroup.procs", 0,
             "cpu.idle group: normal-policy background has zero service");

    printf("IDLE_ZERO_WEIGHT_TEST_RESULT: %s (%d failures)\n",
           failures ? "FAIL" : "PASS", failures);
    sync();
    reboot(RB_POWER_OFF);
    return failures ? 1 : 0;
}
