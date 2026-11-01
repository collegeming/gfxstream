#define _GNU_SOURCE
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <string.h>

static void handler(int sig) {
    void *bt[64];
    char msg[] = "\n=== BACKTRACE ===\n";
    int n = backtrace(bt, 64);
    write(2, msg, sizeof(msg) - 1);
    backtrace_symbols_fd(bt, n, 2);
    signal(sig, SIG_DFL);
    raise(sig);
}

__attribute__((constructor)) static void bt_init(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGABRT, &sa, 0);
    sigaction(SIGBUS, &sa, 0);
}
