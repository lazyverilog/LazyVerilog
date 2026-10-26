// lazyverilog patch: the lock slang's SourceManager takes on every lookup.
//
// MinGW's libstdc++ builds std::shared_mutex on winpthreads' pthread_rwlock,
// which serializes on process-wide state: lock_shared on a mutex no other
// thread touches cost 157 / 673 / 3404 ns at 1 / 4 / 8 threads, against 29 /
// 48 / 75 ns for an SRWLOCK.  The indexer asks the SourceManager about every
// macro token, so a MinGW build indexed 7.6x slower than the MSVC one built
// from the same source.  Elsewhere this is std::shared_mutex unchanged.
#pragma once

#if defined(_WIN32) && defined(__GNUC__)
namespace slang {

/// An SRWLOCK, with out-of-line members so this header needs no <windows.h>.
/// Like std::shared_mutex it is not recursive.
class SourceManagerMutex {
public:
    SourceManagerMutex() = default;
    SourceManagerMutex(const SourceManagerMutex&) = delete;
    SourceManagerMutex& operator=(const SourceManagerMutex&) = delete;

    void lock();
    bool try_lock();
    void unlock();
    void lock_shared();
    bool try_lock_shared();
    void unlock_shared();

private:
    void* srw_ = nullptr; // SRWLOCK_INIT
};

} // namespace slang
#else
#include <shared_mutex>
namespace slang {
using SourceManagerMutex = std::shared_mutex;
} // namespace slang
#endif
