// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <ostream>
#include <span>

namespace mt7925 {
// Only the polling thread writes these counters. Read after that thread joins.
// Fixed storage keeps diagnostics allocation-free on the receive path.
struct RxDiagnostics {
  enum Reason : size_t {Passed, ShortHeader, ProtocolVersion, DsForm, FrameType, Bssid, ReasonCount};
  struct Sample {
    std::array<uint8_t,32> header{};
    size_t bytes=0,frame_bytes=0;
    bool local_ra=false,awdl_bssid=false;
  };
  struct Class {
    uint64_t count=0,unrelated=0;
    std::array<uint64_t,65> fc{}; // type*16+subtype; 64 means unavailable.
    std::array<uint64_t,5> ds{}; // 4 means unavailable.
    // Filter BSSID compares addr3, exactly as Bridge::poll does, even for DS frames.
    // States: unset, match, other, unavailable. RA: identity unset, local, other, unavailable.
    std::array<uint64_t,4> filter_bssid{},ra{};
    std::array<Sample,4> first{}; // At most one relevant header per type and reason.
  };
  uint64_t poll_calls=0,normal_returns=0,pending_returns=0,invalid_air_returns=0,empty_returns=0;
  uint64_t records=0,returned_frames=0,capture_submitted=0;
  std::array<Class,ReasonCount> classes{};

  void empty(int32_t result) noexcept {
    ++normal_returns;
    if(result==-3)++pending_returns;
    else if(result==-1)++invalid_air_returns;
    else ++empty_returns;
  }
  static bool same(std::span<const uint8_t> f,size_t offset,const std::array<uint8_t,6>& a) noexcept {
    return offset<=f.size() && f.size()-offset>=6 && std::equal(a.begin(),a.end(),f.begin()+offset);
  }
  void record(Reason reason,std::span<const uint8_t> f,bool identity,
              const std::array<uint8_t,6>& own,const std::array<uint8_t,6>& filter) noexcept {
    auto& c=classes[reason];++records;++c.count;
    const bool have_fc=f.size()>=2;
    const size_t type=have_fc?(f[0]>>2)&3:4,subtype=have_fc?f[0]>>4:0,ds=have_fc?f[1]&3:4;
    ++c.fc[have_fc?type*16+subtype:64];++c.ds[ds];
    const bool configured=std::any_of(filter.begin(),filter.end(),[](auto b){return b!=0;});
    ++c.filter_bssid[!configured?0:f.size()<24?3:same(f,16,filter)?1:2];
    const bool local=identity && same(f,4,own);
    ++c.ra[!identity?0:f.size()<10?3:local?1:2];
    // For relevance use the actual 802.11 BSSID location, not the addr3 filter.
    static constexpr std::array<uint8_t,6> awdl={0,0x25,0,0xff,0x94,0x73};
    const bool version0=have_fc && !(f[0]&3);
    const bool awdl_match=version0 && (type==0 || type==2) && ds!=3 &&
      same(f,ds==1?4:ds==2?10:16,awdl);
    if(!local && !awdl_match){++c.unrelated;return;}
    if(!version0 || type>=3)return;
    size_t header=0;
    if(type==0)header=24;
    else if(type==2)header=24+(ds==3?6:0)+((subtype&8)?2:0)+((subtype&8) && (f[1]&0x80)?4:0);
    else if(subtype==12 || subtype==13)header=10; // CTS / ACK.
    else if(subtype==11)header=16; // RTS. Other control layouts are not sampled.
    if(!header || f.size()<header)return;
    auto& sample=c.first[type];
    if(sample.bytes)return;
    sample.bytes=std::min(header,sample.header.size());sample.frame_bytes=f.size();
    sample.local_ra=local;sample.awdl_bssid=awdl_match;
    std::copy_n(f.begin(),sample.bytes,sample.header.begin());
  }
  template<size_t N> static void counts(std::ostream& out,const std::array<uint64_t,N>& a) {
    out<<'[';for(size_t i=0;i<N;++i){if(i)out<<',';out<<a[i];}out<<']';
  }
  void write_json(std::ostream& out) const {
    static constexpr const char* names[]={"passed","short_header","protocol_version","ds_form","frame_type","bssid"};
    out<<"{\"version\":1,\"poll_calls\":"<<poll_calls<<",\"normal_returns\":"<<normal_returns
       <<",\"pending_returns\":"<<pending_returns<<",\"invalid_air_returns\":"<<invalid_air_returns
       <<",\"empty_returns\":"<<empty_returns<<",\"records\":"<<records<<",\"returned_frames\":"<<returned_frames
       <<",\"capture_submitted\":"<<capture_submitted
       <<",\"filter_bssid_states\":[\"unset\",\"match_addr3\",\"other_addr3\",\"unavailable\"]"
       <<",\"ra_states\":[\"identity_unset\",\"local\",\"other\",\"unavailable\"],\"classes\":[";
    for(size_t r=0;r<ReasonCount;++r) {
      if(r)out<<',';const auto& c=classes[r];
      out<<"{\"reason\":\""<<names[r]<<"\",\"count\":"<<c.count<<",\"unrelated\":"<<c.unrelated<<",\"fc\":[";
      bool comma=false;
      for(size_t i=0;i<64;++i)if(c.fc[i]) {
        if(comma)out<<',';comma=true;
        out<<"{\"type\":"<<i/16<<",\"subtype\":"<<i%16<<",\"count\":"<<c.fc[i]<<'}';
      }
      out<<"],\"fc_unavailable\":"<<c.fc[64]<<",\"ds\":";counts(out,c.ds);
      out<<",\"filter_bssid\":";counts(out,c.filter_bssid);out<<",\"ra\":";counts(out,c.ra);
      out<<",\"samples\":[";comma=false;
      for(size_t type=0;type<4;++type) {
        const auto& s=c.first[type];if(!s.bytes)continue;
        if(comma)out<<',';comma=true;
        out<<"{\"type\":"<<type<<",\"frame_bytes\":"<<s.frame_bytes<<",\"header_bytes\":"<<s.bytes
           <<",\"local_ra\":"<<(s.local_ra?"true":"false")<<",\"awdl_bssid\":"<<(s.awdl_bssid?"true":"false")<<",\"header_hex\":\"";
        static constexpr char hex[]="0123456789abcdef";
        for(size_t i=0;i<s.bytes;++i)out<<hex[s.header[i]>>4]<<hex[s.header[i]&15];
        out<<"\"}";
      }
      out<<"]}";
    }
    out<<"]}";
  }
};
} // namespace mt7925
