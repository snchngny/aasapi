#pragma once
#include <windows.h>
#include <unknwn.h>
#include <cstdint>
// The ASIO ABI requires 4-byte packing also with LLVM/MinGW.
#pragma pack(push,4)
#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"
#pragma pack(pop)
static_assert(sizeof(ASIOBufferInfo)==24,"Windows x64 ASIO buffer ABI");
static_assert(sizeof(ASIOChannelInfo)==52,"Windows x64 ASIO channel ABI");
static_assert(sizeof(ASIOTime)==148,"Windows x64 ASIO time ABI");

inline constexpr CLSID aggregate_clsid={0xa51448c1,0x33a9,0x44d4,{0x91,0x73,0x67,0x84,0xe1,0x92,0x0d,0x63}};
inline constexpr char aggregate_clsid_text[]="{A51448C1-33A9-44D4-9173-6784E1920D63}";
template<class SampleCounter>
inline void split_samples(uint64_t value,SampleCounter& result) {
    result.hi=static_cast<unsigned long>(value>>32);
    result.lo=static_cast<unsigned long>(value);
}
