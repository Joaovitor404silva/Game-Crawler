#pragma once
#include <string>
#include <vector>
#include <stdint.h>

struct Out;

struct EngineBackend {
  const char* name;
  bool (*detect)();
  bool (*tryInit)(std::string& why);
  bool (*ready)();
  void (*tick)();
  uintptr_t (*resolveObj)(const std::string& s);
  size_t (*objSize)(uintptr_t a);
  std::string (*fieldAt)(uintptr_t obj, int off);
  std::string (*describe)(uintptr_t a);
  std::string (*label)(uintptr_t a);
  bool (*handle)(const std::string& c, const std::vector<std::string>& t, const std::string& line, Out& o);
  bool (*propAddr)(const std::string& obj, const std::string& path, uintptr_t& addr, int& size, std::string& type, std::string& val, std::string& err);
};

const EngineBackend* ActiveEngine();
std::string EngineReport();
bool ImageHasBytes(const void* p, size_t n);
bool ImageHasWide(const wchar_t* s);
bool ExeNameHas(const char* s);

bool EngineTry(std::string& why);
bool EngineReady();
void EngineTick();
bool EngineHandle(const std::string& c, const std::vector<std::string>& t, const std::string& line, Out& o);
uintptr_t ResolveObj(const std::string& s);
size_t ObjSize(uintptr_t a);
std::string PropAtOffset(uintptr_t obj, int off);
std::string DescribeObj(uintptr_t a);
std::string ObjLabel(uintptr_t a);
bool UPropAddr(const std::string& obj, const std::string& path, uintptr_t& addr, int& size, std::string& type, std::string& val, std::string& err);
