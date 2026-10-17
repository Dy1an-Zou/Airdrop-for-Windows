// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include "mt7925_codec.h"

namespace mt7925 {
inline std::runtime_error win_error(const char* message) {
  return std::runtime_error(std::string(message)+" (Windows "+std::to_string(GetLastError())+")");
}
inline std::wstring find_device() {
  GUID guid={0x3c9e8b97,0x7d9b,0x4db2,{0x91,0x3d,0xda,0x42,0x79,0x25,0,1}};
  HDEVINFO list=SetupDiGetClassDevsW(&guid,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
  if(list==INVALID_HANDLE_VALUE)throw win_error("enumerate MT7925 candidate");
  struct Cleanup {HDEVINFO h;~Cleanup(){SetupDiDestroyDeviceInfoList(h);}} cleanup{list};
  std::wstring path;
  for(DWORD i=0;i<16;++i) {
    SP_DEVICE_INTERFACE_DATA entry{};entry.cbSize=sizeof(entry);
    if(!SetupDiEnumDeviceInterfaces(list,nullptr,&guid,i,&entry)) {
      if(GetLastError()==ERROR_NO_MORE_ITEMS)break;
      throw win_error("enumerate candidate interface");
    }
    DWORD size=0;
    SetupDiGetDeviceInterfaceDetailW(list,&entry,nullptr,0,&size,nullptr);
    if(size<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || size>65536)throw std::runtime_error("invalid device path size");
    // Heap allocation is suitably aligned for the SDK structure.
    std::vector<uint64_t> memory((size+7u)/8u);
    auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(memory.data());
    detail->cbSize=sizeof(*detail);
    if(!SetupDiGetDeviceInterfaceDetailW(list,&entry,detail,size,nullptr,nullptr))throw win_error("read candidate path");
    if(!path.empty())throw std::runtime_error("multiple MT7925 candidate interfaces");
    path=detail->DevicePath;
  }
  if(path.empty())throw std::runtime_error("MT7925 candidate is not installed");
  return path;
}
inline Bytes pinned_image(const std::filesystem::path& path,const std::string& expected) {
  auto size=std::filesystem::file_size(path);
  if(!size || size>8*1024*1024)throw std::runtime_error("invalid pinned firmware length");
  Bytes p(static_cast<size_t>(size));std::ifstream input(path,std::ios::binary);
  if(!input.read(reinterpret_cast<char*>(p.data()),static_cast<std::streamsize>(p.size())) || input.peek()!=EOF)
    throw std::runtime_error("firmware file changed or unreadable");
  BCRYPT_ALG_HANDLE algorithm=nullptr;std::array<uint8_t,32> hash{};
  if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("SHA256 provider unavailable");
  auto result=BCryptHash(algorithm,nullptr,0,p.data(),static_cast<ULONG>(p.size()),hash.data(),static_cast<ULONG>(hash.size()));
  BCryptCloseAlgorithmProvider(algorithm,0);
  if(result<0)throw std::runtime_error("SHA256 failed");
  constexpr char digits[]="0123456789abcdef";std::string actual;
  for(auto b:hash){actual+=digits[b>>4];actual+=digits[b&15];}
  if(actual!=expected)throw std::runtime_error("firmware does not match pinned source");
  return p;
}
inline Bytes firmware_bundle(const std::filesystem::path& folder) {
  auto patch=pinned_image(folder/L"WIFI_MT7925_PATCH_MCU_1_1_hdr.bin","8eb46014d2a6b4124472eee7476d995008a6f40b1daffef87eb42f30d98699e1");
  auto ram=pinned_image(folder/L"WIFI_RAM_CODE_MT7925_1_1.bin","23ff53b4bb639b30481e2e06bb1688569ad1ba971b897936db539882abfbd120");
  Bytes p(16);put32(p,0,0x3546574d);put32(p,4,1);put32(p,8,uint32_t(patch.size()));put32(p,12,uint32_t(ram.size()));
  p.insert(p.end(),patch.begin(),patch.end());p.insert(p.end(),ram.begin(),ram.end());return p;
}
class Device {
  HANDLE handle_=INVALID_HANDLE_VALUE;
  std::mutex mutex_;
public:
  Device() {
    const auto path=find_device();
    handle_=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
    if(handle_==INVALID_HANDLE_VALUE)throw win_error("open MT7925 candidate");
  }
  ~Device(){if(handle_!=INVALID_HANDLE_VALUE)CloseHandle(handle_);} // Kernel file cleanup fences DMA.
  Device(const Device&)=delete;Device& operator=(const Device&)=delete;
  std::string diagnostics() {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::array<uint8_t,2436> out{};DWORD count=0;
    if(!DeviceIoControl(handle_,CTL_CODE(FILE_DEVICE_UNKNOWN,0x80c,METHOD_BUFFERED,FILE_READ_DATA),
        nullptr,0,out.data(),static_cast<DWORD>(out.size()),&count,nullptr))throw win_error("read MCU diagnostics");
    if(count!=out.size() || le32(out,0)!=2436 || le32(out,4)!=12)throw std::runtime_error("invalid MCU diagnostic ABI");
    constexpr char digits[]="0123456789abcdef";std::string hex;
    for(auto b:out){hex+=digits[b>>4];hex+=digits[b&15];}
    return hex;
  }
  Reply call(uint32_t operation,std::span<const uint8_t> input={}) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if(operation<Start || operation>Address || input.size()>56+2*8*1024*1024)throw std::runtime_error("invalid driver request");
    Bytes out(operation==Poll?PollBytes:ReplyBytes);DWORD count=0;
    DWORD code=CTL_CODE(FILE_DEVICE_UNKNOWN,operation,METHOD_BUFFERED,operation==Status?FILE_READ_DATA:FILE_READ_DATA|FILE_WRITE_DATA);
    if(!DeviceIoControl(handle_,code,input.empty()?nullptr:const_cast<uint8_t*>(input.data()),static_cast<DWORD>(input.size()),
        out.data(),static_cast<DWORD>(out.size()),&count,nullptr))throw win_error("MT7925 control");
    if(count>out.size())throw std::runtime_error("driver returned excess bytes");
    out.resize(count);return decode(out,operation);
  }
};
} // namespace mt7925
