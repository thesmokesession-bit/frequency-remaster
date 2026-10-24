#pragma once

#include_next <eekernel.h>

// An interrupt handler of the native build is an ordinary call, and it returns with nothing to
// restore.
#undef ExitHandler
#define ExitHandler() ((void)0)
