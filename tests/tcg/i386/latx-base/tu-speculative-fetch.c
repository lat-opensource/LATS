/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 * SPDX-License-Identifier: GPL-2.0-only
 */

/*
 * Run in an x86-64 Linux guest with JIT TU and
 * -latx select,aotsys=0,tumode=jit,tucpopt=off,tucp=7
 * to exercise cross-page exploration with TU page tracking disabled.
 */
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <unistd.h>

enum { PAGE = 4096, ENTRY = 256 };
static sigjmp_buf fault_return;
static volatile sig_atomic_t expected_fault;
static void *volatile fault_address;

static void fault_handler(int sig, siginfo_t *info, void *context)
{
    (void)sig;
    (void)context;
    fault_address = info->si_addr;
    siglongjmp(fault_return, 1);
}

static int check_hole(int (*branch)(int), const void *target)
{
    if (sigsetjmp(fault_return, 1)) {
        if (!expected_fault || fault_address != target) {
            printf("TU_FETCH premature_or_wrong_fault addr=%p\n",
                   (void *)fault_address);
            return 1;
        }
        puts("TU_FETCH taken_branch_fault");
    } else {
        if (branch(0) != 17) {
            return 1;
        }
        puts("TU_FETCH untaken_branch_ok");
        expected_fault = 1;
        branch(1);
        puts("TU_FETCH missing_taken_branch_fault");
        return 1;
    }

    return 0;
}

static int run_tests(void)
{
    const unsigned char branch[] = {
        0x85, 0xff,                         /* test %edi,%edi */
        0x0f, 0x85, 0xf8, 0x0f, 0x00, 0x00, /* jnz entry + PAGE */
        0xb8, 17, 0, 0, 0, 0xc3,            /* mov $17,%eax; ret */
    };
    const unsigned char target[] = {0xb8, 29, 0, 0, 0, 0xc3};
    unsigned char *pages = mmap((void *)UINT64_C(0x4000000000), 6 * PAGE,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    memcpy(pages + ENTRY, branch, sizeof(branch));
    memcpy(pages + 2 * PAGE + ENTRY, branch, sizeof(branch));
    memcpy(pages + 3 * PAGE + ENTRY, target, sizeof(target));
    /* A Jcc on page four targets a mov that starts two bytes before its end. */
    unsigned char *split_branch = pages + 4 * PAGE + ENTRY;
    int32_t displacement = (pages + 5 * PAGE - 2) - (split_branch + 8);
    memcpy(split_branch, branch, sizeof(branch));
    memcpy(split_branch + 4, &displacement, sizeof(displacement));
    /* The first instruction spans the last two pages. */
    memcpy(pages + 5 * PAGE - 2, target, sizeof(target));
    if (mprotect(pages, 6 * PAGE, PROT_READ | PROT_EXEC) ||
        munmap(pages + PAGE, PAGE)) {
        perror("prepare code pages");
        return 1;
    }
    struct sigaction action = {.sa_sigaction = fault_handler,
                               .sa_flags = SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, NULL)) {
        perror("sigaction");
        return 1;
    }

    printf("TU_TEST_BASE=%p\n", (void *)pages);
    int (*branch_to_hole)(int) = (void *)(pages + ENTRY);
    int (*branch_to_code)(int) = (void *)(pages + 2 * PAGE + ENTRY);
    int (*branch_to_split)(int) = (void *)split_branch;
    int (*split_insn)(void) = (void *)(pages + 5 * PAGE - 2);
    int hole_result = check_hole(branch_to_hole, pages + PAGE + ENTRY);

    if (sigsetjmp(fault_return, 1)) {
        printf("TU_FETCH wrong_control_fault addr=%p\n", (void *)fault_address);
        return 1;
    }
    /* A different fault during the executable-page controls is unexpected. */
    expected_fault = 0;
    if (branch_to_code(0) != 17 || branch_to_code(1) != 29 ||
        branch_to_split(0) != 17 || branch_to_split(1) != 29 ||
        split_insn() != 29) {
        return 1;
    }
    puts("TU_FETCH mapped_and_split_ok");
    return hole_result;
}

int main(void)
{
    int result = run_tests();

    printf("TU_FETCH %s\n", result ? "FAIL" : "PASS");
    fflush(stdout);
    if (getpid() == 1) {
        reboot(RB_POWER_OFF);
    }
    return result;
}
