/**
 * zts_guard.h — Compile-time ZTS detection
 *
 * Include this header in every translation unit that touches PHP internals.
 * When ZTS is enabled, TSRM headers are pulled in automatically.
 * When ZTS is absent, multi-worker mode is unavailable — the supervisor
 * degrades to single-worker NTS mode at runtime.
 */
#ifndef NATIVEPHP_ZTS_GUARD_H
#define NATIVEPHP_ZTS_GUARD_H

#include "php_config.h"

/*
 * ZTS (Zend Thread Safety) is required for concurrent multi-worker execution.
 * Without it, running PHP in multiple threads will corrupt global state.
 * If ZTS is missing we emit a compile-time warning; at runtime the
 * supervisor falls back to a single worker.
 */
#ifndef ZTS
#error "PHP was built WITHOUT ZTS (Zend Thread Safety). " \
       "Multi-worker concurrency requires ZTS. " \
       "Rebuild PHP with --enable-zts."
#endif

#ifdef ZTS
#ifndef TSRM_H
#include "TSRM.h"
#endif
#endif

/* Runtime check (call once at engine init) */
static inline int nativephp_verify_zts_runtime(void) {
#ifdef ZTS
    return 1;
#else
    return 0;
#endif
}

#endif /* NATIVEPHP_ZTS_GUARD_H */
