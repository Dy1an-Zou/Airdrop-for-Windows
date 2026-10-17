// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "mt7925_txpower.h"
#include <sstream>

namespace mt7925 {
// Only correlated PCIe format-0 TXS is decoded. Firmware's raw power field is
// not an RF measurement; its signedness/scaling is not established here.
inline void write_txs_diagnostic(std::ostream& out,std::string_view hex) {
  const auto d=decode_cached_diagnostic(hex);
  constexpr size_t header=1692,entries=1724,stride=80,limit=8;
  const auto requests=le32(d,header),matched=le32(d,header+12);
  if(requests>limit || matched>requests)throw std::runtime_error("TXS counters");
  uint32_t found=0;
  auto zero=[](std::span<const uint8_t> p){return std::all_of(p.begin(),p.end(),[](uint8_t b){return b==0;});};
  for(size_t i=0;i<limit;++i) {
    const auto e=std::span<const uint8_t>(d).subspan(entries+i*stride,stride);
    if(i>=requests) {
      if(!zero(e))throw std::runtime_error("TXS unused entry");
      continue;
    }
    const auto pid=le32(e,0),fc=le32(e,4),seq=le32(e,8),wcid=le32(e,12),rate=le32(e,16),valid=le32(e,28);
    const auto type=(fc>>2)&3u,sub=(fc>>4)&15u;
    if(pid!=3+i || fc>65535 || seq>4095 || wcid>4095 || rate<18 || rate>25 ||
       !((type==0 && sub==13) || (type==2 && (sub==0 || sub==8))) ||
       (e[20]&1) || zero(e.subspan(20,6)) || e[26] || e[27] || valid>1)
      throw std::runtime_error("TXS request identity");
    const auto raw=e.subspan(32,48);
    if(!valid) {
      if(!zero(raw))throw std::runtime_error("TXS unmatched raw result");
      continue;
    }
    ++found;
    const auto t0=le32(raw,0),t1=le32(raw,4),t2=le32(raw,8),t3=le32(raw,12);
    if(((t0>>23)&3)!=0 || !(t0&(1u<<15)) || (t1>>20)!=seq ||
       ((t2>>16)&4095)!=wcid || (t3>>24)!=pid)
      throw std::runtime_error("TXS uncorrelated cached result");
  }
  if(found!=matched)throw std::runtime_error("TXS matched count");
  const auto first_size=le32(d,2364),first_bytes=le32(d,2368);
  const auto first=std::span<const uint8_t>(d).subspan(2372,64);
  if(first_bytes>64 || first_bytes!=std::min(first_size,64u) ||
     (le32(d,1696) ? first_size<4 : first_size!=0) ||
     !zero(first.subspan(first_bytes)) || (first_bytes>=4 && (first[3]>>3)!=0))
    throw std::runtime_error("TXS first envelope cache");
  // Validate the whole bounded object before emitting a partial JSON record.
  std::ostringstream json;
  constexpr const char* names[]={"requests","envelopes","records","matched","duplicates","unmatched","malformed","unsupported"};
  json<<"TXS firmware packet status JSON: {\"measurement\":\"firmware_packet_status_not_rf_measurement\",\"power_units\":\"raw_u8_unscaled\"";
  for(size_t i=0;i<std::size(names);++i)json<<",\""<<names[i]<<"\":"<<le32(d,header+i*4);
  json<<",\"first_envelope_size\":"<<first_size<<",\"first_envelope_bytes\":"<<first_bytes
      <<",\"first_envelope_hex\":\""<<hex.substr(2372*2,first_bytes*2)<<"\",\"entries\":[";
  for(size_t i=0;i<requests;++i) {
    if(i)json<<',';
    const size_t pos=entries+i*stride;
    const auto e=std::span<const uint8_t>(d).subspan(pos,stride);
    json<<"{\"pid\":"<<le32(e,0)<<",\"fc\":"<<le32(e,4)<<",\"sequence\":"<<le32(e,8)
        <<",\"wcid\":"<<le32(e,12)<<",\"requested_rate_table\":"<<le32(e,16)
        <<",\"receiver_hex\":\""<<hex.substr((pos+20)*2,12)<<"\",\"matched\":"<<le32(e,28);
    if(le32(e,28)) {
      const auto raw=e.subspan(32,48);
      json<<",\"tx_rate_raw\":"<<(le32(raw,0)&16383)<<",\"tx_power_raw\":"<<(le32(raw,4)&255)
          <<",\"error_bits_raw\":"<<(le32(raw,0)&0x007f0000u)
          <<",\"band_raw\":"<<((le32(raw,8)>>28)&3)
          <<",\"timestamp_raw\":"<<le32(raw,16)
          <<",\"raw_txs_hex\":\""<<hex.substr((pos+32)*2,96)<<'"';
    }
    json<<'}';
  }
  json<<"]}";
  out<<json.str()<<std::endl;
}
} // namespace mt7925
