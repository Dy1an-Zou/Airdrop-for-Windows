// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace mt7925 {
using Bytes=std::vector<uint8_t>;
constexpr uint32_t Start=0x805,Status=0x806,Channel=0x807,Tx=0x808,Poll=0x809,Stop=0x80a,Address=0x80b;
constexpr size_t ReplyBytes=96,PollBytes=16501;
inline uint32_t le16(std::span<const uint8_t> p,size_t n) {
  if(n>p.size() || p.size()-n<2) throw std::runtime_error("short u16");
  return uint32_t(p[n]) | uint32_t(p[n+1])<<8;
}
inline uint32_t le32(std::span<const uint8_t> p,size_t n) {
  return le16(p,n) | le16(p,n+2)<<16;
}
inline void put32(Bytes& p,size_t n,uint32_t v) {
  if(n>p.size() || p.size()-n<4) throw std::runtime_error("short output");
  for(size_t i=0;i<4;++i)p[n+i]=uint8_t(v>>(i*8));
}
struct Reply {
  int32_t result=0,error=0,stop_error=0;
  bool ready=false,attempted=false,stopped=false,published=false,quiescent=false;
  uint32_t stage=0,primary=0,center=0,width=0,inflight=0,bands=0,hardware=0,token=0xffffffff;
  std::array<uint8_t,6> mac{};
  Bytes record;
};
inline Reply decode(std::span<const uint8_t> p,uint32_t operation) {
  if(p.size()<ReplyBytes || le32(p,0)!=ReplyBytes || le32(p,4)!=1 || p[74] || p[75] ||
     le32(p,92)!=p.size()-ReplyBytes || p.size()>PollBytes ||
     (operation!=Poll && p.size()!=ReplyBytes)) throw std::runtime_error("invalid MT7925 reply envelope");
  for(size_t offset: {20u,24u,28u,36u,40u})
    if(le32(p,offset)>1) throw std::runtime_error("invalid reply state");
  Reply r;r.result=int32_t(le32(p,8));r.error=int32_t(le32(p,12));r.stop_error=int32_t(le32(p,16));
  r.ready=le32(p,20)!=0;r.attempted=le32(p,24)!=0;r.stopped=le32(p,28)!=0;r.stage=le32(p,32);
  r.published=le32(p,36)!=0;r.quiescent=le32(p,40)!=0;r.primary=le32(p,44);
  r.center=le32(p,48);r.width=le32(p,52);r.inflight=le32(p,56);r.bands=le32(p,60);
  r.hardware=le32(p,64);r.token=le32(p,88);std::copy_n(p.begin()+68,6,r.mac.begin());
  if(r.ready && (!r.attempted || r.stopped || r.error || r.stop_error || !r.published || r.quiescent || r.stage!=12))
    throw std::runtime_error("inconsistent radio readiness");
  if(p.size()>ReplyBytes) {
    auto record=p.subspan(ReplyBytes);
    if(r.result || record.size()<22 || le16(record,0)!=2 || le16(record,2) ||
       le32(record,4)!=record.size()-8 || le32(record,4)>16384 || record[8] ||
       le16(record,10)<8 || le16(record,10)>record.size()-8)
      throw std::runtime_error("invalid RX record");
    r.record.assign(record.begin(),record.end());
  }
  return r;
}
struct Config {std::array<uint8_t,2> country;uint32_t environment,primary,center,width,fcs;int32_t power;};
inline Bytes start_request(std::span<const uint8_t> bundle,const Config& c) {
  if(bundle.size()<16 || bundle.size()>16+2*8*1024*1024 || le32(bundle,0)!=0x3546574d ||
     le32(bundle,4)!=1 || le32(bundle,8)>8*1024*1024 || le32(bundle,12)>8*1024*1024 ||
     uint64_t(le32(bundle,8))+le32(bundle,12)+16!=bundle.size()) throw std::runtime_error("invalid firmware bundle");
  if(!((c.country[0]=='0' && c.country[1]=='0') || (c.country[0]>='A' && c.country[0]<='Z' && c.country[1]>='A' && c.country[1]<='Z')) ||
     c.environment>2 || c.fcs>1 || c.power < -127 || c.power>126 ||
     !c.primary || c.primary>165 || !c.center || c.center>165 || (c.width!=20 && c.width!=40 && c.width!=80))
    throw std::runtime_error("invalid radio configuration");
  Bytes p(40+bundle.size());put32(p,0,1);put32(p,4,uint32_t(bundle.size()));
  p[8]=c.country[0];p[9]=c.country[1];put32(p,12,c.environment);put32(p,16,c.primary);
  put32(p,20,c.center);put32(p,24,c.width);put32(p,28,c.fcs);put32(p,32,uint32_t(c.power));
  std::copy(bundle.begin(),bundle.end(),p.begin()+40);return p;
}
inline Bytes channel_request(uint32_t primary,uint32_t center,uint32_t width) {
  Bytes p(16);put32(p,0,1);put32(p,4,primary);put32(p,8,center);put32(p,12,width);return p;
}
inline Bytes address_request(std::span<const uint8_t> mac) {
  if(mac.size()!=6 || (mac[0]&1) || std::all_of(mac.begin(),mac.end(),[](auto b){return b==0;}))
    throw std::runtime_error("invalid source address");
  Bytes p(12);put32(p,0,1);std::copy(mac.begin(),mac.end(),p.begin()+4);return p;
}
// Legacy injection fields actually emitted by pinned OWL, plus fixed-size
// radiotap fields0..18. Unknown/extended/MCS namespaces fail instead of being
// interpreted as MPDU bytes. No implicit HT/HE downgrade or channel change.
inline Bytes tx_request(std::span<const uint8_t> p,uint32_t primary) {
  if(p.size()<8 || p[0] || p[1]) throw std::runtime_error("invalid radiotap header");
  const size_t end=le16(p,2);const uint32_t present=le32(p,4);
  if(end<8 || end>p.size() || present&~0x7ffffu) throw std::runtime_error("unsupported radiotap fields");
  constexpr std::array<size_t,19> align={8,1,1,2,2,1,1,2,2,2,1,1,1,1,2,2,1,1,4};
  constexpr std::array<size_t,19> sizes={8,1,1,4,2,1,1,2,2,2,1,1,1,1,2,2,1,1,8};
  uint32_t rate=24,flags=0,txflags=0;size_t offset=8;
  for(size_t bit=0;bit<sizes.size();++bit) if(present&(1u<<bit)) {
    offset=(offset+align[bit]-1)&~(align[bit]-1);
    if(offset>end || sizes[bit]>end-offset) throw std::runtime_error("truncated radiotap field");
    if(bit==1)flags=p[offset];
    if(bit==2)rate=p[offset];
    if(bit==3) {
      uint32_t frequency=primary<=13?2407+5*primary:5000+5*primary;
      if(le16(p,offset)!=frequency) throw std::runtime_error("TX channel differs from current channel");
    }
    if(bit==15)txflags=le16(p,offset);
    offset+=sizes[bit];
  }
  if(flags&~0x10u || txflags&~0x38u) throw std::runtime_error("unsupported TX flags");
  constexpr std::array<uint32_t,8> rates={12,18,24,36,48,72,96,108};
  auto pos=std::find(rates.begin(),rates.end(),rate);
  if(pos==rates.end()) throw std::runtime_error("unsupported legacy PHY rate");
  auto frame=p.subspan(end);
  if(flags&0x10) {
    if(frame.size()<4) throw std::runtime_error("truncated FCS");
    frame=frame.first(frame.size()-4);
  }
  if(frame.size()<24 || frame.size()>4095) throw std::runtime_error("unsupported MPDU length");
  // Kernel checks complete MPDU semantics. The wrapper preserves source and
  // sequence, and multicast never waits for an impossible recipient ACK.
  Bytes out(16+frame.size());put32(out,0,1);put32(out,4,18+uint32_t(pos-rates.begin()));
  put32(out,8,((txflags&8) || (frame[4]&1))?1u:0u);put32(out,12,uint32_t(frame.size()));
  std::copy(frame.begin(),frame.end(),out.begin()+16);return out;
}
} // namespace mt7925
