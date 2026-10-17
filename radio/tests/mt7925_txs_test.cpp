// SPDX-License-Identifier: GPL-2.0-only
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include "mt7925_shutdown.h"
using namespace mt7925;
int failures=0,checks=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL %s\n",label);}}
std::string encode(const Bytes& b){constexpr char h[]="0123456789abcdef";std::string s;for(auto v:b){s+=h[v>>4];s+=h[v&15];}return s;}
Bytes fixture(){
  Bytes b(DiagnosticBytes);put32(b,0,uint32_t(b.size()));put32(b,4,DiagnosticVersion);
  put32(b,1692,1);put32(b,1696,1);put32(b,1700,1);put32(b,1704,1);
  const size_t e=1724,r=e+32;
  put32(b,e,3);put32(b,e+4,8);put32(b,e+8,0xabc);put32(b,e+12,19);put32(b,e+16,20);
  b[e+20]=2;b[e+25]=1;put32(b,e+28,1);
  put32(b,r,0x8000|0x123);put32(b,r+4,(0xabcu<<20)|0xe7);put32(b,r+8,19u<<16);
  put32(b,r+12,3u<<24);put32(b,r+16,12345);
  put32(b,2364,64);put32(b,2368,64);put32(b,2372,64);
  std::copy_n(b.begin()+r,48,b.begin()+2388);return b;
}
std::string decoded(const Bytes& b){std::ostringstream out;write_txs_diagnostic(out,encode(b));return out.str();}
void rejected(const Bytes& b,const char* label){
  bool failed=false;std::ostringstream out;
  try{write_txs_diagnostic(out,encode(b));}catch(const std::exception&){failed=true;}
  check(failed && out.str().empty(),label);
}
struct Model {
  std::string hex;int stops=0,reads=0;
  Reply stop(){++stops;Reply r{};r.stopped=true;r.quiescent=true;return r;}
  std::string diagnostics(){++reads;return hex;}
};
void check_consumer(const Bytes& b){
  Model model{encode(b)};std::ostringstream output,errors;
  finish_session(model,model,{},true,output,errors);
  check(model.stops==1 && model.reads==1,"same STOP/cache consumer");
  check(output.str().find("STOP confirmed")!=std::string::npos,"STOP independent of telemetry");
  check(errors.str().find("TXS firmware packet status JSON:")!=std::string::npos,"native shutdown emits TXS");
  check(errors.str().find("MCU diagnostics unavailable:")==std::string::npos,"native consumer accepted cached ABI");
}
int main(){
  try {
    auto b=fixture();const auto s=decoded(b);
    check(s.find("\"tx_power_raw\":231")!=s.npos,"power stays unsigned raw8");
    check(s.find("\"tx_rate_raw\":291")!=s.npos,"raw PHY rate distinct from requested table");
    check(s.find("\"receiver_hex\":\"020000000001\"")!=s.npos,"request identity preserved");
    check(s.find("firmware_packet_status_not_rf_measurement")!=s.npos,"measurement boundary");
    check_consumer(b);
    for(const auto offset:{0u,4u,1692u,1704u,1724u,1728u,1732u,1736u,1740u,1750u,1752u}) {
      b=fixture();put32(b,offset,0xffffffffu);rejected(b,"one invalid envelope/request field");
    }
    b=fixture();b[1744]=3;rejected(b,"multicast request rejected");
    b=fixture();for(size_t i=1744;i<1750;++i)b[i]=0;rejected(b,"zero receiver rejected");
    b=fixture();b[1804]=1;rejected(b,"unused slot is zero");
    for(const auto offset:{1756u,1760u,1764u,1768u}) {
      b=fixture();put32(b,offset,0);rejected(b,"single broken HOST/seq/WCID/PID correlation");
    }
    b=fixture();put32(b,1756,0x808123);rejected(b,"format1 cannot masquerade as format0");
    b=fixture();put32(b,1704,0);rejected(b,"aggregate match count agrees with entries");
    b=fixture();put32(b,1704,0);put32(b,1752,0);rejected(b,"unmatched slot cannot report a power value");
    for(size_t n:{size_t(0),size_t(1692),DiagnosticBytes-1,DiagnosticBytes+1}) {
      b=fixture();b.resize(n);rejected(b,"exact diagnostic length");
    }
    b=fixture();put32(b,1704,0);put32(b,1752,0);std::fill(b.begin()+1756,b.begin()+1804,uint8_t(0));
    check(decoded(b).find("tx_power_raw")==std::string::npos,"no response reports no invented value");
    b=Bytes(DiagnosticBytes);put32(b,0,uint32_t(b.size()));put32(b,4,DiagnosticVersion);
    check(decoded(b).find("\"requests\":0")!=std::string::npos,"no-TX session remains readable");
    check_consumer(b);
    b=fixture();put32(b,2364,3);rejected(b,"first envelope cannot be shorter than packet type");
    for(const auto offset:{2368u,2372u}) {
      b=fixture();put32(b,offset,0xffffffffu);rejected(b,"first envelope bounds and type");
    }
    char path[4096]{};size_t chars=0;
    if(getenv_s(&chars,path,sizeof(path),"MT_TXS_FIXTURE_OUT"))throw std::runtime_error("fixture path");
    if(chars>1) {
      std::ifstream file(path,std::ios::binary);Bytes actual{std::istreambuf_iterator<char>(file),{}};
      check(actual.size()==DiagnosticBytes,"C runtime ABI loaded");check_consumer(actual);
      check(decoded(actual).find("\"matched\":1")!=std::string::npos,"C matched TXS reaches native JSON");
      std::puts("C session -> TXS receive -> cached IOCTL -> native shutdown consumed");
    }
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
  }catch(const std::exception& e){std::printf("FAIL exception: %s\n",e.what());return 1;}
}
