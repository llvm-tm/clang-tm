#include <csignal>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>
#include "test_helpers.hpp"

// ── Reproducer: eager-read null-address dereference ─────────────────
//
// Problem: WT and TL2 eagerly dereference *addr at write time to
// capture the old value for the undo log.  When a TX function generates
// a GEP through a moved-from null pointer (e.g. null+4 during STL
// vector reallocation), the TM write receives an invalid address like
// 0x4 and crashes with SIGSEGV.
//
// SwissTM has a guard (< 0x100000) that prevents the crash but silently
// drops the write (correctness gap).
//
// WBCTL has a guard that prevents both crash and write — safe.
//
// NOrec has no eager-read in its write path, but its COMMIT path does
// write_value_to_addr directly — a write-back to a null address also
// crashes at commit time (then hangs because global_lock is stuck odd).
//
// Each test case runs in a forked child so crashes are isolated.

static sigjmp_buf g_recovery;
static volatile bool g_sigsegv = false;

static void sighandler(int) {
    g_sigsegv = true;
    siglongjmp(g_recovery, 1);
}

int main() {
    tm_init();

    const char* backend = "unknown";
#if defined(DESIGN_WT)
    backend = "TinySTM/WT";
#elif defined(DESIGN_WBCTL)
    backend = "TinySTM/WBCTL";
#elif defined(TM_BACKEND_TL2)
    backend = "TL2";
#elif defined(TM_BACKEND_NOREC)
    backend = "NOrec";
#elif defined(TM_BACKEND_SWISSTM)
    backend = "SwissTM";
#endif
    printf("Backend: %s\n\n", backend);
    printf("Testing tm_write_i4 to near-null addresses inside TX\n");
    printf("(simulating GEP through moved-from null pointer)\n\n");

    struct { const char* name; uint32_t* addr; } tests[] = {
        {"write to 0x0",        (uint32_t*)(uintptr_t)0x0},
        {"write to 0x4",        (uint32_t*)(uintptr_t)0x4},
        {"write to 0x8",        (uint32_t*)(uintptr_t)0x8},
        {"write to 0x10",       (uint32_t*)(uintptr_t)0x10},
        {"write to 0x100",      (uint32_t*)(uintptr_t)0x100},
        {"write to 0xFFFFFFE8", (uint32_t*)(uintptr_t)0xFFFFFFFFFFFFFFE8ULL},
    };
    int n = sizeof(tests) / sizeof(tests[0]);
    int crashes = 0;

    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            // ── Child process: run one test case ──
            signal(SIGSEGV, sighandler);
            signal(SIGBUS, sighandler);
            tm_init_thread();

            g_sigsegv = false;
            if (sigsetjmp(g_recovery, 1) == 0) {
                tm_nested_call_counter = 1;
                tm_begin();
                tm_write_i4(tests[i].addr, 42, 0);
                tm_end();
                _exit(g_sigsegv ? 2 : 0);
            } else {
                // SIGSEGV was caught via sighandler → siglongjmp
                _exit(2);
            }
        }

        // ── Parent: wait for child and report ──
        int status;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status)) {
            int rc = WEXITSTATUS(status);
            if (rc == 0) {
                printf("  %-18s PASS (no crash)\n", tests[i].name);
            } else {
                printf("  %-18s CRASH (SIGSEGV caught in handler)\n", tests[i].name);
                crashes++;
            }
        } else if (WIFSIGNALED(status)) {
            printf("  %-18s CRASH (signal %d)\n", tests[i].name, WTERMSIG(status));
            crashes++;
        } else {
            printf("  %-18s UNKNOWN (status=%d)\n", tests[i].name, status);
            crashes++;
        }
    }

    tm_exit();

    printf("\n%d/%d tests passed", n - crashes, n);
    if (crashes > 0) {
        printf(" — %d crash(es)\n", crashes);
        printf("FAIL: Backend %s crashes when writing to near-null addresses.\n", backend);
        if (crashes == n) {
            printf("No null-address guard exists in the write path.\n");
        }
        printf("The eager-read (read_value_from_addr / to_word(*addr)) at write time\n");
        printf("or write-back (write_value_to_addr) at commit time dereferences the\n");
        printf("invalid address.\n");
        return 1;
    }
    printf("\n");
    printf("PASS: Backend %s handles near-null addresses gracefully.\n", backend);
    return 0;
}
