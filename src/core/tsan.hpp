#pragma once

namespace roost {

// These state an ordering that Qt guarantees but ThreadSanitizer cannot see,
// because Qt is not instrumented. Use them only where
// .claude/rules/threads.md allows, with the Qt guarantee named in a comment.
// Outside a ThreadSanitizer build they do nothing.
//
// Everything the releasing thread wrote before TsanRelease(address) is
// visible to a thread that calls TsanAcquire(address) afterwards.
void TsanRelease(const void* address);
void TsanAcquire(const void* address);

}  // namespace roost
