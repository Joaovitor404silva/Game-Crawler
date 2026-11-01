#pragma once
#include "core_util.h"
#include <functional>

bool RunOnGame(std::function<void()> fn, DWORD timeoutMs, std::string* err);
bool EvalAddr(const std::string& s, uint64_t& v, std::string& err);
bool InModule(uintptr_t a);
bool InText(uintptr_t a);
std::string Sym(uintptr_t a);
std::string ModOf(uintptr_t a);
int SafeProcessEvent(void* fn, void* obj, void* func, void* params);
bool SafeNameToString(void* fn, const void* name, void* outFString);

void InitUObj();
void TickWatches();
uintptr_t ResolveObj(const std::string& s);
size_t ObjSize(uintptr_t a);
std::string PropAtOffset(uintptr_t obj, int off);
std::string DescribeObj(uintptr_t a);
bool HandleUObj(const std::string& c, const std::vector<std::string>& t, const std::string& line, Out& o);

void LoadNatives();
std::string NativeNameAt(uintptr_t a);
bool LookupNative(const std::string& name, uintptr_t& exec, uintptr_t& impl);
void InitHooks();
void ShutdownHooks();
uintptr_t DetourTarget(uintptr_t a);
std::string DetourOwner(uintptr_t a);
bool HandleHooks(const std::string& c, const std::vector<std::string>& t, Out& o);

void InitPlugins();
void ShutdownPlugins();
void TickPlugins();
bool HandlePlugins(const std::string& c, const std::vector<std::string>& t, Out& o);
std::string CVarCmd(const std::string& name, const std::string* value);

std::string FrameSym(uint64_t a, bool isReturn);
uintptr_t ResolveObjCached(const std::string& s);
extern thread_local int t_objCacheMs;
extern thread_local int t_jsonSuppress;
void JsonSetData(const std::string& rawJson);
void JsonSetOk(bool ok);
bool UPropAddr(const std::string& obj, const std::string& path, uintptr_t& addr, int& size, std::string& type, std::string& val, std::string& err);


void InitProbe();
void ShutdownProbe();
bool HandleProbe(const std::string& c, const std::vector<std::string>& t, const std::string& line, Out& o);
