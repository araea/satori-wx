// Minimal arm64 ptrace injector: remote-calls dlopen(path, RTLD_NOW) in a target's main thread.
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <link.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dlfcn.h>

static int die(const char *m) {
    perror(m);
    return 1;
}

// Find the lowest mapping base of a module (path substring) in a pid.
static uintptr_t module_base(pid_t pid, const char *needle) {
    char p[64];
    snprintf(p, sizeof p, "/proc/%d/maps", pid);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strstr(line, needle)) continue;
        uintptr_t lo;
        if (sscanf(line, "%" SCNxPTR "-", &lo) == 1 && (!base || lo < base)) base = lo;
    }
    fclose(f);
    return base;
}

static int get_regs(pid_t tid, struct user_pt_regs *r) {
    struct iovec io = {r, sizeof *r};
    return ptrace(PTRACE_GETREGSET, tid, (void *)NT_PRSTATUS, &io);
}
static int set_regs(pid_t tid, struct user_pt_regs *r) {
    struct iovec io = {r, sizeof *r};
    return ptrace(PTRACE_SETREGSET, tid, (void *)NT_PRSTATUS, &io);
}
static int remote_write(pid_t pid, uintptr_t addr, const void *buf, size_t n) {
    struct iovec l = {(void *)buf, n}, r = {(void *)addr, n};
    return process_vm_writev(pid, &l, 1, &r, 1, 0) == (ssize_t)n ? 0 : -1;
}

// Calls fn(a0,a1,a2) in the stopped thread, returns x0 via *ret.
static int remote_call(pid_t tid, uintptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t *ret) {
    struct user_pt_regs saved, r;
    int saved_nr = -1;
    struct iovec snr = {&saved_nr, sizeof saved_nr};
    if (get_regs(tid, &saved)) return die("getregs");
    ptrace(PTRACE_GETREGSET, tid, (void *)NT_ARM_SYSTEM_CALL, &snr);
    r = saved;
    r.regs[0] = a0;
    r.regs[1] = a1;
    r.regs[2] = a2;
    r.regs[30] = 0x1000; // return to an unmapped address -> SIGSEGV
    r.sp = (saved.sp - 0x400) & ~0xfULL;
    r.pc = fn;
    // Cancel any in-progress syscall restart.
    int nr = -1;
    struct iovec sc = {&nr, sizeof nr};
    ptrace(PTRACE_SETREGSET, tid, (void *)NT_ARM_SYSTEM_CALL, &sc);
    if (set_regs(tid, &r)) return die("setregs");
    if (ptrace(PTRACE_CONT, tid, 0, 0)) return die("cont");
    int st;
    for (;;) {
        if (waitpid(tid, &st, __WALL) < 0) return die("waitpid");
        if (WIFSTOPPED(st) && WSTOPSIG(st) == SIGSEGV) break;
        if (WIFSTOPPED(st)) {
            ptrace(PTRACE_CONT, tid, 0, WSTOPSIG(st) == SIGSTOP ? 0 : WSTOPSIG(st));
            continue;
        }
        fprintf(stderr, "target exited: %d\n", st);
        return 1;
    }
    struct user_pt_regs after;
    get_regs(tid, &after);
    if (after.pc != 0x1000) fprintf(stderr, "warning: stopped at pc=%llx (fault)\n", (unsigned long long)after.pc);
    *ret = after.regs[0];
    // restore
    set_regs(tid, &saved);
    struct iovec rnr = {&saved_nr, sizeof saved_nr};
    ptrace(PTRACE_SETREGSET, tid, (void *)NT_ARM_SYSTEM_CALL, &rnr);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <pid> <so path> [symbol-to-call]\n", argv[0]);
        return 2;
    }
    pid_t pid = atoi(argv[1]);
    const char *so = argv[2];
    // Resolve dlopen in the target: local libdl offset + remote libdl base.
    void *h = dlopen("libdl.so", RTLD_NOW);
    void *local_dlopen = dlsym(RTLD_DEFAULT, "dlopen");
    Dl_info info;
    if (!dladdr(local_dlopen, &info)) return die("dladdr");
    uintptr_t lbase = (uintptr_t)info.dli_fbase;
    fprintf(stderr, "local dlopen=%p in %s base=%p\n", local_dlopen, info.dli_fname, info.dli_fbase);
    uintptr_t rbase = module_base(pid, strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') : info.dli_fname);
    fprintf(stderr, "remote base of %s = %lx\n", info.dli_fname, (unsigned long)rbase);
    if (!rbase) {
        fprintf(stderr, "no libdl in target\n");
        return 1;
    }
    uintptr_t rdlopen = rbase + ((uintptr_t)local_dlopen - lbase);
    (void)h;
    if (ptrace(PTRACE_ATTACH, pid, 0, 0)) return die("attach");
    int st;
    waitpid(pid, &st, __WALL);
    if (!WIFSTOPPED(st)) {
        fprintf(stderr, "not stopped\n");
        return 1;
    }
    struct user_pt_regs regs;
    get_regs(pid, &regs);
    uintptr_t buf = ((regs.sp - 0x1000) & ~0xfULL);
    if (remote_write(pid, buf, so, strlen(so) + 1)) {
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return die("write");
    }
    uint64_t ret = 0;
    int rc = remote_call(pid, rdlopen, buf, 2 /*RTLD_NOW*/, 0, &ret);
    fprintf(stderr, "dlopen returned %llx rc=%d\n", (unsigned long long)ret, rc);
    if (!ret && !rc) {
        uintptr_t rdlerror = rbase + ((uintptr_t)dlsym(RTLD_DEFAULT, "dlerror") - lbase);
        uint64_t e = 0;
        if (!remote_call(pid, rdlerror, 0, 0, 0, &e) && e) {
            char msg[400] = {0};
            struct iovec l = {msg, sizeof msg - 1}, r = {(void *)e, sizeof msg - 1};
            process_vm_readv(pid, &l, 1, &r, 1, 0);
            fprintf(stderr, "dlerror: %s\n", msg);
        }
    }
    ptrace(PTRACE_DETACH, pid, 0, 0);
    return rc || !ret;
}
