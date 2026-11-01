#pragma once
#include <windows.h>
#include <intrin.h>
#include <cstdint>

namespace s2n_pf
{
    inline int g_on = 1;
    inline int g_fix = 0;
    inline int g_seen = 0;
    inline const DWORD* g_idx = nullptr;
    inline bool g_init = false;
    constexpr uintptr_t kSlot = 0xd88;

    inline auto idx_ptr() -> const DWORD*
    {
        if (g_init) return g_idx;
        g_init = true;
        auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        if (!base) return nullptr;
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        if (!dd.VirtualAddress || dd.Size < sizeof(IMAGE_TLS_DIRECTORY64)) return nullptr;
        auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(base + dd.VirtualAddress);
        g_idx = reinterpret_cast<const DWORD*>(tls->AddressOfIndex);
        return g_idx;
    }
    inline auto slot() -> uintptr_t*
    {
        const DWORD* ip = idx_ptr();
        if (!ip) return nullptr;
        auto* arr = reinterpret_cast<uint8_t**>(__readgsqword(0x58));
        if (!arr || !arr[*ip]) return nullptr;
        return reinterpret_cast<uintptr_t*>(arr[*ip] + kSlot);
    }
    inline auto save() -> uintptr_t
    {
        uintptr_t* s = slot();
        return s ? *s : 0;
    }
    inline auto fix(uintptr_t v) -> void
    {
        uintptr_t* s = slot();
        if (!s || *s == v) return;
        ++g_seen;
        if (!g_on) return;
        *s = v;
        ++g_fix;
    }
}
