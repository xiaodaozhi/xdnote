#pragma once
namespace xdnote {
// GUI-thread admission gate. Already running work finishes normally. The update
// handoff polls the global pool only after all mutable document work is idle.
inline bool &updateTaskGate() { static bool blocked = false; return blocked; }
inline bool updateTasksBlocked() { return updateTaskGate(); }
}
