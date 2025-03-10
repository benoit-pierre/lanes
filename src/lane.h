#pragma once

#include "cancel.h"
#include "uniquekey.h"
#include "universe.h"

#include <chrono>
#include <condition_variable>
#include <latch>
#include <stop_token>
#include <string_view>
#include <thread>

// #################################################################################################

// xxh64 of string "kExtendedStackTraceRegKey" generated at https://www.pelock.com/products/hash-calculator
static constexpr RegistryUniqueKey kExtendedStackTraceRegKey{ 0x38147AD48FB426E2ull }; // used as registry key

/*
 * registry[FINALIZER_REG_KEY] is either nil (no finalizers) or a table
 * of functions that Lanes will call after the executing 'pcall' has ended.
 *
 * We're NOT using the GC system for finalizer mainly because providing the
 * error (and maybe stack trace) parameters to the finalizer functions would
 * anyways complicate that approach.
 */
// xxh64 of string "kFinalizerRegKey" generated at https://www.pelock.com/products/hash-calculator
static constexpr RegistryUniqueKey kFinalizerRegKey{ 0xFE936BFAA718FEEAull };

// xxh64 of string "kLaneGC" generated at https://www.pelock.com/products/hash-calculator
static constexpr UniqueKey kLaneGC{ 0x5D6122141727F960ull };

// xxh64 of string "kLanePointerRegKey" generated at https://www.pelock.com/products/hash-calculator
static constexpr RegistryUniqueKey kLanePointerRegKey{ 0x2D8CF03FE9F0A51Aull }; // used as registry key

    // xxh64 of string "debugName" generated at https://www.pelock.com/products/hash-calculator
static constexpr RegistryUniqueKey kLaneNameRegKey{ 0xA194E2645C57F6DDull };

// #################################################################################################

// The chain is ended by '(Lane*)(-1)', not nullptr: 'selfdestructFirst -> ... -> ... -> (-1)'
#define SELFDESTRUCT_END ((Lane*) (-1))

// must be a #define instead of a constexpr to work with lua_pushliteral (until I templatize it)
#define kLaneMetatableName "Lane"
#define kLanesLibName "lanes"
#define kLanesCoreLibName kLanesLibName ".core"

// NOTE: values to be changed by either thread, during execution, without locking, are marked "volatile"
class Lane
{
    public:
    /*
      Pending: The Lua VM hasn't done anything yet.
      Running, Waiting: Thread is inside the Lua VM. If the thread is forcefully stopped, we can't lua_close() the Lua State.
      Done, Error, Cancelled: Thread execution is outside the Lua VM. It can be lua_close()d.
    */
    enum class Status
    {
        Pending,
        Running,
        Waiting,
        Done,
        Error,
        Cancelled
    };
    using enum Status;

    enum class ErrorTraceLevel
    {
        Minimal, // no error handler function when running the lane body
        Basic, // lane body errors caught by a error handler
        Extended // same as above, but with more data extracted from the debug infos
    };
    using enum ErrorTraceLevel;

    // the thread
    std::jthread thread;
    // a latch to wait for the lua_State to be ready
    std::latch ready{ 1 };
    // to wait for stop requests through thread's stop_source
    std::mutex doneMutex;
    std::condition_variable doneCondVar; // use condition_variable_any if waiting for a stop_token
    //
    // M: sub-thread OS thread
    // S: not used

    std::string_view debugName{ "<unnamed>" };

    Universe* const U;
    lua_State* L;
    //
    // M: prepares the state, and reads results
    // S: while S is running, M must keep out of modifying the state

    Status volatile status{ Pending };
    //
    // M: sets to Pending (before launching)
    // S: updates -> Running/Waiting -> Done/Error/Cancelled

    std::condition_variable* volatile waiting_on{ nullptr };
    //
    // When status is Waiting, points on the linda's signal the thread waits on, else nullptr

    CancelRequest volatile cancelRequest{ CancelRequest::None };
    //
    // M: sets to false, flags true for cancel request
    // S: reads to see if cancel is requested

    Lane* volatile selfdestruct_next{ nullptr };
    //
    // M: sets to non-nullptr if facing lane handle '__gc' cycle but the lane
    //    is still running
    // S: cleans up after itself if non-nullptr at lane exit

    // For tracking only
    Lane* volatile tracking_next{ nullptr };

    ErrorTraceLevel const errorTraceLevel{ Basic };

    [[nodiscard]] static void* operator new(size_t size_, Universe* U_) noexcept { return U_->internalAllocator.alloc(size_); }
    // can't actually delete the operator because the compiler generates stack unwinding code that could call it in case of exception
    static void operator delete(void* p_, Universe* U_) { U_->internalAllocator.free(p_, sizeof(Lane)); }
    // this one is for us, to make sure memory is freed by the correct allocator
    static void operator delete(void* p_) { static_cast<Lane*>(p_)->U->internalAllocator.free(p_, sizeof(Lane)); }

    Lane(Universe* U_, lua_State* L_, ErrorTraceLevel errorTraceLevel_);
    ~Lane();

    void changeDebugName(int const nameIdx_);
    void close() { lua_State* _L{ L }; L = nullptr; lua_close(_L); }
    [[nodiscard]] std::string_view errorTraceLevelString() const;
    [[nodiscard]] int pushErrorHandler() const;
    [[nodiscard]] std::string_view pushErrorTraceLevel(lua_State* L_) const;
    static void PushMetatable(lua_State* L_);
    [[nodiscard]] std::string_view pushThreadStatus(lua_State* L_) const;
    void securizeDebugName(lua_State* L_);
    void startThread(int priority_);
    [[nodiscard]] std::string_view threadStatusString() const;
    [[nodiscard]] bool waitForCompletion(std::chrono::time_point<std::chrono::steady_clock> until_);
};

// #################################################################################################

// To allow free-running threads (longer lifespan than the handle's)
// 'Lane' are malloc/free'd and the handle only carries a pointer.
// This is not deep userdata since the handle's not portable among lanes.
//
[[nodiscard]] inline Lane* ToLane(lua_State* L_, int i_)
{
    return *(static_cast<Lane**>(luaL_checkudata(L_, i_, kLaneMetatableName)));
}
