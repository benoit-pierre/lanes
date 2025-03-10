#pragma once

#ifdef __cplusplus
extern "C"
{
#endif // __cplusplus
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#ifdef __cplusplus
}
#endif // __cplusplus

// try to detect if we are building against LuaJIT or MoonJIT
#if defined(LUA_JITLIBNAME)
#include "luajit.h"
#if (defined(__x86_64__) || defined(_M_X64) || defined(__LP64__))
#define LUAJIT_FLAVOR() 64
#else // 64 bits
#define LUAJIT_FLAVOR() 32
#endif // 64 bits
#else // LUA_JITLIBNAME
#define LUAJIT_FLAVOR() 0
#define LUA_JITLIBNAME "jit"
#endif // LUA_JITLIBNAME

#include <cassert>
#include <string_view>

// code is now preferring Lua 5.4 API

// #################################################################################################

// a strong-typed wrapper over lua types to see them easier in a debugger
enum class LuaType
{
    NONE = LUA_TNONE,
    NIL = LUA_TNIL,
    BOOLEAN = LUA_TBOOLEAN,
    LIGHTUSERDATA = LUA_TLIGHTUSERDATA,
    NUMBER = LUA_TNUMBER,
    STRING = LUA_TSTRING,
    TABLE = LUA_TTABLE,
    FUNCTION = LUA_TFUNCTION,
    USERDATA = LUA_TUSERDATA,
    THREAD = LUA_TTHREAD,
    CDATA = 10 // LuaJIT CDATA
};

inline LuaType lua_type_as_enum(lua_State* L_, int idx_)
{
    return static_cast<LuaType>(lua_type(L_, idx_));
}
inline char const* lua_typename(lua_State* L_, LuaType t_)
{
    return lua_typename(L_, static_cast<int>(t_));
}

// #################################################################################################

// add some Lua 5.3-style API when building for Lua 5.1
#if LUA_VERSION_NUM == 501

#define lua501_equal lua_equal
inline int lua_absindex(lua_State* L_, int idx_)
{
    return (((idx_) >= 0 || (idx_) <= LUA_REGISTRYINDEX) ? (idx_) : lua_gettop(L_) + (idx_) + 1);
}
#if LUAJIT_VERSION_NUM < 20200 // moonjit is 5.1 plus bits of 5.2 that we don't need to wrap
inline void lua_pushglobaltable(lua_State* L_)
{
    lua_pushvalue(L_, LUA_GLOBALSINDEX);
}
#endif // LUAJIT_VERSION_NUM
inline int lua_setuservalue(lua_State* L_, int idx_)
{
    return lua_setfenv(L_, idx_);
}
inline void lua_getuservalue(lua_State* L_, int idx_)
{
    lua_getfenv(L_, idx_);
}
inline size_t lua_rawlen(lua_State* L_, int idx_)
{
    return lua_objlen(L_, idx_);
}
inline void luaG_registerlibfuncs(lua_State* L_, luaL_Reg const funcs_[])
{
    luaL_register(L_, nullptr, funcs_);
}
// keep as macros to be consistent with Lua headers
#define LUA_OK 0
#define LUA_ERRGCMM 666 // doesn't exist in Lua 5.1, we don't care about the actual value
void luaL_requiref(lua_State* L_, const char* modname_, lua_CFunction openf_, int glb_); // implementation copied from Lua 5.2 sources
inline int lua504_dump(lua_State* L_, lua_Writer writer_, void* data_, [[maybe_unused]] int strip_)
{
    return lua_dump(L_, writer_, data_);
}
#define LUA_LOADED_TABLE "_LOADED" // // doesn't exist in Lua 5.1

int luaL_getsubtable(lua_State* L_, int idx_, const char* fname_);

#endif // LUA_VERSION_NUM == 501

// #################################################################################################

// wrap Lua 5.2 calls under Lua 5.1 API when it is simpler that way
#if LUA_VERSION_NUM == 502

#ifndef lua501_equal // already defined when compatibility is active in luaconf.h
inline int lua501_equal(lua_State* L_, int a_, int b_)
{
    return lua_compare(L_, a_, b_, LUA_OPEQ);
}
#endif // lua501_equal
#ifndef lua_lessthan // already defined when compatibility is active in luaconf.h
inline int lua_lessthan(lua_State* L_, int a_, int b_)
{
    return lua_compare(L_, a_, b_, LUA_OPLT);
}
#endif // lua_lessthan
inline void luaG_registerlibfuncs(lua_State* L_, luaL_Reg const funcs_[])
{
    luaL_setfuncs(L_, funcs_, 0);
}
inline int lua504_dump(lua_State* L_, lua_Writer writer_, void* data_, [[maybe_unused]] int strip_)
{
    return lua_dump(L_, writer_, data_);
}
#define LUA_LOADED_TABLE "_LOADED" // // doesn't exist in Lua 5.2

#endif // LUA_VERSION_NUM == 502

// #################################################################################################

[[nodiscard]] inline LuaType luaG_getfield(lua_State* L_, int idx_, std::string_view const& k_)
{
// starting with Lua 5.3, lua_getfield returns the type of the value it found
#if LUA_VERSION_NUM < 503
    lua_getfield(L_, idx_, k_.data());
    return lua_type_as_enum(L_, -1);
#else // LUA_VERSION_NUM >= 503
    return static_cast<LuaType>(lua_getfield(L_, idx_, k_.data()));
#endif // LUA_VERSION_NUM >= 503
}

// #################################################################################################

// wrap Lua 5.3 calls under Lua 5.1 API when it is simpler that way
#if LUA_VERSION_NUM == 503

#ifndef lua501_equal // already defined when compatibility is active in luaconf.h
inline int lua501_equal(lua_State* L_, int a_, int b_)
{
    return lua_compare(L_, a_, b_, LUA_OPEQ);
}
#endif // lua501_equal
#ifndef lua_lessthan // already defined when compatibility is active in luaconf.h
inline int lua_lessthan(lua_State* L_, int a_, int b_)
{
    return lua_compare(L_, a_, b_, LUA_OPLT);
}
#endif // lua_lessthan
inline void luaG_registerlibfuncs(lua_State* L_, luaL_Reg const funcs_[])
{
    luaL_setfuncs(L_, funcs_, 0);
}
inline int lua504_dump(lua_State* L_, lua_Writer writer_, void* data_, int strip_)
{
    return lua_dump(L_, writer_, data_, strip_);
}
inline int luaL_optint(lua_State* L_, int n_, lua_Integer d_)
{
    return static_cast<int>(luaL_optinteger(L_, n_, d_));
}

#endif // LUA_VERSION_NUM == 503

// #################################################################################################

#if LUA_VERSION_NUM < 504

void* lua_newuserdatauv(lua_State* L_, size_t sz_, int nuvalue_);
int lua_getiuservalue(lua_State* L_, int idx_, int n_);
int lua_setiuservalue(lua_State* L_, int idx_, int n_);

#define LUA_GNAME "_G"

#endif // LUA_VERSION_NUM < 504

// #################################################################################################

// wrap Lua 5.4 calls under Lua 5.1 API when it is simpler that way
#if LUA_VERSION_NUM == 504

#ifndef lua501_equal // already defined when compatibility is active in luaconf.h
inline int lua501_equal(lua_State* L_, int a_, int b_)
{
    return lua_compare(L_, a_, b_, LUA_OPEQ);
}
#endif // lua501_equal
#ifndef lua_lessthan // already defined when compatibility is active in luaconf.h
inline int lua_lessthan(lua_State* L_, int a_, int b_)
{
    return lua_compare(L_, a_, b_, LUA_OPLT);
}
#endif // lua_lessthan
inline void luaG_registerlibfuncs(lua_State* L_, luaL_Reg const funcs_[])
{
    luaL_setfuncs(L_, funcs_, 0);
}
inline int lua504_dump(lua_State* L_, lua_Writer writer_, void* data_, int strip_)
{
    return lua_dump(L_, writer_, data_, strip_);
}
inline int luaL_optint(lua_State* L_, int n_, lua_Integer d_)
{
    return static_cast<int>(luaL_optinteger(L_, n_, d_));
}
#define LUA_ERRGCMM 666 // doesn't exist in Lua 5.4, we don't care about the actual value

#endif // LUA_VERSION_NUM == 504

// #################################################################################################

// a strong-typed wrapper over lua error codes to see them easier in a debugger
enum class LuaError
{
    OK = LUA_OK,
    YIELD = LUA_YIELD,
    ERRRUN = LUA_ERRRUN,
    ERRSYNTAX = LUA_ERRSYNTAX,
    ERRMEM = LUA_ERRMEM,
    ERRGCMM = LUA_ERRGCMM, // pre-5.4
    ERRERR = LUA_ERRERR,
    ERRFILE = LUA_ERRFILE
};

inline constexpr LuaError ToLuaError(int rc_)
{
    assert(rc_ == LUA_OK || rc_ == LUA_YIELD || rc_ == LUA_ERRRUN || rc_ == LUA_ERRSYNTAX || rc_ == LUA_ERRMEM || rc_ == LUA_ERRGCMM || rc_ == LUA_ERRERR);
    return static_cast<LuaError>(rc_);
}

// #################################################################################################

LuaType luaG_getmodule(lua_State* L_, std::string_view const& name_);

// #################################################################################################

#define STRINGVIEW_FMT "%.*s"

// a replacement of lua_tolstring
[[nodiscard]] inline std::string_view lua_tostringview(lua_State* L_, int idx_)
{
    size_t _len{ 0 };
    char const* _str{ lua_tolstring(L_, idx_, &_len) };
    return std::string_view{ _str, _len };
}

[[nodiscard]] inline std::string_view luaL_checkstringview(lua_State* L_, int idx_)
{
    size_t _len{ 0 };
    char const* _str{ luaL_checklstring(L_, idx_, &_len) };
    return std::string_view{ _str, _len };
}

[[nodiscard]] inline std::string_view luaL_optstringview(lua_State* L_, int idx_, std::string_view const& default_)
{
    if (lua_isnoneornil(L_, idx_)) {
        return default_;
    }
    size_t _len{ 0 };
    char const* _str{ luaL_optlstring(L_, idx_, default_.data(), &_len) };
    return std::string_view{ _str, _len };
}

[[nodiscard]] inline std::string_view lua_pushstringview(lua_State* L_, std::string_view const& str_)
{
#if LUA_VERSION_NUM == 501
    // lua_pushlstring doesn't return a value in Lua 5.1
    lua_pushlstring(L_, str_.data(), str_.size());
    return lua_tostringview(L_, -1);
#else
    return std::string_view{ lua_pushlstring(L_, str_.data(), str_.size()), str_.size() };
#endif // LUA_VERSION_NUM > 501
}