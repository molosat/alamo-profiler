#include "deferred_log.h"
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>

int main() {
    DeferredLog log;
    log.Push("session line");
    std::promise<void> entered, release;
    auto wait = release.get_future();
    std::thread writer([&] {
        log.Drain([&](const char*) { entered.set_value(); wait.wait(); });
    });
    entered.get_future().wait();
    auto producer = std::async(std::launch::async, [&] { log.Push("frame mark"); });
    const bool unblocked = producer.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
    release.set_value();
    producer.get();
    writer.join();
    for (int i = 0; i < 100; ++i) log.Push("overflow");
    const bool ok = unblocked && log.Dropped() > 0;
    std::puts(ok ? "PASS: deferred session log" : "FAIL: deferred session log");
    return ok ? 0 : 1;
}
