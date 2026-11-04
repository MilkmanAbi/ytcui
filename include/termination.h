#pragma once
#include <csignal>
namespace ytui { inline volatile std::sig_atomic_t termination_requested = 0; }
