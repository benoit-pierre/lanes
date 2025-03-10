/*
 --
 -- KEEPER.CPP
 --
 -- Keeper state logic
 --
 -- This code is read in for each "keeper state", which are the hidden, inter-
 -- mediate data stores used by Lanes inter-state communication objects.
 --
 -- Author: Benoit Germain <bnt.germain@gmail.com>
 --
 -- C implementation replacement of the original keeper.lua
 --
 --[[
 ===============================================================================

 Copyright (C) 2011-2024 Benoit Germain <bnt.germain@gmail.com>

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.

 ===============================================================================
 ]]--
 */
#include "keeper.h"

#include "intercopycontext.h"
#include "lane.h"
#include "linda.h"
#include "state.h"

#include <algorithm>
#include <cassert>
#include <ranges>

// There is a table at _R[kLindasRegKey] (aka LindasDB)
// This table contains entries of the form [Linda*] = {KeysDB...}
// Each KeysDB contains entries of the form [key] = KeyUD
// where key is a key used in the Lua Linda API to exchange data, and KeyUD is a full userdata with a table uservalue
// the table uservalue is the actual fifo, where elements are added and removed.

// #################################################################################################
// #################################################################################################
// ############################################ KeyUD ##############################################
// #################################################################################################
// #################################################################################################

// the full userdata associated to a given Linda key to store its contents
class KeyUD
{
    public:
    int first{ 1 };
    int count{ 0 };
    int limit{ -1 };

    // a fifo full userdata has one uservalue, the table that holds the actual fifo contents
    [[nodiscard]] static void* operator new([[maybe_unused]] size_t size_, KeeperState L_) noexcept { return lua_newuserdatauv<KeyUD>(L_, 1); }
    // always embedded somewhere else or "in-place constructed" as a full userdata
    // can't actually delete the operator because the compiler generates stack unwinding code that could call it in case of exception
    static void operator delete([[maybe_unused]] void* p_, [[maybe_unused]] KeeperState L_) { LUA_ASSERT(L_, !"should never be called"); }

    [[nodiscard]] static KeyUD* GetPtr(KeeperState K_, int idx_);
    [[nodiscard]] static KeyUD* Create(KeeperState K_);
    [[nodiscard]] static KeyUD* PrepareAccess(KeeperState K_, int idx_);
    void peek(KeeperState K_, int count_);
    void pop(KeeperState K_, int count_);
    void push(KeeperState K_, int count_);
};

static constexpr int kContentsTableIndex{ 1 };

// #################################################################################################

// in: nothing
// out: { first = 1, count = 0, limit = -1}
KeyUD* KeyUD::Create(KeeperState const K_)
{
    STACK_GROW(K_, 2);
    STACK_CHECK_START_REL(K_, 0);
    KeyUD* const _key{ new (K_) KeyUD{} };
    STACK_CHECK(K_, 1);
    lua_newtable(K_);
    lua_setiuservalue(K_, -2, kContentsTableIndex);
    STACK_CHECK(K_, 1);
    return _key;
}

// #################################################################################################

KeyUD* KeyUD::GetPtr(KeeperState const K_, int idx_)
{
    return lua_tofulluserdata<KeyUD>(K_, idx_);
}


// #################################################################################################

// replaces the fifo ud by its uservalue on the stack
KeyUD* KeyUD::PrepareAccess(KeeperState const K_, int const idx_)
{
    KeyUD* const _key{ KeyUD::GetPtr(K_, idx_) };
    if (_key) {
        int const _idx{ lua_absindex(K_, idx_) };
        STACK_GROW(K_, 1);
        // we can replace the key userdata in the stack without fear of it being GCed, there are other references around
        lua_getiuservalue(K_, _idx, kContentsTableIndex);
        lua_replace(K_, _idx);
    }
    return _key;
}

// #################################################################################################

// in: fifo
// out: ...|nothing
// expects exactly 1 value on the stack!
// currently only called with a count of 1, but this may change in the future
// function assumes that there is enough data in the fifo to satisfy the request
void KeyUD::peek(KeeperState const K_, int const count_)
{
    STACK_GROW(K_, count_);
    for (int const _i : std::ranges::iota_view{ 0, count_ }) {
        lua_rawgeti(K_, 1, first + _i);
    }
}

// #################################################################################################

// in: fifo
// out: remove the fifo from the stack, push as many items as required on the stack (function assumes they exist in sufficient number)
void KeyUD::pop(KeeperState const K_, int const count_)
{
    LUA_ASSERT(K_, lua_istable(K_, -1));
    int const _fifo_idx{ lua_gettop(K_) };                                                         // K_: ... fifotbl
    // each iteration pushes a value on the stack!
    STACK_GROW(K_, count_ + 2);
    // skip first item, we will push it last
    for (int const _i : std::ranges::iota_view{ 1, count_ }) {
        int const _at{ first + _i };
        // push item on the stack
        lua_rawgeti(K_, _fifo_idx, _at);                                                           // K_: ... fifotbl val
        // remove item from the fifo
        lua_pushnil(K_);                                                                           // K_: ... fifotbl val nil
        lua_rawseti(K_, _fifo_idx, _at);                                                           // K_: ... fifotbl val
    }
    // now process first item
    {
        int const _at{ first };
        lua_rawgeti(K_, _fifo_idx, _at);                                                           // K_: ... fifotbl vals val
        lua_pushnil(K_);                                                                           // K_: ... fifotbl vals val nil
        lua_rawseti(K_, _fifo_idx, _at);                                                           // K_: ... fifotbl vals val
        lua_replace(K_, _fifo_idx);                                                                // K_: ... vals
    }

    // avoid ever-growing indexes by resetting each time we detect the fifo is empty
    {
        int const _new_count{ count - count_ };
        first = (_new_count == 0) ? 1 : (first + count_);
        count = _new_count;
    }
}

// #################################################################################################

// in: expect fifo args... on top of the stack
// out: nothing, removes all pushed values from the stack
void KeyUD::push(KeeperState const K_, int const count_)
{
    int const _idx{ lua_gettop(K_) - count_ };
    int const _start{ first + count - 1 };
    // pop all additional arguments, storing them in the fifo
    for (int const _i : std::ranges::reverse_view{ std::ranges::iota_view{ 1, count_ + 1 } }) {
        // store in the fifo the value at the top of the stack at the specified index, popping it from the stack
        lua_rawseti(K_, _idx, _start + _i);
    }
    count += count_;
}

// #################################################################################################
// #################################################################################################

// in: linda_ud expected at stack slot idx
// out: the KeysDB table of the linda is pushed at the top of the stack
// xxh64 of string "kLindasRegKey" generated at https://www.pelock.com/products/hash-calculator
static constexpr RegistryUniqueKey kLindasRegKey{ 0x3AE0D5243A88B962ull };
static void PushKeysDB(KeeperState const K_, int const idx_)
{
    STACK_GROW(K_, 5);
    STACK_CHECK_START_REL(K_, 0);
    int const _idx{ lua_absindex(K_, idx_) };
    kLindasRegKey.pushValue(K_);                                                                   // K_: ... LindasDB
    lua_pushvalue(K_, _idx);                                                                       // K_: ... LindasDB linda
    lua_rawget(K_, -2);                                                                            // K_: ... LindasDB KeysDB
    STACK_CHECK(K_, 2);
    if (lua_isnil(K_, -1)) {
        lua_pop(K_, 1);                                                                            // K_: ... LindasDB
        // add a new KeysDB table for this linda
        lua_newtable(K_);                                                                          // K_: ... LindasDB KeysDB
        lua_pushvalue(K_, _idx);                                                                   // K_: ... LindasDB KeysDB linda
        lua_pushvalue(K_, -2);                                                                     // K_: ... LindasDB KeysDB linda KeysDB
        lua_rawset(K_, -4);                                                                        // K_: ... LindasDB KeysDB
    }
    lua_remove(K_, -2);                                                                            // K_: ... KeysDB
    STACK_CHECK(K_, 1);
}

// #################################################################################################

// only used by linda:dump() and linda:__towatch() for debugging purposes
int keeper_push_linda_storage(Linda& linda_, DestState L_)
{
    Keeper* const _keeper{ linda_.whichKeeper() };
    KeeperState const _K{ _keeper ? _keeper->L : nullptr };
    if (_K == nullptr) {
        return 0;
    }
    STACK_GROW(_K, 4);
    STACK_CHECK_START_REL(_K, 0);
    kLindasRegKey.pushValue(_K);                                                                   // _K: LindasDB                                       L_:
    lua_pushlightuserdata(_K, &linda_);                                                            // _K: LindasDB linda                                 L_:
    lua_rawget(_K, -2);                                                                            // _K: LindasDB KeysDB                                L_:
    lua_remove(_K, -2);                                                                            // _K: KeysDB                                         L_:
    if (!lua_istable(_K, -1)) { // possible if we didn't send anything through that linda
        lua_pop(_K, 1);                                                                            // _K:                                                L_:
        STACK_CHECK(_K, 0);
        return 0;
    }
    // move data from keeper to destination state
    STACK_GROW(L_, 5);
    STACK_CHECK_START_REL(L_, 0);
    lua_newtable(L_);                                                                              // _K: KeysDB                                         L_: out
    InterCopyContext _c{ linda_.U, L_, SourceState{ _K }, {}, {}, {}, LookupMode::FromKeeper, {} };
    lua_pushnil(_K);                                                                               // _K: KeysDB nil                                     L_: out
    while (lua_next(_K, -2)) {                                                                     // _K: KeysDB key KeyUD                               L_: out
        KeyUD* const _keyUD{ KeyUD::PrepareAccess(_K, -1) };                                       // _K: KeysDB key fifo                                L_: out
        lua_pushvalue(_K, -2);                                                                     // _K: KeysDB key fifo key                            L_: out
        std::ignore = _c.inter_move(1);                                                            // _K: KeysDB key fifo                                L_: out key
        STACK_CHECK(L_, 2);
        lua_newtable(L_);                                                                          // _K: KeysDB key fifo                                L_: out key keyout
        std::ignore = _c.inter_move(1);                                                            // _K: KeysDB key                                     L_: out key keyout fifo
        // keyout.first
        lua_pushinteger(L_, _keyUD->first);                                                        // _K: KeysDB key                                     L_: out key keyout fifo first
        STACK_CHECK(L_, 5);
        lua_setfield(L_, -3, "first");                                                             // _K: KeysDB key                                     L_: out key keyout fifo
        // keyout.count
        lua_pushinteger(L_, _keyUD->count);                                                        // _K: KeysDB key                                     L_: out key keyout fifo count
        STACK_CHECK(L_, 5);
        lua_setfield(L_, -3, "count");                                                             // _K: KeysDB key                                     L_: out key keyout fifo
        // keyout.limit
        lua_pushinteger(L_, _keyUD->limit);                                                        // _K: KeysDB key                                     L_: out key keyout fifo limit
        STACK_CHECK(L_, 5);
        lua_setfield(L_, -3, "limit");                                                             // _K: KeysDB key                                     L_: out key keyout fifo
        // keyout.fifo
        lua_setfield(L_, -2, "fifo");                                                              // _K: KeysDB key                                     L_: out key keyout
        // out[key] = keyout
        lua_rawset(L_, -3);                                                                        // _K: KeysDB key                                     L_: out
        STACK_CHECK(L_, 1);
    }                                                                                              // _K: KeysDB                                         L_: out
    STACK_CHECK(L_, 1);
    lua_pop(_K, 1);                                                                                // _K:                                                L_: out
    STACK_CHECK(_K, 0);
    return 1;
}

// #################################################################################################
// #################################################################################################
// ######################################## keepercall_XXX #########################################
// #################################################################################################
// #################################################################################################

// in: linda
int keepercall_clear(lua_State* const L_)
{
    STACK_GROW(L_, 3);
    STACK_CHECK_START_REL(L_, 0);
    // LindasDB[linda] = nil
    kLindasRegKey.pushValue(L_);                                                                   // L_: linda LindasDB
    lua_pushvalue(L_, 1);                                                                          // L_: linda LindasDB linda
    lua_pushnil(L_);                                                                               // L_: linda LindasDB linda nil
    lua_rawset(L_, -3);                                                                            // L_: linda LindasDB
    lua_pop(L_, 1);                                                                                // L_: linda
    STACK_CHECK(L_, 0);
    return 0;
}

// #################################################################################################

// in: linda_ud, key, ...
// out: true|false
int keepercall_send(lua_State* const L_)
{
    KeeperState const _K{ L_ };
    int const _n{ lua_gettop(_K) - 2 };
    STACK_CHECK_START_REL(_K, 0);
    PushKeysDB(_K, 1);                                                                             // _K: linda key args... KeysDB
    // get the fifo associated to this key in this linda, create it if it doesn't exist
    lua_pushvalue(_K, 2);                                                                          // _K: linda key args... KeysDB key
    lua_rawget(_K, -2);                                                                            // _K: linda key args... KeysDB KeyUD|nil
    if (lua_isnil(_K, -1)) {
        lua_pop(_K, 1);                                                                            // _K: linda key args... KeysDB
        std::ignore = KeyUD::Create(KeeperState{ _K });                                            // _K: linda key args... KeysDB KeyUD
        // KeysDB[key] = KeyUD
        lua_pushvalue(_K, 2);                                                                      // _K: linda key args... KeysDB KeyUD key
        lua_pushvalue(_K, -2);                                                                     // _K: linda key args... KeysDB KeyUD key KeyUD
        lua_rawset(_K, -4);                                                                        // _K: linda key args... KeysDB KeyUD
    }
    lua_remove(_K, -2);                                                                            // _K: linda key args... KeyUD
    STACK_CHECK(_K, 1);
    KeyUD* _key{ KeyUD::GetPtr(_K, -1) };
    if (_key->limit >= 0 && _key->count + _n > _key->limit) { // not enough room?
        // don't send anything
        lua_settop(_K, 0);                                                                         // _K:
        lua_pushboolean(_K, 0);                                                                    // _K: false
    } else {
        // _key should remain unchanged
        _key = KeyUD::PrepareAccess(_K, -1);                                                       // _K: linda key args... fifo
        lua_replace(_K, 2);                                                                        // _K: linda fifo args...
        _key->push(_K, _n);                                                                        // _K: linda fifo
        lua_settop(_K, 0);                                                                         // _K:
        lua_pushboolean(_K, 1);                                                                    // _K: true
    }
    return 1;
}

// #################################################################################################

// in: linda, key [, key]?
// out: (key, val) or nothing
int keepercall_receive(lua_State* const L_)
{
    KeeperState const _K{ L_ };
    int const _top{ lua_gettop(_K) };
    PushKeysDB(_K, 1);                                                                             // _K: linda keys... KeysDB
    lua_replace(_K, 1);                                                                            // _K: KeysDB keys...
    
    for (int const _i : std::ranges::iota_view{ 2, _top + 1 }) {
        lua_pushvalue(_K, _i);                                                                     // _K: KeysDB keys... key[i]
        lua_rawget(_K, 1);                                                                         // _K: KeysDB keys... KeyUD
        KeyUD* const _key{ KeyUD::PrepareAccess(_K, -1) };                                         // _K: KeysDB keys... fifo
        if (_key != nullptr && _key->count > 0) {
            _key->pop(_K, 1);                                                                      // _K: KeysDB keys... val
            if (!lua_isnil(_K, -1)) {
                lua_replace(_K, 1);                                                                // _K: val keys...
                lua_settop(_K, _i);                                                                // _K: val keys... key[i]
                if (_i != 2) {
                    lua_replace(_K, 2);                                                            // _K: val key keys...
                    lua_settop(_K, 2);                                                             // _K: val key
                }
                lua_insert(_K, 1);                                                                 // _K: key, val
                return 2;
            }
        }
        lua_settop(_K, _top);                                                                      // _K: data keys...
    }
    // nothing to receive
    return 0;
}

// #################################################################################################

// in: linda key mincount [maxcount]
int keepercall_receive_batched(lua_State* const L_)
{
    KeeperState const _K{ L_ };
    int const _min_count{ static_cast<int>(lua_tointeger(_K, 3)) };
    if (_min_count > 0) {
        int const _max_count{ static_cast<int>(luaL_optinteger(_K, 4, _min_count)) };
        lua_settop(_K, 2);                                                                         // _K: linda key
        lua_insert(_K, 1);                                                                         // _K: key linda
        PushKeysDB(_K, 2);                                                                         // _K: key linda KeysDB
        lua_remove(_K, 2);                                                                         // _K: key KeysDB
        lua_pushvalue(_K, 1);                                                                      // _K: key KeysDB key
        lua_rawget(_K, 2);                                                                         // _K: key KeysDB KeyUD
        lua_remove(_K, 2);                                                                         // _K: key KeyUD
        KeyUD* const _key{ KeyUD::PrepareAccess(_K, 2) };                                          // _K: key fifo
        if (_key != nullptr && _key->count >= _min_count) {
            _key->pop(_K, std::min(_max_count, _key->count));                                      // _K: key val...
        } else {
            lua_settop(_K, 0);                                                                     // _K:
        }
        // return whatever remains on the stack at that point
        return lua_gettop(_K);
    } else {
        return 0;
    }
}

// #################################################################################################

// in: linda_ud key [n|nil]
// out: true or nil
int keepercall_limit(lua_State* const L_)
{
    KeeperState const _K{ L_ };
    int const _limit{ static_cast<int>(luaL_optinteger(_K, 3, -1)) }; // -1 if we read nil because the argument is absent
    lua_settop(_K, 2);                                                                             // _K: linda key
    PushKeysDB(_K, 1);                                                                             // _K: linda key KeysDB
    lua_replace(_K, 1);                                                                            // _K: KeysDB key
    lua_pushvalue(_K, -1);                                                                         // _K: KeysDB key key
    lua_rawget(_K, -3);                                                                            // _K: KeysDB key KeyUD|nil
    KeyUD* _key{ KeyUD::GetPtr(_K, -1) };
    if (_key == nullptr) {                                                                         // _K: KeysDB key nil
        lua_pop(_K, 1);                                                                            // _K: KeysDB key
        _key = KeyUD::Create(_K);                                                                  // _K: KeysDB key fifo
        lua_rawset(_K, -3);                                                                        // _K: KeysDB
    }
    // remove any clutter on the stack
    lua_settop(_K, 0);                                                                             // _K:
    // return true if we decide that blocked threads waiting to write on that key should be awakened
    // this is the case if we detect the key was full but it is no longer the case
    if (
        ((_key->limit >= 0) && (_key->count >= _key->limit)) // the key was full if limited and count exceeded the previous limit
        && ((_limit < 0) || (_key->count < _limit))          // the key is not full if unlimited or count is lower than the new limit
    ) {
        lua_pushboolean(_K, 1);                                                                    // _K: true
    }
    // set the new limit
    _key->limit = _limit;
    // return 0 or 1 value
    return lua_gettop(_K);
}

// #################################################################################################

// in: linda key [val...]
// out: true if the linda was full but it's no longer the case, else nothing
int keepercall_set(lua_State* const L_)
{
    KeeperState const _K{ L_ };
    bool _should_wake_writers{ false };
    STACK_GROW(_K, 6);

    // retrieve fifos associated with the linda
    PushKeysDB(_K, 1);                                                                             // _K: linda key val... KeysDB
    lua_replace(_K, 1);                                                                            // _K: KeysDB key val...

    // make sure we have a value on the stack
    if (lua_gettop(_K) == 2) {                                                                     // _K: KeysDB key
        lua_pushvalue(_K, -1);                                                                     // _K: KeysDB key key
        lua_rawget(_K, 1);                                                                         // _K: KeysDB key KeyUD|nil
        // empty the KeyUD for the specified key: replace uservalue with a virgin table, reset counters, but leave limit unchanged!
        KeyUD* const _key{ KeyUD::GetPtr(_K, -1) };
        if (_key != nullptr) { // might be nullptr if we set a nonexistent key to nil              // _K: KeysDB key KeyUD
            if (_key->limit < 0) { // KeyUD limit value is the default (unlimited): we can totally remove it
                lua_pop(_K, 1);                                                                    // _K: KeysDB key
                lua_pushnil(_K);                                                                   // _K: KeysDB key nil
                lua_rawset(_K, -3);                                                                // _K: KeysDB
            } else {
                // we create room if the KeyUD was full but it is no longer the case
                _should_wake_writers = (_key->limit > 0) && (_key->count >= _key->limit);
                lua_remove(_K, -2);                                                                // _K: KeysDB KeyUD
                lua_newtable(_K);                                                                  // _K: KeysDB KeyUD {}
                lua_setiuservalue(_K, -2, kContentsTableIndex);                                    // _K: KeysDB KeyUD
                _key->first = 1;
                _key->count = 0;
            }
        }
    } else { // set/replace contents stored at the specified key?
        int const _count{ lua_gettop(_K) - 2 };                                                    // number of items we want to store
        lua_pushvalue(_K, 2);                                                                      // _K: KeysDB key val... key
        lua_rawget(_K, 1);                                                                         // _K: KeysDB key val... KeyUD|nil
        KeyUD* _key{ KeyUD::GetPtr(_K, -1) };
        if (_key == nullptr) { // can be nullptr if we store a value at a new key                  // KeysDB key val... nil
            // no need to wake writers in that case, because a writer can't wait on an inexistent key
            lua_pop(_K, 1);                                                                        // _K: KeysDB key val...
            std::ignore = KeyUD::Create(KeeperState{ _K });                                        // _K: KeysDB key val... KeyUD
            lua_pushvalue(_K, 2);                                                                  // _K: KeysDB key val... KeyUD key
            lua_pushvalue(_K, -2);                                                                 // _K: KeysDB key val... KeyUD key KeyUD
            lua_rawset(_K, 1);                                                                     // _K: KeysDB key val... KeyUD
        } else {                                                                                   // _K: KeysDB key val... KeyUD
            // the KeyUD exists, we just want to update its contents
            // we create room if the KeyUD was full but it is no longer the case
            _should_wake_writers = (_key->limit > 0) && (_key->count >= _key->limit) && (_count < _key->limit);
            // TODO: turn this into a KeyUD::reset() method
            // empty the KeyUD for the specified key: replace uservalue with a virgin table, reset counters, but leave limit unchanged!
            lua_newtable(_K);                                                                      // _K: KeysDB key val... KeyUD {}
            lua_setiuservalue(_K, -2, kContentsTableIndex);                                        // _K: KeysDB key val... KeyUD
            _key->first = 1;
            _key->count = 0;
        }
        _key = KeyUD::PrepareAccess(_K, -1);                                                       // _K: KeysDB key val... fifo
        // move the fifo below the values we want to store
        lua_insert(_K, 3);                                                                         // _K: KeysDB key fifo val...
        _key->push(_K, _count);                                                                    // _K: KeysDB key fifo
    }
    return _should_wake_writers ? (lua_pushboolean(_K, 1), 1) : 0;
}

// #################################################################################################

// in: linda_ud key [count]
// out: at most <count> values
int keepercall_get(lua_State* const L_)
{
    KeeperState const _K{ L_ };
    int _count{ 1 };
    if (lua_gettop(_K) == 3) {                                                                     // _K: linda key count
        _count = static_cast<int>(lua_tointeger(_K, 3));
        lua_pop(_K, 1);                                                                            // _K: linda key
    }
    PushKeysDB(_K, 1);                                                                             // _K: linda key KeyUD
    lua_replace(_K, 1);                                                                            // _K: KeyUD key
    lua_rawget(_K, 1);                                                                             // _K: KeyUD fifo
    KeyUD* const _key{ KeyUD::PrepareAccess(_K, -1) };                                             // _K: KeyUD fifo
    if (_key != nullptr && _key->count > 0) {
        lua_remove(_K, 1);                                                                         // _K: fifo
        _count = std::min(_count, _key->count);
        // read <count> value off the fifo
        _key->peek(_K, _count);                                                                    // _K: fifo ...
        return _count;
    }
    // no fifo was ever registered for this key, or it is empty
    return 0;
}

// #################################################################################################

// in: linda_ud [, key [, ...]]
int keepercall_count(lua_State* const L_)
{
    KeeperState const _K{ L_ };
    switch (lua_gettop(_K)) {
    // no key is specified: return a table giving the count of all known keys
    case 1:                                                                                        // _K: linda
        PushKeysDB(_K, 1);                                                                         // _K: linda KeysDB
        lua_newtable(_K);                                                                          // _K: linda KeysDB out
        lua_replace(_K, 1);                                                                        // _K: out KeysDB
        lua_pushnil(_K);                                                                           // _K: out KeysDB nil
        while (lua_next(_K, 2)) {                                                                  // _K: out KeysDB key KeyUD
            KeyUD* const _key{ KeyUD::GetPtr(_K, -1) };
            lua_pop(_K, 1);                                                                        // _K: out KeysDB key
            lua_pushvalue(_K, -1);                                                                 // _K: out KeysDB key key
            lua_pushinteger(_K, _key->count);                                                      // _K: out KeysDB key key count
            lua_rawset(_K, -5);                                                                    // _K: out KeysDB key
        }
        lua_pop(_K, 1);                                                                            // _K: out
        break;

    // 1 key is specified: return its count
    case 2:                                                                                        // _K: linda key
        PushKeysDB(_K, 1);                                                                         // _K: linda key KeysDB
        lua_replace(_K, 1);                                                                        // _K: KeysDB key
        lua_rawget(_K, -2);                                                                        // _K: KeysDB KeyUD|nil
        if (lua_isnil(_K, -1)) { // the key is unknown                                             // _K: KeysDB nil
            lua_remove(_K, -2);                                                                    // _K: nil
        } else { // the key is known                                                               // _K: KeysDB KeyUD
            KeyUD* const _key{ KeyUD::GetPtr(_K, -1) };
            lua_pushinteger(_K, _key->count);                                                      // _K: KeysDB KeyUD count
            lua_replace(_K, -3);                                                                   // _K: count KeyUD
            lua_pop(_K, 1);                                                                        // _K: count
        }
        break;

    // a variable number of keys is specified: return a table of their counts
    default:                                                                                       // _K: linda keys... key#1
        lua_pushvalue(_K, 2); // duplicate the first key of the list                               // _K: linda keys... key#1
        PushKeysDB(_K, 1);                                                                         // _K: linda keys... key#1 KeysDB
        lua_newtable(_K);                                                                          // _K: linda keys... key#1 KeysDB out
        lua_replace(_K, 1);                                                                        // _K: out keys... key#1 KeysDB
        lua_replace(_K, 2); // the list of keys is the same, but for key#1 moved at the end        // _K: out KeysDB keys...
        while (lua_gettop(_K) > 2) {
            lua_pushvalue(_K, -1);                                                                 // _K: out KeysDB keys... key
            lua_rawget(_K, 2);                                                                     // _K: out KeysDB keys... KeyUD|nil
            KeyUD* const _key{ KeyUD::GetPtr(_K, -1) };
            lua_pop(_K, 1);                                                                        // _K: out KeysDB keys...
            if (_key != nullptr) { // the key is known
                lua_pushinteger(_K, _key->count);                                                  // _K: out KeysDB keys... count
                lua_rawset(_K, 1);                                                                 // _K: out KeysDB keys...
            } else { // the key is unknown
                lua_pop(_K, 1);                                                                    // _K: out KeysDB keys...
            }
        } // all keys are exhausted                                                                // _K: out KeysDB
        lua_pop(_K, 1);                                                                            // _K: out
    }
    LUA_ASSERT(_K, lua_gettop(_K) == 1);
    return 1;
}

// #################################################################################################

/*
 * Call a function ('func_name') in the keeper state, and pass on the returned
 * values to 'L'.
 *
 * 'linda':          deep Linda pointer (used only as a unique table key, first parameter)
 * 'starting_index': first of the rest of parameters (none if 0)
 *
 * Returns: number of return values (pushed to 'L'), unset in case of error
 */
KeeperCallResult keeper_call(KeeperState K_, keeper_api_t func_, lua_State* L_, Linda* linda_, int starting_index_)
{
    KeeperCallResult _result;
    int const _args{ starting_index_ ? (lua_gettop(L_) - starting_index_ + 1) : 0 };               // L: ... args...                                  K_:
    int const _top_K{ lua_gettop(K_) };
    // if we didn't do anything wrong, the keeper stack should be clean
    LUA_ASSERT(L_, _top_K == 0);

    STACK_GROW(K_, 2);
    PUSH_KEEPER_FUNC(K_, func_);                                                                   // L: ... args...                                  K_: func_
    lua_pushlightuserdata(K_, linda_);                                                             // L: ... args...                                  K_: func_ linda
    if (
        (_args == 0) ||
        (InterCopyContext{ linda_->U, DestState{ K_ }, SourceState{ L_ }, {}, {}, {}, LookupMode::ToKeeper, {} }.inter_copy(_args) == InterCopyResult::Success)
    ) {                                                                                            // L: ... args...                                  K_: func_ linda args...
        lua_call(K_, 1 + _args, LUA_MULTRET);                                                      // L: ... args...                                  K_: result...
        int const _retvals{ lua_gettop(K_) - _top_K };
        // note that this can raise a lua error while the keeper state (and its mutex) is acquired
        // this may interrupt a lane, causing the destruction of the underlying OS thread
        // after this, another lane making use of this keeper can get an error code from the mutex-locking function
        // when attempting to grab the mutex again (WINVER <= 0x400 does this, but locks just fine, I don't know about pthread)
        if (
            (_retvals == 0) ||
            (InterCopyContext{ linda_->U, DestState{ L_ }, SourceState{ K_ }, {}, {}, {}, LookupMode::FromKeeper, {} }.inter_move(_retvals) == InterCopyResult::Success)
        ) {                                                                                        // L: ... args... result...                        K_: result...
            _result.emplace(_retvals);
        }
    }
    // whatever happens, restore the stack to where it was at the origin
    lua_settop(K_, _top_K);                                                                        // L: ... args... result...                        K_:

    // don't do this for this particular function, as it is only called during Linda destruction, and we don't want to raise an error, ever
    if (func_ != KEEPER_API(clear)) [[unlikely]] {
        // since keeper state GC is stopped, let's run a step once in a while if required
        int const _gc_threshold{ linda_->U->keepers.gc_threshold };
        if (_gc_threshold == 0) [[unlikely]] {
            lua_gc(K_, LUA_GCSTEP, 0);
        } else if (_gc_threshold > 0) [[likely]] {
            int const _gc_usage{ lua_gc(K_, LUA_GCCOUNT, 0) };
            if (_gc_usage >= _gc_threshold) {
                lua_gc(K_, LUA_GCCOLLECT, 0);
                int const _gc_usage_after{ lua_gc(K_, LUA_GCCOUNT, 0) };
                if (_gc_usage_after > _gc_threshold) [[unlikely]] {
                    raise_luaL_error(L_, "Keeper GC threshold is too low, need at least %d", _gc_usage_after);
                }
            }
        }
    }

    return _result;
}

// #################################################################################################
// #################################################################################################
// ########################################## Keeper ###############################################
// #################################################################################################
// #################################################################################################

void* Keeper::operator new[](size_t size_, Universe* U_) noexcept
{
    // size_ is the memory for the element count followed by the elements themselves
    return U_->internalAllocator.alloc(size_);
}

// #################################################################################################

// can't actually delete the operator because the compiler generates stack unwinding code that could call it in case of exception
void Keeper::operator delete[](void* p_, Universe* U_)
{
    U_->internalAllocator.free(p_, *static_cast<size_t*>(p_) * sizeof(Keeper) + sizeof(size_t));
}

// #################################################################################################
// #################################################################################################
// ########################################## Keepers ##############################################
// #################################################################################################
// #################################################################################################

void Keepers::DeleteKV::operator()(Keeper* k_) const
{
    for (Keeper& _k : std::views::counted(k_, count)) {
        _k.~Keeper();
    }
    // operator[] returns the result of the allocation shifted by a size_t (the hidden element count)
    U->internalAllocator.free(reinterpret_cast<size_t*>(k_) - 1, count * sizeof(Keeper));
}

// #################################################################################################

void Keepers::close()
{
    if (isClosing.test_and_set(std::memory_order_release)) {
        assert(false); // should never close more than once in practice
        return;
    }

    if (std::holds_alternative<std::monostate>(keeper_array)) {
        return;
    }

    auto _closeOneKeeper = [](Keeper& keeper_) {
        lua_State* const _K{ std::exchange(keeper_.L, KeeperState{ nullptr }) };
        if (_K) {
            lua_close(_K);
        }
        return _K ? true : false;
    };

    if (std::holds_alternative<Keeper>(keeper_array)) {
        _closeOneKeeper(std::get<Keeper>(keeper_array));
    } else {
        KV& _kv = std::get<KV>(keeper_array);

        // NOTE: imagine some keeper state N+1 currently holds a linda that uses another keeper N, and a _gc that will make use of it
        // when keeper N+1 is closed, object is GCed, linda operation is called, which attempts to acquire keeper N, whose Lua state no longer exists
        // in that case, the linda operation should do nothing. which means that these operations must check for keeper acquisition success
        // which is early-outed with a keepers->nbKeepers null-check
        size_t const _nbKeepers{ std::exchange(_kv.nbKeepers, 0) };
        for (size_t const _i : std::ranges::iota_view{ size_t{ 0 }, _nbKeepers }) {
            if (!_closeOneKeeper(_kv.keepers[_i])) {
                // detected partial init: destroy only the mutexes that got initialized properly
                break;
            }
        }
    }

    keeper_array.emplace<std::monostate>();
}

// #################################################################################################

[[nodiscard]] Keeper* Keepers::getKeeper(int idx_)
{
    if (isClosing.test(std::memory_order_acquire)) {
        return nullptr;
    }

    if (std::holds_alternative<std::monostate>(keeper_array)) {
        return nullptr;
    }

    if (std::holds_alternative<Keeper>(keeper_array)) {
        return &std::get<Keeper>(keeper_array);
    }

    return &std::get<KV>(keeper_array).keepers.get()[idx_];
}

// #################################################################################################

[[nodiscard]] int Keepers::getNbKeepers() const
{
    if (isClosing.test(std::memory_order_acquire)) {
        return 0;
    }

    if (std::holds_alternative<std::monostate>(keeper_array)) {
        return 0;
    }

    if (std::holds_alternative<Keeper>(keeper_array)) {
        return 1;
    }

    return static_cast<int>(std::get<KV>(keeper_array).nbKeepers);
}

// #################################################################################################
/*
 * Initialize keeper states
 *
 * If there is a problem, returns nullptr and pushes the error message on the stack
 * else returns the keepers bookkeeping structure.
 *
 * Note: Any problems would be design flaws; the created Lua state is left
 *       unclosed, because it does not really matter. In production code, this
 *       function never fails.
 * settings table is expected at position 1 on the stack
 */

void Keepers::initialize(Universe& U_, lua_State* L_, int const nbKeepers_, int const gc_threshold_)
{
    gc_threshold = gc_threshold_;

    auto _initOneKeeper = [U = &U_, L = L_, gc_threshold = gc_threshold](Keeper& keeper_, int const i_) {
        STACK_CHECK_START_REL(L, 0);
        // note that we will leak K if we raise an error later
        KeeperState const _K{ state::CreateState(U, L) };                                          // L_: settings                                   _K:
        if (_K == nullptr) {
            raise_luaL_error(L, "out of memory while creating keeper states");
        }

        keeper_.L = _K;

        // Give a name to the state
        lua_pushfstring(_K, "Keeper #%d", i_ + 1);                                                 // L_: settings                                   _K: "Keeper #n"
        if constexpr (HAVE_DECODA_SUPPORT()) {
            lua_pushvalue(_K, -1);                                                                 //                                                _K: "Keeper #n" Keeper #n"
            lua_setglobal(_K, "decoda_name");                                                      // L_: settings                                   _K: "Keeper #n"
        }
        kLaneNameRegKey.setValue(_K, [](lua_State* L_) { lua_insert(L_, -2); });                   //                                                _K:

        STACK_CHECK_START_ABS(_K, 0);

        // copy the universe pointer in the keeper itself
        Universe::Store(_K, U);
        STACK_CHECK(_K, 0);

        // make sure 'package' is initialized in keeper states, so that we have require()
        // this because this is needed when transferring deep userdata object
        luaL_requiref(_K, LUA_LOADLIBNAME, luaopen_package, 1);                                    // L_: settings                                   _K: package
        lua_pop(_K, 1);                                                                            // L_: settings                                   _K:
        STACK_CHECK(_K, 0);
        tools::SerializeRequire(_K);
        STACK_CHECK(_K, 0);

        // copy package.path and package.cpath from the source state
        if (luaG_getmodule(L, LUA_LOADLIBNAME) != LuaType::NIL) {                                  // L_: settings package                           _K:
            // when copying with mode LookupMode::ToKeeper, error message is pushed at the top of the stack, not raised immediately
            InterCopyContext _c{ U, DestState{ _K }, SourceState{ L }, {}, SourceIndex{ lua_absindex(L, -1) }, {}, LookupMode::ToKeeper, {} };
            if (_c.inter_copy_package() != InterCopyResult::Success) {                             // L_: settings ... error_msg                     _K:
                // if something went wrong, the error message is at the top of the stack
                lua_remove(L, -2);                                                                 // L_: settings error_msg
                raise_lua_error(L);
            }
        }
        lua_pop(L, 1);                                                                             // L_: settings                                   _K:
        STACK_CHECK(L, 0);
        STACK_CHECK(_K, 0);

        // attempt to call on_state_create(), if we have one and it is a C function
        // (only support a C function because we can't transfer executable Lua code in keepers)
        // will raise an error in L_ in case of problem
        state::CallOnStateCreate(U, _K, L, LookupMode::ToKeeper);

        // _R[kLindasRegKey] = {}
        kLindasRegKey.setValue(_K, [](lua_State* L_) { lua_newtable(L_); });
        STACK_CHECK(_K, 0);

        // configure GC last
        if (gc_threshold >= 0) {
            lua_gc(_K, LUA_GCSTOP, 0);
        }
    };

    switch (nbKeepers_) {
    case 0:
        break;

    case 1:
        keeper_array.emplace<Keeper>();
        _initOneKeeper(std::get<Keeper>(keeper_array), 0);
        break;

    default:
        KV& _kv = keeper_array.emplace<KV>(
            std::unique_ptr<Keeper[], DeleteKV>{ new(&U_) Keeper[nbKeepers_], DeleteKV{ &U_, nbKeepers_ } },
            nbKeepers_
        );
        for (int const _i : std::ranges::iota_view{ 0, nbKeepers_ }) {
            _initOneKeeper(_kv.keepers[_i], _i);
        }
    }
}
