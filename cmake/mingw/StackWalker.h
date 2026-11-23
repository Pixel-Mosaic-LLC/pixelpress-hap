#pragma once
// MinGW cross-builds only (scripts/build-win-cross.sh): StackWalker is MSVC-only. The foundation
// logging code uses it to add a call stack to crash logs; without it the crash is still logged,
// just without the stack.
#include <windows.h>
class StackWalker {
public:
    virtual ~StackWalker() {}
    BOOL ShowCallstack(HANDLE = GetCurrentThread(), const CONTEXT* = nullptr) { return FALSE; }
protected:
    virtual void OnOutput(LPCSTR) {}
};
