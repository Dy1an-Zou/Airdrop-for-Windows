// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "mt7925_codec.h"
#include <ostream>
#include <string_view>
namespace mt7925 {
inline constexpr size_t DiagnosticBytes=2436;
inline constexpr uint32_t DiagnosticVersion=12;
// Decode only the bounded cached ABI. This function never calls a device.
inline Bytes decode_cached_diagnostic(std::string_view hex) {
  if(hex.size()!=DiagnosticBytes*2)throw std::runtime_error("TXPOWER diagnostic size");
  Bytes d(DiagnosticBytes);
  auto nibble=[](char c)->unsigned {
    if(c>='0' && c<='9')return unsigned(c-'0');
    if(c>='a' && c<='f')return unsigned(c-'a'+10);
    throw std::runtime_error("TXPOWER diagnostic hex");
  };
  for(size_t i=0;i<d.size();++i)d[i]=uint8_t(nibble(hex[i*2])*16+nibble(hex[i*2+1]));
  if(le32(d,0)!=DiagnosticBytes || le32(d,4)!=DiagnosticVersion)
    throw std::runtime_error("TXPOWER diagnostic ABI");
  return d;
}
inline void write_txpower_diagnostic(std::ostream& out,std::string_view hex) {
  const auto d=decode_cached_diagnostic(hex);
  const auto attempted=le32(d,772),valid=le32(d,776),sequence=le32(d,784),reason=le32(d,788),size=le32(d,792);
  const auto error=static_cast<int32_t>(le32(d,780));
  const auto p=std::span<const uint8_t>(d).subspan(796,896);
  constexpr const char* reasons[]={"not_requested","waiting","size","packet_type","declared_length",
    "sequence","unsolicited","ack_only","ack_error","tag","tlv_length","category","band","format","valid","event_id"};
  if(attempted>1 || valid>1 || reason>=std::size(reasons) ||
      (valid && (!attempted || error || reason!=14 || !sequence || sequence>15 || size!=893 ||
       le16(p,32)!=861 || le16(p,34)!=0x3800 || p[36]!=0x2a || p[37]!=sequence || (p[38]&4) ||
       le16(p,48)!=5 || le16(p,50)!=841 || p[52]!=5 || p[53]!=0 || p[55]>1)))
    throw std::runtime_error("TXPOWER inconsistent cached result");
  out<<"TXPOWER firmware target JSON: {\"attempted\":"<<attempted<<",\"valid\":"<<valid
     <<",\"error\":"<<error<<",\"expected_sequence\":"<<sequence<<",\"reason\":\""<<reasons[reason]
     <<"\",\"response_bytes\":"<<size<<",\"raw_truncated\":"<<(size>896?"true":"false")
     <<",\"measurement\":\"firmware_target_not_rf_measurement\",\"units\":\"raw_signed_8_bit\",\"second_dimension\":\"band_index\"";
  if(size>=40)out<<",\"event_id\":"<<unsigned(p[36])<<",\"reply_sequence\":"<<unsigned(p[37])<<",\"option\":"<<unsigned(p[38]);
  out<<",\"raw_event_hex\":\""<<hex.substr(796*2,(size<896?size:896)*2)<<'"';
  if(valid) {
    auto signed_byte=[](uint8_t b){return b<128?int(b):int(b)-256;};
    struct Group {const char* name;size_t count;};
    constexpr Group groups[]={{"cck",4},{"ofdm",8},{"ht20",8},{"ht40",9},
      {"vht20",12},{"vht40",12},{"vht80",12},{"vht160",12},
      {"he26",12},{"he52",12},{"he106",12},{"he242",12},{"he484",12},{"he996",12},{"he996x2",12},
      {"eht26",16},{"eht52",16},{"eht106",16},{"eht242",16},{"eht484",16},{"eht996",16},{"eht996x2",16},{"eht996x4",16},
      {"eht26_52",16},{"eht26_106",16},{"eht484_242",16},{"eht996_484",16},{"eht996_484_242",16},
      {"eht996x2_484",16},{"eht996x3",16},{"eht996x3_484",16}};
    out<<",\"category\":5,\"queried_band_index\":0,\"channel_band_raw\":"<<unsigned(p[54])
       <<",\"format\":"<<unsigned(p[55])<<",\"pwr_max\":"<<signed_byte(p[890])
       <<",\"pwr_min\":"<<signed_byte(p[891])<<",\"rates_by_band\":{";
    size_t offset=56;bool first=true;
    for(const auto& group:groups) {
      if(!first)out<<',';first=false;out<<'"'<<group.name<<"\":[";
      for(size_t rate=0;rate<group.count;++rate) {
        if(rate)out<<',';
        out<<'['<<signed_byte(p[offset])<<','<<signed_byte(p[offset+1])<<']';offset+=2;
      }
      out<<']';
    }
    out<<'}';
  }
  out<<'}'<<std::endl;
}
} // namespace mt7925
