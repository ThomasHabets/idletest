#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/io.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* PID 1 in a disposable VM; never run this program on the host. */
struct state {
    _Atomic unsigned long count[3];
    _Atomic unsigned int ready;
    _Atomic unsigned int reader_started;
    _Atomic unsigned int reader_done;
    _Atomic unsigned int writer_done;
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

static void send_fd(int socket, int fd)
{
    char data = 0, control[CMSG_SPACE(sizeof(fd))] = {0};
    struct iovec iov = { .iov_base = &data, .iov_len = 1 };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = control, .msg_controllen = sizeof(control) };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
    if (sendmsg(socket, &msg, 0) != 1)
        fail("send userfaultfd");
}

static int receive_fd(int socket)
{
    int fd;
    char data, control[CMSG_SPACE(sizeof(fd))] = {0};
    struct iovec iov = { .iov_base = &data, .iov_len = 1 };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = control, .msg_controllen = sizeof(control) };
    if (recvmsg(socket, &msg, 0) != 1)
        fail("receive userfaultfd");
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
        fail("missing userfaultfd");
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    return fd;
}

static int wait_flag(_Atomic unsigned int *flag, unsigned int ms)
{
    for (unsigned int i = 0; i < ms / 10; i++) {
        if (atomic_load(flag))
            return 1;
        pause_ms(10);
    }
    return atomic_load(flag) != 0;
}

static void run_pending_signal(void)
{
    memset(state, 0, sizeof(*state));
    feature(0);
    pid_t background = worker(1, 1, NULL);
    pid_t foreground = worker(0, 0, NULL);
    while (atomic_load(&state->ready) != 3)
        pause_ms(10);
    feature(1);
    pause_ms(250);
    unsigned long n = atomic_load(&state->count[0]);
    kill(background, SIGKILL);
    int reaped = 0;
    for (int i = 0; i < 300; i++) {
        if (waitpid(background, NULL, WNOHANG) == background) {
            reaped = 1;
            break;
        }
        pause_ms(10);
    }
    n = atomic_load(&state->count[0]) - n;
    check(reaped && n > 0,
          "pending SIGKILL escapes starvation while foreground stays runnable",
          n, reaped);
    feature(0);
    kill(foreground, SIGKILL);
    waitpid(foreground, NULL, 0);
    if (!reaped)
        waitpid(background, NULL, 0);
}

/*
 * pipe_write() holds pipe->mutex across copy_page_from_iter(). Stall that
 * copy with userfaultfd, start a normal reader blocked on the mutex, and
 * resolve the fault while another normal task continuously occupies CPU 1.
 * Kernel-mode exemption must let the idle writer release the mutex, but its
 * first userspace store after write() must remain starved at the return gate.
 */
static void run_kernel_lock(const char *writer_group, int idle_policy)
{
    int pipefd[2], sockets[2];
    const size_t size = 4096;
    memset(state, 0, sizeof(*state));
    feature(0);
    if (pipe(pipefd) || socketpair(AF_UNIX, SOCK_STREAM, 0, sockets))
        fail("kernel-lock test descriptors");
    pid_t writer = fork();
    if (writer < 0)
        fail("fork idle writer");
    if (!writer) {
        close(sockets[0]);
        pin(1);
        if (writer_group)
            write_file(writer_group, "0");
        int uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
        if (uffd < 0)
            fail("userfaultfd (kernel faults must be allowed)");
        struct uffdio_api api = { .api = UFFD_API };
        if (ioctl(uffd, UFFDIO_API, &api))
            fail("UFFDIO_API");
        void *page = mmap(NULL, size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED)
            fail("faulting source page");
        struct uffdio_register reg = {
            .range = { .start = (uintptr_t)page, .len = size },
            .mode = UFFDIO_REGISTER_MODE_MISSING,
        };
        if (ioctl(uffd, UFFDIO_REGISTER, &reg))
            fail("UFFDIO_REGISTER");
        send_fd(sockets[1], uffd);
        close(sockets[1]);
        if (idle_policy) {
            struct sched_param param = {0};
            if (sched_setscheduler(0, SCHED_IDLE, &param))
                fail("idle writer policy");
        }
        if (write(pipefd[1], page, size) != (ssize_t)size)
            fail("faulting pipe write");
        atomic_store(&state->writer_done, 1);
        _exit(0);
    }
    close(sockets[1]);
    int uffd = receive_fd(sockets[0]);
    close(sockets[0]);
    struct pollfd pollfd = { .fd = uffd, .events = POLLIN };
    if (poll(&pollfd, 1, 5000) != 1)
        fail("wait for kernel page fault");
    struct uffd_msg msg;
    if (read(uffd, &msg, sizeof(msg)) != sizeof(msg) ||
        msg.event != UFFD_EVENT_PAGEFAULT)
        fail("kernel page fault event");

    pid_t reader = fork();
    if (reader < 0)
        fail("fork pipe reader");
    if (!reader) {
        char data[4096];
        pin(1);
        atomic_store(&state->reader_started, 1);
        ssize_t ret = read(pipefd[0], data, sizeof(data));
        atomic_store(&state->reader_done,
                     ret == sizeof(data) && data[0] == 'x' ? 1 : 2);
        _exit(0);
    }
    if (!wait_flag(&state->reader_started, 2000))
        fail("pipe reader startup");
    pause_ms(100);
    pid_t burner = worker(0, 0, NULL);
    if (!wait_flag(&state->ready, 2000))
        fail("normal burner startup");
    feature(1);
    char *donor = mmap(NULL, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (donor == MAP_FAILED)
        fail("donor page");
    memset(donor, 'x', size);
    struct uffdio_copy copy = {
        .dst = msg.arg.pagefault.address & ~(uint64_t)(size - 1),
        .src = (uintptr_t)donor, .len = size,
    };
    if (ioctl(uffd, UFFDIO_COPY, &copy))
        fail("resolve kernel-mode fault");
    int progressed = wait_flag(&state->reader_done, 3000);
    check(progressed && atomic_load(&state->reader_done) == 1,
          "idle kernel writer releases pipe mutex under normal CPU load",
          atomic_load(&state->reader_done), atomic_load(&state->writer_done));
    pause_ms(250);
    check(atomic_load(&state->writer_done) == 0,
          "idle writer is stopped before returning to userspace", 0,
          atomic_load(&state->writer_done));

    kill(burner, SIGKILL);
    waitpid(burner, NULL, 0);
    int returned = wait_flag(&state->writer_done, 3000);
    check(returned,
          "idle writer returns once foreground work stops", 0,
          atomic_load(&state->writer_done));
    /* Always restore progress for cleanup, including on an old kernel. */
    feature(0);
    waitpid(reader, NULL, 0);
    waitpid(writer, NULL, 0);
    close(uffd);
    close(pipefd[0]);
    close(pipefd[1]);
    munmap(donor, size);
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

    int lock_only = getenv("idle_lock_only") != NULL;
    if (!lock_only) {
        run_pair(NULL, NULL, 1, "same group: idle has zero service");
        run_three();
        run_pending_signal();
    }
    run_kernel_lock(NULL, 1);

    write_file("/sys/fs/cgroup/cgroup.subtree_control", "+cpu");
    mkdir("/sys/fs/cgroup/normal", 0755);
    mkdir("/sys/fs/cgroup/background", 0755);
    write_file("/sys/fs/cgroup/background/cgroup.subtree_control", "+cpu");
    mkdir("/sys/fs/cgroup/background/nested", 0755);
    if (!lock_only)
        run_pair("/sys/fs/cgroup/normal/cgroup.procs",
             "/sys/fs/cgroup/background/nested/cgroup.procs", 1,
             "nested groups: idle-only descendants have zero service");
    run_kernel_lock("/sys/fs/cgroup/background/nested/cgroup.procs", 1);

    write_file("/sys/fs/cgroup/background/cpu.idle", "1");
    if (!lock_only)
        run_pair("/sys/fs/cgroup/normal/cgroup.procs",
             "/sys/fs/cgroup/background/nested/cgroup.procs", 0,
             "cpu.idle group: normal-policy background has zero service");
    run_kernel_lock("/sys/fs/cgroup/background/nested/cgroup.procs", 0);

    printf("IDLE_ZERO_WEIGHT_TEST_RESULT: %s (%d failures)\n",
           failures ? "FAIL" : "PASS", failures);
    sync();
    /* Exit the disposable VM even when its kernel has no ACPI poweroff. */
    if (!ioperm(0xf4, 4, 1))
        outl(failures ? 1 : 0, 0xf4);
    reboot(RB_POWER_OFF);
    return failures ? 1 : 0;
}
