// A stand-in for a host process's own C++ runtime: replaces global operator new/delete and counts the calls. Linked into the test before libstdc++, it takes
// over every allocation of code that binds to the shared runtime; a layer with its own static runtime must never reach it.
#include <cstddef>
#include <cstdlib>
#include <new>

static unsigned long g_new_calls = 0;
extern "C" unsigned long hoststd_new_calls() { return __atomic_load_n(&g_new_calls, __ATOMIC_RELAXED); }

static void* Alloc(std::size_t n, std::size_t align = 0) {
    __atomic_fetch_add(&g_new_calls, 1, __ATOMIC_RELAXED);
    void* p = nullptr;
    if (align > alignof(std::max_align_t)) { if (posix_memalign(&p, align, n ? n : 1) != 0) p = nullptr; }
    else p = std::malloc(n ? n : 1);
    return p;
}
void* operator new(std::size_t n) { if (void* p = Alloc(n)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { if (void* p = Alloc(n)) return p; throw std::bad_alloc(); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return Alloc(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return Alloc(n); }
void* operator new(std::size_t n, std::align_val_t a) { if (void* p = Alloc(n, std::size_t(a))) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n, std::align_val_t a) { if (void* p = Alloc(n, std::size_t(a))) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
