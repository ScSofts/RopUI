#pragma once

#include "asyncnet.h"

// Business entry coroutine (keep this file clean).
asyncnet::Task<void> async_main(asyncnet::Executor& accept_exec,
                          ::RopHive::Hive& hive,
                          int worker_n,
                          std::shared_ptr<std::vector<std::shared_ptr<asyncnet::Executor>>> execs);

#define rophive_async_main int main(int argc, char** argv) \
    { return ::tcp_server_async_example::run(argc, argv); } \
    asyncnet::Task<void>