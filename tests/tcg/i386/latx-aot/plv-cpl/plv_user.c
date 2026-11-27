typedef unsigned int u32;
typedef unsigned long u64;

#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_MMAP 9
#define SYS_IOCTL 16
#define SYS_EXIT 60

#define O_RDWR 2
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define PLV_TEST_TOUCH 0x5001

static long syscall6(long nr, long a1, long a2, long a3,
                     long a4, long a5, long a6)
{
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;
    long ret;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

static void write_text(const char *text, u64 size)
{
    syscall6(SYS_WRITE, 1, (long)text, size, 0, 0, 0);
}

static __attribute__((noreturn)) void exit_process(int status)
{
    syscall6(SYS_EXIT, status, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

static void fail(const char *text, u64 size)
{
    write_text(text, size);
    exit_process(1);
}

void _start(void)
{
    static const char start[] = "USER_MIN=start\n";
    static const char positive[] = "USER_MIN=user-positive\n";
    static const char positive_failed[] = "USER_MIN=fail-user-page\n";
    static const char mapped[] = "USER_MIN=supervisor-mmap\n";
    static const char preload[] = "USER_MIN=preload\n";
    static const char read_fault[] = "USER_MIN=read-expect-fault\n";
    static const char read_succeeded[] = "USER_MIN=fail-read-succeeded\n";
    static const char mmap_failed[] = "USER_MIN=fail-mmap\n";
    static const char open_failed[] = "USER_MIN=fail-open\n";
    static const char ioctl_failed[] = "USER_MIN=fail-ioctl\n";
    volatile u32 *positive_page;
    volatile u32 *supervisor;
    long fd;

    write_text(start, sizeof(start) - 1);

    positive_page = (volatile u32 *)syscall6(
        SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((long)positive_page == -1) {
        fail(mmap_failed, sizeof(mmap_failed) - 1);
    }
    *positive_page = 0x12345678U;
    if (*positive_page != 0x12345678U) {
        fail(positive_failed, sizeof(positive_failed) - 1);
    }
    write_text(positive, sizeof(positive) - 1);

    fd = syscall6(SYS_OPEN, (long)"/dev/plv_tlb", O_RDWR, 0, 0, 0, 0);
    if (fd < 0) {
        fail(open_failed, sizeof(open_failed) - 1);
    }
    supervisor = (volatile u32 *)syscall6(
        SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
        MAP_SHARED, fd, 0);
    if ((long)supervisor == -1) {
        fail(mmap_failed, sizeof(mmap_failed) - 1);
    }
    write_text(mapped, sizeof(mapped) - 1);

    if (syscall6(SYS_IOCTL, fd, PLV_TEST_TOUCH, (long)supervisor,
                 0, 0, 0) != 0) {
        fail(ioctl_failed, sizeof(ioctl_failed) - 1);
    }
    write_text(preload, sizeof(preload) - 1);
    write_text(read_fault, sizeof(read_fault) - 1);

    (void)*supervisor;
    fail(read_succeeded, sizeof(read_succeeded) - 1);
}
