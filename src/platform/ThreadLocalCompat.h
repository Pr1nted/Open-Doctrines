#ifndef OD_PLATFORM_THREAD_LOCAL_COMPAT_H
#define OD_PLATFORM_THREAD_LOCAL_COMPAT_H

// A per-thread value that works on the PS Vita.
//
// `thread_local` is unusable there: the compiler emits an ARM TLS relocation
// (R_ARM_*TLS*, which vita-elf-create reports as "Invalid relocation type 25")
// and the .velf never links. The Vita's loader has no TLS, so there is nothing
// for that relocation to resolve against. Every other target keeps real
// `thread_local`; only the Vita routes through a pthread key, which is an
// ordinary function call and emits no TLS relocation.
//
// Use it only where `thread_local` already earned its keep -- a genuinely
// per-thread singleton. `odtls::PerThread<T>` is meant to be a function-local
// `static`, constructed once; `.get()` returns this thread's T, default-built
// on first touch and destroyed when the thread exits (the pthread key's
// destructor). Off the Vita the header is empty and the call sites use
// `thread_local` directly behind the same `#if`.

#if defined(__vita__)

#include <pthread.h>

namespace odtls {

template <class T>
class PerThread {
public:
    PerThread() { pthread_key_create(&key_, &PerThread::destroy_); }
    T& get() {
        void* p = pthread_getspecific(key_);
        if (!p) {
            p = new T();
            pthread_setspecific(key_, p);
        }
        return *static_cast<T*>(p);
    }

private:
    static void destroy_(void* p) { delete static_cast<T*>(p); }
    pthread_key_t key_;
};

}  // namespace odtls

#endif  // __vita__

#endif  // OD_PLATFORM_THREAD_LOCAL_COMPAT_H
