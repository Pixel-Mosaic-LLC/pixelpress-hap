#pragma once

#include <functional>

// Runs task(0) .. task(count-1) across a process-wide pool of worker threads (one per core,
// shared by every frame being encoded, so concurrent frames never oversubscribe the CPU).
// The calling thread works too and returns when all tasks are done.
void parallelFor(int count, const std::function<void(int)>& task);

// Number of worker threads (including the caller) parallelFor spreads work over.
int parallelConcurrency();
