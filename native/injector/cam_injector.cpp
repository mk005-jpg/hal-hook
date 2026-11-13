// cam_injector — ptrace-based dlopen injector for arm64 Android.
// Usage: cam_injector <process-name> <lib-path>
// Example (as root): cam_injector cameraserver /data/local/tmp/hal_inject/libcam_hook.so
//
// Technique:
//   1. Find target PID via /proc/*/cmdline
//   2. PTRACE_ATTACH
//   3. Locate dlopen in target process address space (via /proc/PID/maps + our own base)
//   4. Write library path string to target stack space via /proc/PID/mem
//   5. Redirect PC to dlopen, wait for SIGSEGV (return to NULL lr triggers it)
//   6. PTRACE_DETACH
//
// No Frida strings. No Frida runtime. Detection-clean.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <linux/elf.h>   // NT_PRSTATUS

// arm64 user_pt_regs layout (matches struct user_pt_regs in asm/ptrace.h)
struct arm64_regs {
    uint64_t regs[31]; // x0-x30 (x30 = link register)
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
};

// ─── Helpers ──────────────────────────────────────────────────────────────

static pid_t find_pid(const char *proc_name) {
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *e;
    pid_t found = -1;
    while ((e = readdir(d)) && found == -1) {
        if (e->d_type != DT_DIR) continue;
        char *end;
        long n = strtol(e->d_name, &end, 10);
        if (*end) continue;

        char path[64];
        snprintf(path, sizeof(path), "/proc/%ld/cmdline", n);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        char buf[256] = {};
        read(fd, buf, sizeof(buf) - 1);
        close(fd);

        // cmdline is NUL-delimited; first token = process name or full path
        const char *bn = strrchr(buf, '/');
        bn = bn ? bn + 1 : buf;
        if (strcmp(bn,  proc_name) == 0 ||
            strcmp(buf, proc_name) == 0) found = (pid_t)n;
    }
    closedir(d);
    return found;
}

// First executable mapping of a library in the given PID's address space.
// Matches the basename EXACTLY — a loose substring match is a trap: "libdl"
// also matches libdl_android.so (mapped at a different base), which corrupts the
// base/offset arithmetic and makes the remote dlopen land on the wrong function.
static uintptr_t get_lib_base(pid_t pid, const char *lib_name) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f) && base == 0) {
        char perms[8], fname[256] = {};
        uint64_t start;
        if (sscanf(line, "%lx-%*lx %4s %*x %*x:%*x %*u %255s",
                   &start, perms, fname) >= 2) {
            const char *bn = strrchr(fname, '/');
            bn = bn ? bn + 1 : fname;
            if (strcmp(bn, lib_name) == 0 && perms[2] == 'x')
                base = (uintptr_t)start;
        }
    }
    fclose(f);
    return base;
}

static bool ptrace_get_regs(pid_t pid, arm64_regs *r) {
    struct iovec iov = { r, sizeof(*r) };
    return ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) == 0;
}

static bool ptrace_set_regs(pid_t pid, const arm64_regs *r) {
    struct iovec iov = { (void *)r, sizeof(*r) };
    return ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov) == 0;
}

// Write bytes directly into target process via /proc/PID/mem.
static bool remote_write_mem(pid_t pid, uintptr_t addr,
                              const void *data, size_t len) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open /proc/PID/mem"); return false; }
    ssize_t r = pwrite64(fd, data, len, (off64_t)addr);
    close(fd);
    return r == (ssize_t)len;
}

// Execute dlopen in target via ptrace: set up call frame, CONT, wait for fault.
// Returns the handle value (x0 on return), or 0 on failure.
//
// caller_lr MUST point inside a library already loaded in the target that lives
// in the DEFAULT linker namespace (e.g. libcameraservice.so — NOT libc, which is
// in the runtime APEX namespace). Android's libdl dlopen passes
// __builtin_return_address(0) — i.e. the LR we set here — to the linker as the
// "caller address", and the linker resolves the caller's linker NAMESPACE from
// it. With LR=0 the linker finds no caller and the load fails; anchoring LR in a
// default-namespace lib selects the namespace whose permitted_paths include /data.
static uintptr_t remote_dlopen(pid_t pid, uintptr_t dlopen_addr,
                                uintptr_t str_addr, uintptr_t caller_lr) {
    arm64_regs saved, call;
    if (!ptrace_get_regs(pid, &saved)) {
        perror("GETREGSET"); return 0;
    }

    // Plant a BRK #0 at caller_lr so the return from dlopen traps
    // DETERMINISTICALLY. Relying on the bytes at the return address to be an
    // illegal instruction is unreliable — if they decode to something runnable
    // the target keeps executing and the tracer hangs (and holds cameraserver
    // under ptrace). caller_lr lies inside the target's libc, so it doubles as
    // the dlopen caller address for linker-namespace resolution. POKETEXT does a
    // private copy-on-write of the page, so other processes are unaffected.
    errno = 0;
    long orig = ptrace(PTRACE_PEEKTEXT, pid, (void *)caller_lr, 0);
    if (orig == -1 && errno) { perror("PEEKTEXT"); return 0; }
    long with_brk = (orig & ~0xFFFFFFFFL) | 0xD4200000L;   // BRK #0 in low 32 bits
    if (ptrace(PTRACE_POKETEXT, pid, (void *)caller_lr, (void *)with_brk) != 0) {
        perror("POKETEXT"); return 0;
    }

    call            = saved;
    call.sp         = (saved.sp - 0x8000) & ~15ULL;  // reserve scratch below the path string
    call.regs[0]    = str_addr;      // arg0: path
    call.regs[1]    = 2;             // arg1: RTLD_NOW
    call.regs[30]   = caller_lr;     // LR = namespace anchor + breakpoint
    call.pc         = dlopen_addr;
    if (!ptrace_set_regs(pid, &call)) {
        perror("SETREGSET");
        ptrace(PTRACE_POKETEXT, pid, (void *)caller_lr, (void *)orig);
        return 0;
    }

    ptrace(PTRACE_CONT, pid, NULL, NULL);

    uintptr_t retval = 0;
    int status;
    while (waitpid(pid, &status, 0) > 0) {
        if (!WIFSTOPPED(status)) break;
        int sig = WSTOPSIG(status);
        arm64_regs ret;
        ptrace_get_regs(pid, &ret);
        if (sig == SIGTRAP) {              // our breakpoint: dlopen has returned
            fprintf(stderr, "[dbg] TRAP pc=0x%lx x0=0x%lx (anchor=0x%lx)\n",
                    (unsigned long)ret.pc, (unsigned long)ret.regs[0],
                    (unsigned long)caller_lr);
            if (ret.pc == caller_lr || ret.pc == caller_lr + 4)
                retval = ret.regs[0];      // x0 = dlopen handle (or NULL)
            break;
        }
        if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL) {
            fprintf(stderr, "[dbg] FAULT sig=%d pc=0x%lx x0=0x%lx\n",
                    sig, (unsigned long)ret.pc, (unsigned long)ret.regs[0]);
            break;                         // dlopen faulted internally → failure
        }
        ptrace(PTRACE_CONT, pid, NULL, (void *)(uintptr_t)sig);
    }

    // Restore the clobbered instruction and the original register state.
    ptrace(PTRACE_POKETEXT, pid, (void *)caller_lr, (void *)orig);
    ptrace_set_regs(pid, &saved);
    return retval;
}

// ─── Main ─────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
                "Usage: cam_injector <process-name> <lib-path>\n"
                "  e.g. cam_injector cameraserver "
                "/data/local/tmp/hal_inject/libcam_hook.so\n");
        return 1;
    }

    const char *proc  = argv[1];
    const char *lib   = argv[2];
    size_t      libsz = strlen(lib) + 1;

    printf("[*] Looking for process: %s\n", proc);
    pid_t target = find_pid(proc);
    if (target < 0) { fprintf(stderr, "[-] Not found\n"); return 1; }
    printf("[+] PID: %d\n", target);

    // Find dlopen in our process, then calculate its address in the target.
    // Both processes use the same linker / libdl.so binary; only the load base differs.
    void *local_dlopen = dlsym(RTLD_DEFAULT, "dlopen");
    if (!local_dlopen) {
        // Android 10+: dlopen may be an alias in linker64
        void *h = dlopen("libdl.so", RTLD_NOW | RTLD_NOLOAD);
        local_dlopen = h ? dlsym(h, "dlopen") : nullptr;
    }
    if (!local_dlopen) { fprintf(stderr, "[-] Cannot find dlopen\n"); return 1; }

    // Determine which lib provides dlopen in our process
    const char *dlopen_libs[] = { "libdl.so", "linker64", nullptr };
    uintptr_t local_base = 0, remote_base = 0;
    for (int i = 0; dlopen_libs[i] && !local_base; i++) {
        local_base  = get_lib_base(getpid(), dlopen_libs[i]);
        remote_base = get_lib_base(target,   dlopen_libs[i]);
    }
    if (!local_base || !remote_base) {
        fprintf(stderr, "[-] Cannot locate dlopen provider in maps\n");
        return 1;
    }

    uintptr_t remote_dlopen_addr =
        remote_base + ((uintptr_t)local_dlopen - local_base);
    printf("[+] dlopen remote: 0x%lx\n", (unsigned long)remote_dlopen_addr);

    // Attach
    if (ptrace(PTRACE_ATTACH, target, NULL, NULL) != 0) {
        perror("[-] PTRACE_ATTACH"); return 1;
    }
    int status;
    waitpid(target, &status, 0);
    printf("[+] Attached\n");

    // Read SP so we can write the library path just below it
    arm64_regs regs;
    if (!ptrace_get_regs(target, &regs)) {
        perror("[-] GETREGSET");
        ptrace(PTRACE_DETACH, target, NULL, NULL);
        return 1;
    }

    // Place the path string well above the hijack SP (which we move 0x8000 down),
    // so dlopen's own frames — realpath uses a PATH_MAX buffer — can't clobber it.
    uintptr_t str_addr = (regs.sp - 0x4000) & ~15ULL;
    printf("[*] Writing lib path to 0x%lx\n", (unsigned long)str_addr);

    if (!remote_write_mem(target, str_addr, lib, libsz)) {
        ptrace(PTRACE_DETACH, target, NULL, NULL);
        fprintf(stderr, "[-] remote_write_mem failed\n");
        return 1;
    }

    // Anchor the dlopen caller address inside a target library that lives in the
    // DEFAULT linker namespace — the one whose permitted_paths include /data.
    // libc.so is the WRONG choice: on Android 10+ it lives in the runtime APEX
    // namespace, whose permitted_paths are APEX-only, so a dlopen attributed to
    // libc cannot load from /data. libcameraservice.so (/system/lib64) is in the
    // default namespace; fall back to the cameraserver executable, then libdl.
    uintptr_t caller_lr = get_lib_base(target, "libcameraservice.so");
    if (!caller_lr) caller_lr = get_lib_base(target, "cameraserver");   // main exe
    if (!caller_lr) caller_lr = remote_base;
    printf("[*] namespace anchor: 0x%lx\n", (unsigned long)caller_lr);

    // Execute dlopen in target
    printf("[*] Calling dlopen(\"%s\", RTLD_NOW) in target...\n", lib);
    uintptr_t handle = remote_dlopen(target, remote_dlopen_addr, str_addr, caller_lr);

    ptrace(PTRACE_DETACH, target, NULL, NULL);
    printf("[+] Detached\n");

    if (handle == 0) {
        fprintf(stderr, "[-] dlopen returned NULL — check logcat for details\n");
        return 1;
    }
    printf("[+] Library loaded, handle=0x%lx\n", (unsigned long)handle);
    return 0;
}
