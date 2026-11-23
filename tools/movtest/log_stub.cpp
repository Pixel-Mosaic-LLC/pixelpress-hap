// Linux-only test stub for the foundation logger (real one is Windows/macOS specific)
#include <cstdio>
#include "logging.hpp"
namespace fdn {
CodeLocation::CodeLocation(const std::string& f, size_t l) : file(f), line(l), handle(0) {}
void logMessage(const CodeLocation& c, LogMessageSeverity s, const std::string& m) {
    if (s >= LogMessageSeverity::Warning) fprintf(stderr, "[log %d] %s", (int)s, m.c_str());
}
void nameThread(const std::string&) {}
void fatalError(const std::string& m) { fprintf(stderr, "FATAL %s\n", m.c_str()); }
}
