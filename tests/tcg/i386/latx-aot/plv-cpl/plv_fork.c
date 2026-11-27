#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t signal_seen;

static void handle_signal(int signo)
{
    signal_seen = signo;
}

static void fail(const char *stage)
{
    printf("PLV_FORK_FAIL=%s errno=%d\n", stage, errno);
    exit(1);
}

static void wait_child(pid_t pid, const char *stage)
{
    int status;
    pid_t waited;

    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail(stage);
    }
}

static void test_fork_exit(void)
{
    pid_t pid;

    puts("PLV_FORK_STAGE=exit-child");
    pid = fork();
    if (pid < 0) {
        fail("fork-exit");
    }
    if (pid == 0) {
        _exit(0);
    }
    wait_child(pid, "wait-exit");
    puts("PLV_FORK_EXIT=PASS");
}

static void test_fork_exec(const char *path, const char *stage)
{
    pid_t pid;

    printf("PLV_FORK_STAGE=%s\n", stage);
    pid = fork();
    if (pid < 0) {
        fail(stage);
    }
    if (pid == 0) {
        execl(path, path, (char *)NULL);
        _exit(127);
    }
    wait_child(pid, stage);
}

static void test_signal(void)
{
    struct sigaction action = {
        .sa_handler = handle_signal,
    };
    pid_t pid;

    puts("PLV_FORK_STAGE=signal");
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGUSR1, &action, NULL) != 0) {
        fail("sigaction");
    }
    pid = fork();
    if (pid < 0) {
        fail("fork-signal");
    }
    if (pid == 0) {
        if (kill(getppid(), SIGUSR1) != 0) {
            _exit(1);
        }
        _exit(0);
    }
    wait_child(pid, "wait-signal");
    if (signal_seen != SIGUSR1) {
        fail("signal-not-seen");
    }
    puts("PLV_FORK_SIGNAL=PASS");
}

static void test_bash(void)
{
    pid_t pid;

    puts("PLV_FORK_STAGE=bash-exec");
    pid = fork();
    if (pid < 0) {
        fail("fork-bash");
    }
    if (pid == 0) {
        execl("/bin/bash", "bash", "--noprofile", "--norc",
              "-c", "printf 'PLV_FORK_BASH=PASS\\n'", (char *)NULL);
        _exit(127);
    }
    wait_child(pid, "wait-bash");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("PLV_FORK_STAGE=start");
    test_fork_exit();
    test_fork_exec("/bin/true", "exec-true");
    puts("PLV_FORK_EXEC=PASS");
    test_signal();
    test_bash();
    puts("PLV_FORK_RESULT=PASS");
    return 0;
}
