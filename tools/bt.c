#define _GNU_SOURCE
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>

static void handler(int sig) {
    static const char marker[] = "\nBT_MARKER sig=?\n";
    char mbuf[32];
    (void)marker;
    int mlen = snprintf(mbuf, sizeof mbuf, "\nBT_MARKER sig=%d\n", sig);
    write(2, mbuf, mlen);

    void *bt[64];
    int n = backtrace(bt, 64);
    for (int i = 0; i < n; i++) {
        Dl_info info;
        char buf[320];
        int len;
        if (dladdr(bt[i], &info) && info.dli_fname) {
            unsigned long off = 0;
            if (info.dli_fbase) {
                off = (unsigned long)((char *)bt[i] - (char *)info.dli_fbase);
            }
            len = snprintf(buf, sizeof buf, "  #%d %s(+0x%lx) [%p]\n", i, info.dli_fname, off,
                           bt[i]);
        } else {
            len = snprintf(buf, sizeof buf, "  #%d [%p]\n", i, bt[i]);
        }
        if (len > 0) write(2, buf, (size_t)len);
    }
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
    write(2, "BT_PRELOAD_ACTIVE\n", 18);
}
