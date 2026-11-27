typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;

#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_MMAP 9
#define SYS_IOCTL 16
#define SYS_EXIT 60
#define SYS_REBOOT 169

#define O_RDWR 2
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20

#define PAGE_BYTES 4096UL
#define PTWFLAGS_TLB_TOUCH 0x5002

#define PTWFLAGS_MAP_RAM 0
#define PTWFLAGS_MAP_VGA 1
#define PTWFLAGS_MAP_MMIO 2
#define PTWFLAGS_MAP_SUPERVISOR 3
#define PTWFLAGS_MAP_READONLY 4

#define LINUX_REBOOT_MAGIC1 0xfee1dead
#define LINUX_REBOOT_MAGIC2 672274793
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321fedc

#define MODE_BASELINE 1
#define MODE_SMC 2
#define MODE_VGA 3
#define MODE_MMIO 4
#define MODE_SUPER_READ 5
#define MODE_SUPER_WRITE 6
#define MODE_SUPER_EXEC 7
#define MODE_SUPER_WRITE_COLD 8
#define MODE_READONLY_WRITE 9
#define MODE_USER_PRESENT_READONLY_WRITE 10

#ifndef TEST_MODE
#error TEST_MODE must be defined
#endif

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

static void write_hex(const char *tag, u64 tag_size, u32 value)
{
	static const char digits[] = "0123456789abcdef";
	char text[11];

	text[0] = '0';
	text[1] = 'x';
	for (unsigned i = 0; i < 8; i++) {
		text[2 + i] = digits[(value >> (28 - i * 4)) & 0xf];
	}
	text[10] = '\n';
	write_text(tag, tag_size);
	write_text(text, sizeof(text));
}

static __attribute__((noreturn)) void exit_status(int status)
{
	syscall6(SYS_EXIT, status, 0, 0, 0, 0, 0);
	__builtin_unreachable();
}

static void poweroff_machine(void)
{
	syscall6(SYS_REBOOT, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
		 LINUX_REBOOT_CMD_POWER_OFF, 0, 0, 0);
}

static __attribute__((noreturn)) void fail(const char *text, u64 size)
{
	write_text(text, size);
	poweroff_machine();
	exit_status(1);
}

static int syscall_failed(long value)
{
	return value < 0 && value >= -4095;
}

static void *map_anonymous(u64 size, int prot)
{
	long ret = syscall6(SYS_MMAP, 0, size, prot,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	return syscall_failed(ret) ? (void *)-1 : (void *)ret;
}

static __attribute__((unused)) long open_device(void)
{
	return syscall6(SYS_OPEN, (long)"/dev/ptwflags_tlb", O_RDWR,
			0, 0, 0, 0);
}

static __attribute__((unused)) void *map_device(long fd, unsigned long kind,
						 int prot)
{
	long ret = syscall6(SYS_MMAP, 0, PAGE_BYTES, prot, MAP_SHARED,
			    fd, kind * PAGE_BYTES);
	return syscall_failed(ret) ? (void *)-1 : (void *)ret;
}

static __attribute__((unused)) int touch_supervisor(long fd, void *page)
{
	return syscall6(SYS_IOCTL, fd, PTWFLAGS_TLB_TOUCH, (long)page,
			0, 0, 0) == 0 ? 0 : -1;
}

static void write_code(u8 *page, u32 value)
{
	page[0] = 0xb8;
	page[1] = value;
	page[2] = value >> 8;
	page[3] = value >> 16;
	page[4] = value >> 24;
	page[5] = 0xc3;
	__builtin___clear_cache((char *)page, (char *)page + 6);
}

static u32 execute_code(u8 *page)
{
	u32 (*fn)(void) = (void *)page;
	return fn();
}

static int check_code(u8 *base, unsigned page, u32 value)
{
	write_code(base + page * PAGE_BYTES, value);
	return execute_code(base + page * PAGE_BYTES) == value ? 0 : -1;
}

static __attribute__((unused)) void run_baseline(void)
{
	static const char start[] = "PTWFLAGS_BASELINE=begin\n";
	static const char success[] = "PTWFLAGS_BASELINE=PASS\n";
	static const char mmap_failed[] = "PTWFLAGS_BASELINE=FAIL-mmap\n";
	static const char mismatch[] = "PTWFLAGS_BASELINE=FAIL-data\n";
	volatile u32 *page;

	write_text(start, sizeof(start) - 1);
	page = map_anonymous(2 * PAGE_BYTES, PROT_READ | PROT_WRITE);
	if ((long)page == -1) {
		fail(mmap_failed, sizeof(mmap_failed) - 1);
	}
	page[0] = 0x11223344U;
	page[PAGE_BYTES / sizeof(*page)] = 0x55667788U;
	if (page[0] != 0x11223344U ||
	    page[PAGE_BYTES / sizeof(*page)] != 0x55667788U) {
		fail(mismatch, sizeof(mismatch) - 1);
	}
	write_text(success, sizeof(success) - 1);
}

static __attribute__((unused)) void run_smc(void)
{
	static const char start[] = "PTWFLAGS_SMC=begin\n";
	static const char success[] = "PTWFLAGS_SMC=PASS\n";
	static const char mmap_failed[] = "PTWFLAGS_SMC=FAIL-mmap\n";
	static const char stale[] = "PTWFLAGS_SMC=FAIL-stale-code\n";
	u8 *base;

	write_text(start, sizeof(start) - 1);
	base = map_anonymous(2 * PAGE_BYTES,
			     PROT_READ | PROT_WRITE | PROT_EXEC);
	if ((long)base == -1) {
		fail(mmap_failed, sizeof(mmap_failed) - 1);
	}
	base[0] = 0x90;
	base[PAGE_BYTES] = 0x90;
	if (check_code(base, 0, 0x40000000U) ||
	    check_code(base, 1, 0x40000001U)) {
		fail(stale, sizeof(stale) - 1);
	}
	if (check_code(base, 0, 0x50000000U) ||
	    check_code(base, 1, 0x50000001U)) {
		fail(stale, sizeof(stale) - 1);
	}
	write_text(success, sizeof(success) - 1);
}

static __attribute__((unused)) void run_vga(void)
{
	static const char start[] = "PTWFLAGS_VGA=begin\n";
	static const char success[] = "PTWFLAGS_VGA=PASS\n";
	static const char open_failed[] = "PTWFLAGS_VGA=FAIL-open\n";
	static const char mmap_failed[] = "PTWFLAGS_VGA=FAIL-mmap\n";
	volatile u32 *page;
	u32 original;
	long fd;

	write_text(start, sizeof(start) - 1);
	fd = open_device();
	if (fd < 0) {
		fail(open_failed, sizeof(open_failed) - 1);
	}
	page = map_device(fd, PTWFLAGS_MAP_VGA, PROT_READ | PROT_WRITE);
	if ((long)page == -1) {
		fail(mmap_failed, sizeof(mmap_failed) - 1);
	}
	original = *page;
	write_hex("PTWFLAGS_VGA_ORIGINAL=",
		  sizeof("PTWFLAGS_VGA_ORIGINAL=") - 1, original);
	*page = original ^ 0x01010101U;
	write_hex("PTWFLAGS_VGA_WRITTEN=",
		  sizeof("PTWFLAGS_VGA_WRITTEN=") - 1, *page);
	*page = original;
	write_text(success, sizeof(success) - 1);
}

static __attribute__((unused)) void run_mmio(void)
{
	static const char start[] = "PTWFLAGS_MMIO=begin\n";
	static const char success[] = "PTWFLAGS_MMIO=PASS\n";
	static const char open_failed[] = "PTWFLAGS_MMIO=FAIL-open\n";
	static const char mmap_failed[] = "PTWFLAGS_MMIO=FAIL-mmap\n";
	volatile u32 *page;
	u32 first;
	long fd;

	write_text(start, sizeof(start) - 1);
	fd = open_device();
	if (fd < 0) {
		fail(open_failed, sizeof(open_failed) - 1);
	}
	page = map_device(fd, PTWFLAGS_MAP_MMIO, PROT_READ | PROT_WRITE);
	if ((long)page == -1) {
		fail(mmap_failed, sizeof(mmap_failed) - 1);
	}
	first = *page;
	write_hex("PTWFLAGS_MMIO_FIRST=",
		  sizeof("PTWFLAGS_MMIO_FIRST=") - 1, first);
	*page = 0;
	write_hex("PTWFLAGS_MMIO_SECOND=",
		  sizeof("PTWFLAGS_MMIO_SECOND=") - 1, *page);
	write_text(success, sizeof(success) - 1);
}

static __attribute__((unused)) void run_supervisor_read(void)
{
	static const char start[] = "PTWFLAGS_SUPER_READ=begin\n";
	static const char open_failed[] = "PTWFLAGS_SUPER_READ=FAIL-open\n";
	static const char mmap_failed[] = "PTWFLAGS_SUPER_READ=FAIL-mmap\n";
	static const char touch_failed[] = "PTWFLAGS_SUPER_READ=FAIL-touch\n";
	static const char read_succeeded[] = "PTWFLAGS_SUPER_READ=FAIL-read\n";
	volatile u32 *page;
	long fd;

	write_text(start, sizeof(start) - 1);
	fd = open_device();
	if (fd < 0) {
		fail(open_failed, sizeof(open_failed) - 1);
	}
	page = map_device(fd, PTWFLAGS_MAP_SUPERVISOR, PROT_READ);
	if ((long)page == -1) {
		fail(mmap_failed, sizeof(mmap_failed) - 1);
	}
	if (touch_supervisor(fd, (void *)page)) {
		fail(touch_failed, sizeof(touch_failed) - 1);
	}
	(void)*page;
	fail(read_succeeded, sizeof(read_succeeded) - 1);
}

static __attribute__((unused)) void run_supervisor_write(void)
{
	static const char start[] = "PTWFLAGS_SUPER_WRITE=begin\n";
	static const char open_failed[] = "PTWFLAGS_SUPER_WRITE=FAIL-open\n";
	static const char mmap_failed[] = "PTWFLAGS_SUPER_WRITE=FAIL-mmap\n";
	static const char touch_failed[] = "PTWFLAGS_SUPER_WRITE=FAIL-touch\n";
	static const char write_succeeded[] = "PTWFLAGS_SUPER_WRITE=FAIL-write\n";
	volatile u32 *page;
	long fd;

	write_text(start, sizeof(start) - 1);
	fd = open_device();
	if (fd < 0) {
		fail(open_failed, sizeof(open_failed) - 1);
	}
	page = map_device(fd, PTWFLAGS_MAP_SUPERVISOR, PROT_READ);
	if ((long)page == -1) {
		fail(mmap_failed, sizeof(mmap_failed) - 1);
	}
	if (touch_supervisor(fd, (void *)page)) {
		fail(touch_failed, sizeof(touch_failed) - 1);
	}
	*page = 0xfeedfaceU;
	fail(write_succeeded, sizeof(write_succeeded) - 1);
}

static __attribute__((unused)) void run_supervisor_write_cold(void)
{
	static const char start[] = "PTWFLAGS_SUPER_WRITE_COLD=begin\n";
	static const char open_failed[] = "PTWFLAGS_SUPER_WRITE_COLD=FAIL-open\n";
	static const char mmap_failed[] = "PTWFLAGS_SUPER_WRITE_COLD=FAIL-mmap\n";
	static const char write_succeeded[] =
		"PTWFLAGS_SUPER_WRITE_COLD=FAIL-write\n";
	volatile u32 *page;
	long fd;

	write_text(start, sizeof(start) - 1);
	fd = open_device();
	if (fd < 0) {
		fail(open_failed, sizeof(open_failed) - 1);
	}
	page = map_device(fd, PTWFLAGS_MAP_SUPERVISOR, PROT_READ);
	if ((long)page == -1) {
		fail(mmap_failed, sizeof(mmap_failed) - 1);
	}
	*page = 0xfeedfaceU;
	fail(write_succeeded, sizeof(write_succeeded) - 1);
}

static __attribute__((unused)) void run_readonly_write(void)
{
	static const char start[] = "PTWFLAGS_READONLY_WRITE=begin\n";
	static const char mmap_failed[] = "PTWFLAGS_READONLY_WRITE=FAIL-mmap\n";
	static const char write_succeeded[] = "PTWFLAGS_READONLY_WRITE=FAIL-write\n";
	volatile u32 *page = map_anonymous(PAGE_BYTES, PROT_READ);

	write_text(start, sizeof(start) - 1);
	if ((long)page == -1) {
		fail(mmap_failed, sizeof(mmap_failed) - 1);
	}
	*page = 0xfeedfaceU;
	fail(write_succeeded, sizeof(write_succeeded) - 1);
}

static __attribute__((unused)) void run_user_present_readonly_write(void)
{
	static const char start[] = "PTWFLAGS_USER_PRESENT_READONLY_WRITE=begin\n";
	static const char open_failed[] =
		"PTWFLAGS_USER_PRESENT_READONLY_WRITE=FAIL-open\n";
	static const char mmap_failed[] =
		"PTWFLAGS_USER_PRESENT_READONLY_WRITE=FAIL-mmap\n";
	static const char write_succeeded[] =
		"PTWFLAGS_USER_PRESENT_READONLY_WRITE=FAIL-write\n";
	volatile u32 *page;
	long fd;

	write_text(start, sizeof(start) - 1);
	fd = open_device();
	if (fd < 0) {
		fail(open_failed, sizeof(open_failed) - 1);
	}
	page = map_device(fd, PTWFLAGS_MAP_READONLY, PROT_READ);
	if ((long)page == -1) {
		fail(mmap_failed, sizeof(mmap_failed) - 1);
	}
	*page = 0xfeedfaceU;
	fail(write_succeeded, sizeof(write_succeeded) - 1);
}

static __attribute__((unused)) void run_supervisor_exec(void)
{
	static const char start[] = "PTWFLAGS_SUPER_EXEC=begin\n";
	static const char open_failed[] = "PTWFLAGS_SUPER_EXEC=FAIL-open\n";
	static const char mmap_failed[] = "PTWFLAGS_SUPER_EXEC=FAIL-mmap\n";
	static const char touch_failed[] = "PTWFLAGS_SUPER_EXEC=FAIL-touch\n";
	static const char exec_succeeded[] = "PTWFLAGS_SUPER_EXEC=FAIL-exec\n";
	u8 *page;
	long fd;

	write_text(start, sizeof(start) - 1);
	fd = open_device();
	if (fd < 0) {
		fail(open_failed, sizeof(open_failed) - 1);
	}
	page = map_device(fd, PTWFLAGS_MAP_SUPERVISOR, PROT_READ | PROT_EXEC);
	if ((long)page == -1) {
		fail(mmap_failed, sizeof(mmap_failed) - 1);
	}
	if (touch_supervisor(fd, page)) {
		fail(touch_failed, sizeof(touch_failed) - 1);
	}
	(void)execute_code(page);
	fail(exec_succeeded, sizeof(exec_succeeded) - 1);
}

void _start(void)
{
#if TEST_MODE == MODE_BASELINE
	run_baseline();
#elif TEST_MODE == MODE_SMC
	run_smc();
#elif TEST_MODE == MODE_VGA
	run_vga();
#elif TEST_MODE == MODE_MMIO
	run_mmio();
#elif TEST_MODE == MODE_SUPER_READ
	run_supervisor_read();
#elif TEST_MODE == MODE_SUPER_WRITE
	run_supervisor_write();
#elif TEST_MODE == MODE_SUPER_EXEC
	run_supervisor_exec();
#elif TEST_MODE == MODE_SUPER_WRITE_COLD
	run_supervisor_write_cold();
#elif TEST_MODE == MODE_READONLY_WRITE
	run_readonly_write();
#elif TEST_MODE == MODE_USER_PRESENT_READONLY_WRITE
	run_user_present_readonly_write();
#else
#error unsupported TEST_MODE
#endif
	poweroff_machine();
	exit_status(0);
}
