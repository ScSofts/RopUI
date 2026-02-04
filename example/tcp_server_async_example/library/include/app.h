#pragma once

#include "asyncnet.h"
#include "server_runner.h"

// Business entry coroutine (keep this file clean).
asyncnet::Task<void> async_main(asyncnet::Executor& accept_exec,
                          ::RopHive::Hive& hive,
                          int worker_n,
                          std::shared_ptr<std::vector<std::shared_ptr<asyncnet::Executor>>> execs);

#define ROPHIVE_ASYNC_MAIN \
    int main(int argc, char** argv) { return ::tcp_server_async_example::run(argc, argv); }