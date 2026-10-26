// lazyverilog patch: see SourceManagerMutex.h.
#include "slang/text/SourceManagerMutex.h"

#if defined(_WIN32) && defined(__GNUC__)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace slang {

static_assert(sizeof(SRWLOCK) == sizeof(void*));

static PSRWLOCK srw(void*& storage) {
    return reinterpret_cast<PSRWLOCK>(&storage);
}

void SourceManagerMutex::lock() { AcquireSRWLockExclusive(srw(srw_)); }
bool SourceManagerMutex::try_lock() { return TryAcquireSRWLockExclusive(srw(srw_)) != 0; }
void SourceManagerMutex::unlock() { ReleaseSRWLockExclusive(srw(srw_)); }
void SourceManagerMutex::lock_shared() { AcquireSRWLockShared(srw(srw_)); }
bool SourceManagerMutex::try_lock_shared() { return TryAcquireSRWLockShared(srw(srw_)) != 0; }
void SourceManagerMutex::unlock_shared() { ReleaseSRWLockShared(srw(srw_)); }

} // namespace slang
#endif
