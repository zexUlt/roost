#include "core/tsan.hpp"

#if defined(__SANITIZE_THREAD__)
#define ROOST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define ROOST_TSAN 1
#endif
#endif

#if defined(ROOST_TSAN)
#include <sanitizer/tsan_interface.h>
#endif

namespace roost {

void TsanRelease([[maybe_unused]] const void* address) {
#if defined(ROOST_TSAN)
    __tsan_release(const_cast<void*>(address));
#endif
}

void TsanAcquire([[maybe_unused]] const void* address) {
#if defined(ROOST_TSAN)
    __tsan_acquire(const_cast<void*>(address));
#endif
}

}  // namespace roost
