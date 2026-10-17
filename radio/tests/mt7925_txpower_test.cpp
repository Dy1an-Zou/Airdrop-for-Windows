// SPDX-License-Identifier: GPL-2.0-only
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include "mt7925_shutdown.h"
using namespace mt7925;
int failures=0,checks=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL %s\n",label);}}
void put16(Bytes& b,size_t offset,uint16_t value){b.at(offset)=uint8_t(value);b.at(offset+1)=uint8_t(value>>8);}
std::string encode(const Bytes& b){constexpr char h[]="0123456789abcdef";std::string s;for(auto v:b){s+=h[v>>4];s+=h[v&15];}return s;}
Bytes fixture(){
  Bytes b(DiagnosticBytes);put32(b,0,uint32_t(DiagnosticBytes));put32(b,4,DiagnosticVersion);put32(b,772,1);put32(b,776,1);
  put32(b,784,9);put32(b,788,14);put32(b,792,893);
  const size_t p=796;
  put16(b,p+32,861);put16(b,p+34,0x3800);b[p+36]=0x2a;b[p+37]=9;b[p+38]=2;
  put16(b,p+48,5);put16(b,p+50,841);b[p+52]=5;b[p+55]=1;
  for(size_t r=0;r<417;++r){b[p+56+r*2]=uint8_t(int(r%128)-64);b[p+57+r*2]=uint8_t(63-int(r%128));}
  b[p+890]=37;b[p+891]=uint8_t(-41);return b;
}
std::string decode_power(const Bytes& b){std::ostringstream s;write_txpower_diagnostic(s,encode(b));return s.str();}
struct Model {
  std::string hex;int stops=0,reads=0;
  Reply stop(){++stops;Reply r{};r.stopped=true;r.quiescent=true;return r;}
  std::string diagnostics(){++reads;return hex;}
};
void check_valid(const Bytes& raw,bool captured=false){
  const auto s=decode_power(raw);
  check(s.find("\"measurement\":\"firmware_target_not_rf_measurement\"")!=s.npos,"target not RF measurement");
  check(s.find("\"second_dimension\":\"band_index\"")!=s.npos,"second index is band");
  check(s.find("\"units\":\"raw_signed_8_bit\"")!=s.npos,"raw target units preserved");
  check(s.find("\"event_id\":42")!=s.npos && s.find("\"category\":5")!=s.npos,"observed firmware event and response category");
  if(captured) {
    check(s.find("\"pwr_max\":63,\"pwr_min\":-128")!=s.npos,"captured reported bounds");
    check(s.find("\"ofdm\":[[10,0],[10,0],[10,0],[10,0],[10,0],[10,0],[10,0],[10,0]]")!=s.npos,"captured OFDM 6M raw target 10 band zero");
  } else {
    check(s.find("\"pwr_max\":37,\"pwr_min\":-41")!=s.npos,"signed min max");
    check(s.find("\"cck\":[[-64,63],[-63,62],[-62,61],[-61,60]]")!=s.npos,"distinct band values retained");
    check(s.find("\"eht996x3_484\":[[-47,46]")!=s.npos,"last rate family offset");
    check(s.find("[-32,31]]")!=s.npos,"last table rate retained");
  }
  check(s.find("\"raw_event_hex\":\""+encode(Bytes(raw.begin()+796,raw.begin()+1689))+"\"")!=s.npos,"all 893 raw bytes retained");
  Model model{encode(raw)};std::ostringstream output,errors;
  finish_session(model,model,{},true,output,errors);
  check(model.stops==1 && model.reads==1,"real shutdown consumer reads cached diagnostics once");
  check(errors.str().find("TXPOWER firmware target JSON:")!=s.npos && output.str().find("STOP confirmed")!=s.npos,"shutdown writes raw and decoded power without altering STOP");
}
int main(){
  try {
    check_valid(fixture());
    char path[4096]{};size_t chars=0;
    if(getenv_s(&chars,path,sizeof(path),"MT_TXPOWER_FIXTURE_OUT"))throw std::runtime_error("fixture environment path");
    if(chars>1) {
      std::ifstream file(path,std::ios::binary);Bytes b{std::istreambuf_iterator<char>(file),{}};
      check(b.size()==DiagnosticBytes,"actual C session output loaded");check_valid(b);
      std::puts("C session -> DIAGNOSTICS -> native shutdown fixture consumed");
    }
    if(getenv_s(&chars,path,sizeof(path),"MT_TXPOWER_REAL_FIXTURE_OUT"))throw std::runtime_error("captured fixture environment path");
    if(chars>1) {
      std::ifstream file(path,std::ios::binary);Bytes b{std::istreambuf_iterator<char>(file),{}};
      check(b.size()==DiagnosticBytes,"captured reply C session output loaded");check_valid(b,true);
      std::puts("r32 captured response -> C session -> 2436B DIAGNOSTICS -> native shutdown consumed");
    }
    auto invalid=fixture();put32(invalid,776,0);put32(invalid,780,uint32_t(-1));put32(invalid,788,9);invalid[844]=8;
    auto s=decode_power(invalid);
    check(s.find("\"reason\":\"tag\"")!=s.npos && s.find("\"event_id\":42")!=s.npos,"failed layout retains rejection and actual EID");
    check(s.find("pwr_max")==s.npos && s.find("rates_by_band")==s.npos,"failed reply has no fabricated power values");
    for(const auto offset:{776u,784u,788u,792u,828u,830u,832u,833u,844u,846u,848u,849u,851u}) {
      auto b=fixture();b[offset]=0xff;bool rejected=false;
      try{decode_power(b);}catch(const std::exception&){rejected=true;}
      check(rejected,"inconsistent cached success fails closed");
    }
    invalid=fixture();put32(invalid,776,0);put32(invalid,780,uint32_t(-1));put32(invalid,788,15);invalid[832]=0x2b;
    s=decode_power(invalid);
    check(s.find("\"reason\":\"event_id\"")!=s.npos && s.find("\"event_id\":43")!=s.npos,"rejected EID remains visible");
    check(s.find("rates_by_band")==s.npos,"rejected EID cannot produce rate values");
    for(size_t size:{size_t(0),size_t(772),DiagnosticBytes-1,DiagnosticBytes+1}) {
      auto b=fixture();b.resize(size);bool rejected=false;
      try{decode_power(b);}catch(const std::exception&){rejected=true;}
      check(rejected,"diagnostic ABI exact size");
    }
    invalid=fixture();put32(invalid,776,0);put32(invalid,792,9999);
    check(decode_power(invalid).find("\"raw_truncated\":true")!=s.npos,"oversize failure remains bounded and explicit");
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
  }catch(const std::exception& e){std::printf("FAIL exception: %s\n",e.what());return 1;}
}
