#include "init.h"

namespace {
    volatile std::sig_atomic_t g_stop = 0;
    void onSigint(int) { g_stop = 1; }
}

void init() {
    // Init spdlog e SIGINT
    spdlog::set_pattern("[%H:%M:%S] [%^%l%$] %v");
    spdlog::flush_on(spdlog::level::trace);                        
    std::signal(SIGINT, onSigint);
}

bool stopRequested(void) { 
  return g_stop != 0; 
}