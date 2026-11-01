// The pthread subset libstdc++ needs on the Nintendo 3DS.
//
// devkitARM ships a dummy libpthread, so libstdc++'s gthread layer decides threads are inactive:
// every __gthread_mutex_lock/unlock becomes a no-op and condition-variable waits fall through to
// an unresolved symbol. The core and the frontend synchronize with std::mutex,
// std::condition_variable and std::call_once, so this file supplies those pthread entry points over
// libctru's LightLock/RecursiveLock and a semaphore-backed condition variable. Defining the
// active proxy (pthread_cancel) is what makes libstdc++ take this path at all.
#if defined(__3DS__)

#include <3ds.h>

#include <pthread.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <new>
#include <sys/lock.h>

namespace {

// libctru's lock layouts are what newlib's _LOCK_T/_LOCK_RECURSIVE_T mirror, so the pthread types
// can hold them directly.
static_assert(sizeof(LightLock) == sizeof(_LOCK_T));
static_assert(sizeof(RecursiveLock) == sizeof(_LOCK_RECURSIVE_T));

LightLock g_table_lock = {0};
LightLock g_once_lock = {0};

constexpr uint32_t COND_MAGIC = 0x434F4E44u; // 'COND'
constexpr int SEMAPHORE_MAX = 0x7FFFFFFF;

struct CondState {
    uint32_t magic = COND_MAGIC;
    Handle semaphore = 0;
    int waiters = 0;
};

CondState *cond_state(pthread_cond_t *cond) {
    if (!cond->cond) return nullptr;
    auto *state = reinterpret_cast<CondState *>(static_cast<uintptr_t>(cond->cond));
    if (state->magic != COND_MAGIC || !state->semaphore) return nullptr;
    return state;
}

// Condition variables are created lazily: namespace-scope objects construct them during static
// initialization, before the kernel services a semaphore needs are necessarily up.
CondState *ensure_state(pthread_cond_t *cond) {
    if (CondState *state = cond_state(cond)) return state;
    LightLock_Lock(&g_table_lock);
    CondState *state = cond_state(cond);
    if (!state) {
        auto *fresh = new (std::nothrow) CondState;
        if (fresh && R_FAILED(svcCreateSemaphore(&fresh->semaphore, 0, SEMAPHORE_MAX))) {
            delete fresh;
            fresh = nullptr;
        }
        if (fresh) {
            cond->cond = static_cast<_COND_T>(reinterpret_cast<uintptr_t>(fresh));
            state = fresh;
        }
    }
    LightLock_Unlock(&g_table_lock);
    return state;
}

// Thread-specific data: libstdc++ keeps a few internals per thread. There is no pthread key
// storage in libctru, so a small table keyed by (key, thread) stands in for it.
constexpr unsigned MAX_KEYS = 16;
constexpr unsigned MAX_KEY_THREADS = 8;
struct TlsEntry {
    bool used = false;
    pthread_key_t key = 0;
    Thread thread = nullptr;
    void *value = nullptr;
};
TlsEntry g_tls[MAX_KEYS * MAX_KEY_THREADS];

} // namespace

extern "C" {

// The frontend installs this to log a mutex that stays contended: a stalled lock is otherwise
// indistinguishable from a slow emulated frame.
void (*f3rt_pthread_stall)(const void *lock, unsigned long long waited_us) = nullptr;

namespace {

// Blocks on a light lock without spinning the core, and reports a stall that outlives a second.
void lock_slow(LightLock *lock) {
    const u64 start = svcGetSystemTick();
    u64 last_report = start;
    for (;;) {
        svcSleepThread(20000); // 20 us
        if (LightLock_TryLock(lock) == 0) return;
        if (!f3rt_pthread_stall) continue;
        const u64 now = svcGetSystemTick();
        if (now - last_report < SYSCLOCK_ARM11) continue;
        last_report = now;
        f3rt_pthread_stall(lock, (now - start) * 1000000ull / SYSCLOCK_ARM11);
    }
}

inline void lock_light(LightLock *lock) {
    if (LightLock_TryLock(lock) == 0) return;
    lock_slow(lock);
}

} // namespace

// libstdc++'s __gthread_active_p() tests this symbol's address; it is never called.
int pthread_cancel(pthread_t) { return 0; }

int pthread_once(pthread_once_t *once, void (*init)(void)) {
    if (!once || !init) return EINVAL;
    if (__atomic_load_n(&once->status, __ATOMIC_ACQUIRE) == 2) return 0;
    LightLock_Lock(&g_once_lock);
    if (once->status != 2) {
        if (once->status == 0) {
            once->status = 1;
            LightLock_Unlock(&g_once_lock);
            init();
            LightLock_Lock(&g_once_lock);
            once->status = 2;
        }
    }
    LightLock_Unlock(&g_once_lock);
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    if (!mutex) return EINVAL;
    mutex->type = PTHREAD_MUTEX_NORMAL;
#if defined(_UNIX98_THREAD_MUTEX_ATTRIBUTES)
    if (attr) mutex->type = attr->type;
#endif
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE) {
        RecursiveLock_Init(reinterpret_cast<RecursiveLock *>(&mutex->recursive));
    } else {
        mutex->type = PTHREAD_MUTEX_NORMAL;
        LightLock_Init(reinterpret_cast<LightLock *>(&mutex->normal));
    }
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    mutex->type = PTHREAD_MUTEX_NORMAL;
    LightLock_Init(reinterpret_cast<LightLock *>(&mutex->normal));
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE) {
        RecursiveLock_Lock(reinterpret_cast<RecursiveLock *>(&mutex->recursive));
    } else {
        lock_light(reinterpret_cast<LightLock *>(&mutex->normal));
    }
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE) {
        if (RecursiveLock_TryLock(reinterpret_cast<RecursiveLock *>(&mutex->recursive)))
            return EBUSY;
    } else {
        if (LightLock_TryLock(reinterpret_cast<LightLock *>(&mutex->normal))) return EBUSY;
    }
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE) {
        RecursiveLock_Unlock(reinterpret_cast<RecursiveLock *>(&mutex->recursive));
    } else {
        LightLock_Unlock(reinterpret_cast<LightLock *>(&mutex->normal));
    }
    return 0;
}

#if defined(_UNIX98_THREAD_MUTEX_ATTRIBUTES)
int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
    if (!attr) return EINVAL;
    std::memset(attr, 0, sizeof(*attr));
    attr->type = PTHREAD_MUTEX_NORMAL;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) { return attr ? 0 : EINVAL; }

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) {
    if (!attr) return EINVAL;
    attr->type = type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type) {
    if (!attr || !type) return EINVAL;
    *type = attr->type;
    return 0;
}
#endif

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *) {
    if (!cond) return EINVAL;
    cond->clock_id = CLOCK_REALTIME;
    cond->cond = 0;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond) {
    if (!cond) return EINVAL;
    auto *state = cond_state(cond);
    if (state) {
        state->magic = 0;
        svcCloseHandle(state->semaphore);
        delete state;
    }
    cond->cond = 0;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    CondState *state = cond ? ensure_state(cond) : nullptr;
    if (!state || !mutex) return EINVAL;
    __atomic_add_fetch(&state->waiters, 1, __ATOMIC_ACQ_REL);
    pthread_mutex_unlock(mutex);
    const Result result = svcWaitSynchronization(state->semaphore, U64_MAX);
    __atomic_sub_fetch(&state->waiters, 1, __ATOMIC_ACQ_REL);
    pthread_mutex_lock(mutex);
    return R_SUCCEEDED(result) ? 0 : EINVAL;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *abstime) {
    CondState *state = cond ? ensure_state(cond) : nullptr;
    if (!state || !mutex || !abstime) return EINVAL;
    struct timespec now {};
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t remaining_ns = int64_t(abstime->tv_sec - now.tv_sec) * 1000000000ll +
                           int64_t(abstime->tv_nsec - now.tv_nsec);
    if (remaining_ns < 0) remaining_ns = 0;
    __atomic_add_fetch(&state->waiters, 1, __ATOMIC_ACQ_REL);
    pthread_mutex_unlock(mutex);
    const Result result = svcWaitSynchronization(state->semaphore, s64(remaining_ns));
    __atomic_sub_fetch(&state->waiters, 1, __ATOMIC_ACQ_REL);
    pthread_mutex_lock(mutex);
    if (R_SUCCEEDED(result)) return 0;
    return ETIMEDOUT;
}

int pthread_cond_signal(pthread_cond_t *cond) {
    CondState *state = cond ? ensure_state(cond) : nullptr;
    if (!state) return EINVAL;
    // Predicates are changed under the mutex, so a waiter that has not counted itself yet cannot
    // have missed this notification; a count left behind by a waiter that woke on a timeout is
    // consumed as a harmless spurious wakeup.
    if (__atomic_load_n(&state->waiters, __ATOMIC_ACQUIRE) > 0) {
        // The syscall stub stores the released count through the first argument.
        s32 released = 0;
        svcReleaseSemaphore(&released, state->semaphore, 1);
    }
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
    CondState *state = cond ? ensure_state(cond) : nullptr;
    if (!state) return EINVAL;
    const int waiters = __atomic_load_n(&state->waiters, __ATOMIC_ACQUIRE);
    if (waiters > 0) {
        s32 released = 0;
        svcReleaseSemaphore(&released, state->semaphore, waiters);
    }
    return 0;
}

int pthread_key_create(pthread_key_t *key, void (*)(void *)) {
    if (!key) return EINVAL;
    LightLock_Lock(&g_table_lock);
    bool used[MAX_KEYS] = {};
    for (const auto &entry : g_tls)
        if (entry.used && entry.key < MAX_KEYS) used[entry.key] = true;
    for (unsigned candidate = 0; candidate < MAX_KEYS; ++candidate) {
        if (used[candidate]) continue;
        *key = candidate;
        LightLock_Unlock(&g_table_lock);
        return 0;
    }
    LightLock_Unlock(&g_table_lock);
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key) {
    LightLock_Lock(&g_table_lock);
    for (auto &entry : g_tls)
        if (entry.used && entry.key == key) entry = TlsEntry{};
    LightLock_Unlock(&g_table_lock);
    return 0;
}

void *pthread_getspecific(pthread_key_t key) {
    const Thread self = threadGetCurrent();
    void *value = nullptr;
    LightLock_Lock(&g_table_lock);
    for (const auto &entry : g_tls)
        if (entry.used && entry.key == key && entry.thread == self) {
            value = entry.value;
            break;
        }
    LightLock_Unlock(&g_table_lock);
    return value;
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    const Thread self = threadGetCurrent();
    LightLock_Lock(&g_table_lock);
    TlsEntry *free_entry = nullptr;
    for (auto &entry : g_tls) {
        if (entry.used && entry.key == key && entry.thread == self) {
            entry.value = const_cast<void *>(value);
            LightLock_Unlock(&g_table_lock);
            return 0;
        }
        if (!entry.used && !free_entry) free_entry = &entry;
    }
    if (!free_entry) {
        LightLock_Unlock(&g_table_lock);
        return EAGAIN;
    }
    free_entry->used = true;
    free_entry->key = key;
    free_entry->thread = self;
    free_entry->value = const_cast<void *>(value);
    LightLock_Unlock(&g_table_lock);
    return 0;
}

} // extern "C"

#endif // __3DS__
