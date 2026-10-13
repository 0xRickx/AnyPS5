#include "prx/libc/include/general/VabiMacros.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <stdexcept>
#include <thread>

using Callback = int (APS5_VABI *)(void*, void*, void**);
extern "C" int APS5_VABI _ZSt13_Execute_onceRSt9once_flagPFiPvS1_PS1_ES1__nid_postfix(int*, Callback, void*);
static int ExecuteOnce(int* flag, Callback callback, void* arg) {
    return _ZSt13_Execute_onceRSt9once_flagPFiPvS1_PS1_ES1__nid_postfix(flag, callback, arg);
}
static void Require(bool value) { if (!value) std::abort(); }

static std::atomic<int> calls{0}, arrived{0};
static int control = 0, nested = 0, failing = 0, throwing = 0;
static int published = 0, nestedValue = 0, failures = 0, throws = 0;
static void* seenFirst = &control;
static void* seenArg = nullptr;
static void** seenThird = reinterpret_cast<void**>(&control);

static int APS5_VABI Nested(void*, void* arg, void**) {
    nestedValue = *static_cast<int*>(arg);
    return 1;
}
static int APS5_VABI Initialize(void* first, void* arg, void** third) {
    ++calls;
    seenFirst = first;
    seenArg = arg;
    seenThird = third;
    while (arrived.load() != 16) std::this_thread::yield();
    int value = 17;
    Require(ExecuteOnce(&nested, Nested, &value) == 1);
    published = 42;
    return 5;
}
static int APS5_VABI Fail(void*, void*, void**) {
    ++failures;
    return 0;
}
static int APS5_VABI Throw(void*, void*, void**) {
    if (++throws == 1) throw 7;
    return 1;
}
static int APS5_VABI Unreachable(void*, void*, void**) {
    std::abort();
}

static int waiting = 0, awaited = 0, reentrant = 0, contested = 0;
static std::atomic<int> entered{0}, attempts{0};
static int APS5_VABI Succeed(void*, void*, void**) {
    return 1;
}
static int APS5_VABI AwaitOtherFlag(void*, void*, void**) {
    auto other = std::async(std::launch::async, [] { return ExecuteOnce(&awaited, Succeed, nullptr); });
    Require(other.wait_for(std::chrono::seconds(5)) == std::future_status::ready && other.get() == 1);
    return 1;
}
static int APS5_VABI Reenter(void*, void*, void**) {
    ExecuteOnce(&reentrant, Unreachable, nullptr);
    std::abort();
}
static int APS5_VABI ThrowFirst(void*, void*, void**) {
    if (++attempts == 1) {
        while (entered.load() != 8) std::this_thread::yield();
        throw 9;
    }
    return 1;
}

int main() {
    int token = 0;
    std::array<std::thread, 16> workers;
    for (auto& worker : workers) worker = std::thread([&token] {
        ++arrived;
        Require(ExecuteOnce(&control, Initialize, &token) == 1);
        Require(published == 42 && nestedValue == 17);
    });
    for (auto& worker : workers) worker.join();
    Require(calls == 1 && control == 1 && nested == 1);
    Require(seenFirst == nullptr && seenArg == &token && seenThird == nullptr);
    Require(ExecuteOnce(&control, Unreachable, nullptr) == 1 && calls == 1);

    Require(ExecuteOnce(&failing, Fail, nullptr) == 0 && failing == 0 && failures == 1);
    Require(ExecuteOnce(&failing, Fail, nullptr) == 0 && failing == 0 && failures == 2);

    try { ExecuteOnce(&throwing, Throw, nullptr); std::abort(); }
    catch (int value) { Require(value == 7 && throwing == 0); }
    Require(ExecuteOnce(&throwing, Throw, nullptr) == 1 && throwing == 1 && throws == 2);

    int preset = -3;
    Require(ExecuteOnce(&preset, Unreachable, nullptr) == 1 && preset == -3);

    Require(ExecuteOnce(&waiting, AwaitOtherFlag, nullptr) == 1 && waiting == 1 && awaited == 1);

    try { ExecuteOnce(&reentrant, Reenter, nullptr); std::abort(); }
    catch (const std::runtime_error&) { Require(reentrant == 0); }
    Require(ExecuteOnce(&reentrant, Succeed, nullptr) == 1 && reentrant == 1);

    std::atomic<int> thrown{0}, returned{0};
    std::array<std::thread, 8> contenders;
    for (auto& contender : contenders) contender = std::thread([&thrown, &returned] {
        ++entered;
        try { Require(ExecuteOnce(&contested, ThrowFirst, nullptr) == 1); ++returned; }
        catch (int value) { Require(value == 9); ++thrown; }
    });
    for (auto& contender : contenders) contender.join();
    Require(thrown == 1 && returned == 7 && attempts == 2 && contested == 1);
}
