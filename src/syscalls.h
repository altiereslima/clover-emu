#pragma once
#include "kernel.h"

s32 do_mem_syscall(Thread& t, u32 nr);
s32 do_futex(Thread& t);
s32 do_net_syscall(Thread& t, u32 nr, bool& noret);
int futex_wake(Process& p, u32 addr, int n, u32 bitset);
std::vector<std::string> read_str_array(AddressSpace& m, u32 addr);
// guest mmap helper usable by HLE (anonymous)
u32 guest_mmap_anon(Process& p, u32 len, const char* name);
