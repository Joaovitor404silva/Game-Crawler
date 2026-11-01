#include <windows.h>
#include <stdint.h>
#include <string.h>

bool SafeRead(void* dst, uintptr_t src, size_t n) {
  if (src < 0x10000) return false;
  __try { memcpy(dst, (const void*)src, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool RawWrite(uintptr_t dst, const void* src, size_t n) {
  __try { memcpy((void*)dst, src, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeWrite(uintptr_t dst, const void* src, size_t n) {
  if (dst < 0x10000) return false;
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery((void*)dst, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
  DWORD p = mbi.Protect & 0xff;
  bool writable = p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_WRITECOPY;
  if (writable) return RawWrite(dst, src, n);
  DWORD old;
  if (!VirtualProtect((void*)dst, n, PAGE_EXECUTE_READWRITE, &old)) return false;
  bool ok = RawWrite(dst, src, n);
  VirtualProtect((void*)dst, n, old, &old);
  FlushInstructionCache(GetCurrentProcess(), (void*)dst, n);
  return ok;
}

typedef void(__fastcall* PEFn)(void*, void*, void*);
int SafeProcessEvent(void* fn, void* obj, void* func, void* params) {
  __try { ((PEFn)fn)(obj, func, params); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return (int)GetExceptionCode(); }
}
