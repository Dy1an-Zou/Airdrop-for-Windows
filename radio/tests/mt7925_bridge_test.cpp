// SPDX-License-Identifier: GPL-2.0-only
#include <cstdio>
#include <deque>
#include <sstream>
#include "mt7925_device.h"
#include "mt7925_bridge.h"
#include "mt7925_shutdown.h"
using namespace mt7925;
int failures=0,checks=0;
void check(bool value,const char* name){++checks;if(!value){++failures;std::printf("FAIL %s\n",name);}}
template<class F> void rejects(F f,const char* name){bool threw=false;try{f();}catch(const std::exception&){threw=true;}check(threw,name);}
const std::array<uint8_t,6> address={2,3,4,5,6,7}, bssid={0,0x25,0,0xff,0x94,0x73};
Bytes frame(){Bytes p(40);p[0]=0xd0;std::fill_n(p.begin()+4,6,uint8_t(0xff));std::copy(address.begin(),address.end(),p.begin()+10);std::copy(bssid.begin(),bssid.end(),p.begin()+16);return p;}
Bytes packet(){Bytes p{0,0,9,0,4,0,0,0,24};auto f=frame();p.insert(p.end(),f.begin(),f.end());return p;}
Bytes wire_reply(int32_t result=0){Bytes p(96);put32(p,0,96);put32(p,4,1);put32(p,8,uint32_t(result));for(auto i:{20u,24u,36u})put32(p,i,1);put32(p,32,12);put32(p,44,6);put32(p,48,6);put32(p,52,20);put32(p,88,0);return p;}
Bytes record(){auto f=packet();Bytes p(8);p[0]=2;put32(p,4,uint32_t(f.size()));p.insert(p.end(),f.begin(),f.end());return p;}
struct Fake {
  uint32_t channel=6;int pending=0,full=0,tx_count=0,polls=0;bool fault=false,wrong_channel=false,forever=false,needs_poll=false,echo=false,bad_air=false;
  std::deque<Bytes> receive;
  Bytes own;int address_pending=0,address_calls=0;bool reject_address=false;
  std::vector<uint32_t> tx_channels;
  Reply call(uint32_t op,std::span<const uint8_t> input={}) {
    auto p=wire_reply();
    if(op==Address) {
      ++address_calls;
      if(address_pending-->0)put32(p,8,uint32_t(-3));
      else if(reject_address)put32(p,8,uint32_t(-1));
      else {
        if(input.size()!=12 || le32(input,0)!=1 || input[10] || input[11])throw std::runtime_error("bad ADDRESS request");
        own.assign(input.begin()+4,input.begin()+10);
      }
    }
    if(op==Channel) {
      if(forever || pending>0) {put32(p,8,uint32_t(-3));if(!needs_poll)--pending;}
      else if(!wrong_channel)channel=le32(input,4);
    }
    if(op==Tx) {
      if(full-->0)put32(p,8,uint32_t(-2));
      else {++tx_count;tx_channels.push_back(channel);if(echo)receive.push_back(record());}
    }
    if(op==Poll) {
      ++polls;
      if(needs_poll && pending>0)--pending;
      if(receive.empty())put32(p,8,uint32_t(bad_air?-1:-3));
      else {auto r=std::move(receive.front());receive.pop_front();put32(p,92,uint32_t(r.size()));p.insert(p.end(),r.begin(),r.end());}
    }
    put32(p,44,channel);put32(p,48,channel);
    if(fault){put32(p,20,0);put32(p,12,uint32_t(-1));}
    return decode(p,op);
  }
};
template<class B> void command(B& bridge,uint16_t type,const Bytes& p){bridge.command({type,0,uint32_t(p.size())},p);}
void codec_tests(){
  auto p=packet();auto request=tx_request(p,6);
  check(le32(request,4)==20 && le32(request,8)==1 && le32(request,12)==40,"OWL legacy rate and multicast");
  check(std::equal(request.begin()+16,request.end(),frame().begin()),"exact frame bytes preserved");
  auto bad=p;bad[8]=2;rejects([&]{tx_request(bad,6);},"CCK not silently changed to OFDM");
  bad=p;bad[7]=0x80;rejects([&]{tx_request(bad,6);},"extended radiotap rejected");
  bad=p;bad[2]=8;rejects([&]{tx_request(bad,6);},"radiotap field beyond declared header");
  bad=p;bad.resize(20);rejects([&]{tx_request(bad,6);},"short MPDU rejected");
  bad=p;bad.resize(9+4096);rejects([&]{tx_request(bad,6);},"oversized MPDU rejected");
  Bytes with_fcs{0,0,10,0,6,0,0,0,0x10,24};auto f=frame();with_fcs.insert(with_fcs.end(),f.begin(),f.end());with_fcs.insert(with_fcs.end(),4,0xee);
  auto stripped=tx_request(with_fcs,6);check(stripped.size()==56 && stripped.back()==f.back(),"FCS stripped once");
  with_fcs[8]=0x40;rejects([&]{tx_request(with_fcs,6);},"bad FCS flag rejected");
  Bytes channel_packet{0,0,14,0,0x0c,0,0,0,24,0,0x6c,9,0x80,0};channel_packet.insert(channel_packet.end(),f.begin(),f.end());
  check(tx_request(channel_packet,1).size()==56,"aligned CHANNEL field decoded");
  rejects([&]{tx_request(channel_packet,6);},"stale channel radiotap rejected");
  auto reply=wire_reply();check(decode(reply,Status).ready,"ready reply decoded");
  for(auto offset:{20u,24u,28u,36u,40u}) {auto b=reply;put32(b,offset,2);rejects([&]{decode(b,Status);},"nonboolean reply state");}
  for(auto offset:{12u,16u,28u,40u}) {auto b=reply;put32(b,offset,1);rejects([&]{decode(b,Status);},"ready cannot mask failure/stop");}
  for(auto offset:{24u,32u,36u}) {auto b=reply;put32(b,offset,0);rejects([&]{decode(b,Status);},"ready requires startup state");}
  auto r=record();put32(reply,92,uint32_t(r.size()));reply.insert(reply.end(),r.begin(),r.end());
  check(decode(reply,Poll).record==r,"RX record exact bytes");
  rejects([&]{decode(reply,Status);},"RX bytes forbidden in status reply");
  auto b=reply;put32(b,92,1);rejects([&]{decode(b,Poll);},"record length checked");
  b=reply;put32(b,8,uint32_t(-3));rejects([&]{decode(b,Poll);},"pending cannot contain delivered frame");
  b=reply;b[98]=1;rejects([&]{decode(b,Poll);},"RX reserved checked");
  b=reply;b[106]=255;rejects([&]{decode(b,Poll);},"RX radiotap boundary checked");
  Bytes bundle(18);put32(bundle,0,0x3546574d);put32(bundle,4,1);put32(bundle,8,1);put32(bundle,12,1);
  Config c{{'C','N'},0,6,6,20,0,10};auto start=start_request(bundle,c);
  check(start.size()==58 && le32(start,32)==10 && le32(start,36)==0,"START config ABI");
  c.power=127;rejects([&]{start_request(bundle,c);},"finite power cap");c.power=10;
  c.country={'c','n'};rejects([&]{start_request(bundle,c);},"country canonical form");
  check(address_request(address)==Bytes({1,0,0,0,2,3,4,5,6,7,0,0}),"ADDRESS exact ABI bytes");
  rejects([&]{address_request(Bytes(6));},"zero identity denied");
  rejects([&]{address_request(Bytes({1,2,3,4,5,6}));},"multicast identity denied");
}
void bridge_tests(){
  Fake fixed_device;Bridge fixed(fixed_device,6,[]{return false;},std::chrono::milliseconds(200),true);
  rejects([&]{command(fixed,RADIO_LINK_SET_CHANNEL,{44});},"fixed trial refuses peer-driven channel change");
  check(fixed_device.channel==6,"refused channel never reaches driver");
  command(fixed,RADIO_LINK_SET_CHANNEL,{6});
  check(fixed_device.channel==6,"fixed trial allows current channel command");
  // Windows Sleep(1) can span a scheduler tick; a 5ms fixture budget makes
  // two legitimate retries fail depending on the machine's timer resolution.
  Fake device;Bridge bridge(device,6,[]{return false;},std::chrono::milliseconds(200));
  rejects([&]{command(bridge,RADIO_LINK_TX,packet());},"TX requires identity");
  device.reject_address=true;
  rejects([&]{command(bridge,RADIO_LINK_SET_ADDRESS,Bytes(address.begin(),address.end()));},"firmware identity rejection surfaced");
  rejects([&]{command(bridge,RADIO_LINK_TX,packet());},"failed identity not published to TX");
  device.reject_address=false;device.address_pending=2;
  command(bridge,RADIO_LINK_SET_ADDRESS,Bytes(address.begin(),address.end()));
  check(device.address_calls==4 && device.own==Bytes(address.begin(),address.end()),"source published only after pending clears and firmware accepts");
  auto invalid=packet();invalid[19]^=2;
  rejects([&]{command(bridge,RADIO_LINK_TX,invalid);},"wrong source rejected");
  device.full=2;command(bridge,RADIO_LINK_TX,packet());check(device.tx_count==1,"backpressure retries once accepted");
  device.pending=2;command(bridge,RADIO_LINK_SET_CHANNEL,{44});command(bridge,RADIO_LINK_TX,packet());
  check(device.tx_channels==std::vector<uint32_t>({6,44}),"channel applies before subsequent TX");
  device.wrong_channel=true;rejects([&]{command(bridge,RADIO_LINK_SET_CHANNEL,{149});},"lying channel reply rejected");device.wrong_channel=false;
  device.forever=true;rejects([&]{command(bridge,RADIO_LINK_SET_CHANNEL,{149});},"channel timeout bounded");device.forever=false;
  command(bridge,RADIO_LINK_SET_BSSID_FILTER,Bytes(bssid.begin(),bssid.end()));
  device.receive.push_back(record());check(bridge.poll()==packet(),"matching BSSID reaches client");
  auto r=record();r[8+9+16]^=2;device.receive.push_back(r);check(bridge.poll().empty(),"wrong BSSID filtered");
  r=record();r[8+9+1]=1;device.receive.push_back(r);check(bridge.poll().empty(),"DS frame not misread as AWDL BSSID");
  check(bridge.poll().empty(),"idle POLL accepted");
  device.bad_air=true;check(bridge.poll().empty(),"bad air packet does not end a healthy session");device.bad_air=false;
  device.fault=true;rejects([&]{bridge.poll();},"poll session fault propagates");
  rejects([&]{Bridge<Fake>::length({RADIO_LINK_TX,1,40});},"reserved envelope rejected");
  rejects([&]{Bridge<Fake>::length({RADIO_LINK_SET_CHANNEL,0,2});},"command length exact");
  rejects([&]{Bridge<Fake>::length({RADIO_LINK_HELLO,0,8});},"client cannot send server message");
  rejects([&]{Bridge<Fake>::length({RADIO_LINK_TX,0,16385});},"payload budget enforced");
}
HANDLE connect(const std::string& name){for(int i=0;i<200;++i){auto h=CreateFileA(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);if(h!=INVALID_HANDLE_VALUE)return h;Sleep(5);}return INVALID_HANDLE_VALUE;}
bool read_all(HANDLE h,void* bytes,DWORD n){auto p=static_cast<uint8_t*>(bytes);while(n){DWORD got=0;if(!ReadFile(h,p,n,&got,nullptr)||!got)return false;p+=got;n-=got;}return true;}
void pipe_tests(){
  std::atomic<bool> stopped=false;
  const auto name=std::string("\\\\.\\pipe\\mt7925-bridge-test-")+std::to_string(GetCurrentProcessId());
  PipeServer pipe(name,[&]{return stopped.load();});check(pipe.valid(),"real Windows pipe created");
  Fake device;device.needs_poll=true;device.pending=2;device.echo=true;
  Bridge session(device,6,[&]{return stopped.load();});std::exception_ptr error;
  std::thread server([&]{try{serve(pipe,session,6,stopped);}catch(...){error=std::current_exception();}});
  auto probe=connect(name);check(probe!=INVALID_HANDLE_VALUE,"probe connects");if(probe!=INVALID_HANDLE_VALUE)CloseHandle(probe);Sleep(30);
  auto client=connect(name);check(client!=INVALID_HANDLE_VALUE,"real client follows probe");
  if(client!=INVALID_HANDLE_VALUE){
    radio_link_header h{};radio_link_hello hello{};
    check(read_all(client,&h,sizeof(h)) && h.type==1 && h.length==8 && read_all(client,&hello,sizeof(hello)) && hello.version==3 && hello.width_mhz==20,"client sees protocol3 HELLO");
    Bytes p(8);p[0]=6;put32(p,4,6);p.insert(p.end(),address.begin(),address.end());
    Bytes hop{4,0,0,0,1,0,0,0,44};p.insert(p.end(),hop.begin(),hop.end());
    auto tx=packet();Bytes header(8);header[0]=3;put32(header,4,uint32_t(tx.size()));p.insert(p.end(),header.begin(),header.end());p.insert(p.end(),tx.begin(),tx.end());
    DWORD wrote=0;check(WriteFile(client,p.data(),DWORD(p.size()),&wrote,nullptr) && wrote==p.size(),"actual client sends identity and TX");
    radio_link_header rx{};check(read_all(client,&rx,sizeof(rx)) && rx.type==2 && rx.length==tx.size(),"real RX pipe header");
    if(rx.length<=RADIO_LINK_MAX_PAYLOAD){Bytes data(rx.length);check(read_all(client,data.data(),DWORD(data.size())) && data==tx,"actual RX frame reaches client intact");}
    // Close only after kernel pipe has delivered written data to the reader.
    FlushFileBuffers(client);Sleep(30);CloseHandle(client);
  } else stopped=true;
  server.join();check(!error,"pipe consumer exits without error");check(device.tx_count==1 && device.polls>0 && device.tx_channels==std::vector<uint32_t>{44},"real pipe reaches driver TX after hop while servicing idle MCU");check(stopped,"real client departure stops generation");
  check(device.own==Bytes(address.begin(),address.end()),"real pipe source command reaches firmware adapter");
}
void hash_tests(){
  auto path=std::filesystem::temp_directory_path()/("mt7925-hash-test-"+std::to_string(GetCurrentProcessId())+".bin");
  {std::ofstream file(path,std::ios::binary);file<<"abc";}
  check(pinned_image(path,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")==Bytes({'a','b','c'}),"actual Windows SHA256 validates pinned bytes");
  rejects([&]{pinned_image(path,std::string(64,'0'));},"different firmware hash rejected");
  std::filesystem::remove(path);
}
void stop_tests(){
  {
    auto path=std::filesystem::temp_directory_path()/("mt7925-stop-"+std::to_string(GetCurrentProcessId())+".flag");
    check(!stop_file_present({}) && !stop_file_present(path),"absent stop file continues");
    {std::ofstream file(path);file<<"stop";}
    check(stop_file_present(path),"supervisor file requests stop");std::filesystem::remove(path);
  }
  {
    Fake device;Bridge session(device,6,[]{return true;});
    const radio_link_header h{RADIO_LINK_SET_CHANNEL,0,1};
    bool requested=false;
    try {session.command(h,Bytes{6});}catch(const StopRequested&){requested=true;}
    check(requested,"requested stop is distinct from driver failure");
    check(device.tx_count==0,"requested stop publishes no packet");
  }
  const auto name=std::string("\\\\.\\pipe\\mt7925-stop-test-")+std::to_string(GetCurrentProcessId());
  {
    PipeServer pipe(name,[]{return false;});auto client=connect(name);
    check(client!=INVALID_HANDLE_VALUE && pipe.wait_for_client(),"stalled client connected");
    auto began=std::chrono::steady_clock::now();bool failed=false;
    for(int i=0;i<12;++i)if(!pipe.send(RADIO_LINK_RX,Bytes(16384),100)){failed=true;break;}
    check(failed && std::chrono::steady_clock::now()-began<std::chrono::seconds(3),"stalled writes cancel within deadline");
    if(client!=INVALID_HANDLE_VALUE)CloseHandle(client);
  }
  {
    std::atomic<bool> stopped=false;PipeServer pipe(name,[&]{return stopped.load();});
    Fake device;device.fault=true;Bridge session(device,6,[&]{return stopped.load();});
    auto began=std::chrono::steady_clock::now();
    rejects([&]{serve(pipe,session,6,stopped);},"worker fault cancels pipe accept");
    check(stopped && std::chrono::steady_clock::now()-began<std::chrono::seconds(3),"fault shutdown bounded");
  }
}
void capture_tests(){
  const auto path=std::filesystem::temp_directory_path()/("mt7925-capture-"+std::to_string(GetCurrentProcessId())+".pcap");
  {
    FrameCapture capture(path.string());
    check(capture.is_open(),"diagnostic file created");
    FrameCapture collision(path.string());
    check(!collision.is_open(),"existing evidence never overwritten");
    Fake device;Bridge bridge(device,6,[]{return false;},std::chrono::milliseconds(200),true,&capture);
    device.receive.push_back(record());bridge.poll(); // Before BSSID: not captured.
    rejects([&]{command(bridge,RADIO_LINK_TX,packet());},"capture does not bypass TX identity");
    command(bridge,RADIO_LINK_SET_ADDRESS,Bytes(address.begin(),address.end()));
    command(bridge,RADIO_LINK_SET_BSSID_FILTER,Bytes(bssid.begin(),bssid.end()));
    auto other=record();other[8+9+16]^=2;device.receive.push_back(other);bridge.poll();
    device.receive.push_back(record());check(bridge.poll()==packet(),"capture leaves RX intact");
    device.full=2;command(bridge,RADIO_LINK_TX,packet());
    check(device.tx_count==1,"capture records one accepted TX after retries");
    const auto& d=bridge.rx_diagnostics();
    check(d.records==3 && d.returned_frames==2 && d.capture_submitted==1 && d.normal_returns==3,
          "capture submissions separate passed-before-BSSID from filtered and captured RX");
  }
  std::ifstream input(path,std::ios::binary);Bytes bytes{std::istreambuf_iterator<char>(input),{}};input.close();
  const auto expected=packet();const size_t second=24+16+expected.size();
  check(bytes.size()==24+2*(16+expected.size()),"only filtered RX and accepted TX captured");
  check(bytes.size()>=second+16+expected.size() &&
    std::equal(expected.begin(),expected.end(),bytes.begin()+40) &&
    std::equal(expected.begin(),expected.end(),bytes.begin()+second+16),"capture preserves both radiotap packets");
  std::filesystem::remove(path);
  {
    FrameCapture capture(path.string(),24+16+expected.size());
    capture.radiotap_frame(Bytes{0,0,0,0,0,0,0,0});
    capture.radiotap_frame(expected);capture.radiotap_frame(expected);
  }
  check(std::filesystem::file_size(path)==24+16+expected.size(),"capture byte budget omits whole excess records and invalid headers");
  std::filesystem::remove(path);
}
void receive_backlog_tests(){
  Fake device;std::atomic<bool> stopped=false;
  Bridge bridge(device,6,[]{return false;});
  command(bridge,RADIO_LINK_SET_BSSID_FILTER,Bytes(bssid.begin(),bssid.end()));
  auto unrelated=record();unrelated[8+9+16]^=2;
  for(int i=0;i<100;++i)device.receive.push_back(unrelated);
  device.receive.push_back(record());
  int waits=0,delivered=0;
  auto pause=[&]{++waits;};
  receive_loop(bridge,stopped,[&](const Bytes& p){check(p==packet(),"backlog preserves AWDL packet");++delivered;stopped=true;},pause);
  check(delivered==1 && device.polls==101,"filtered backlog drains without losing target frame");
  check(waits==3,"100 unrelated frames incur three batch waits, not 100 packet sleeps");
  check(device.polls==101,"stop from delivery prevents another poll or wait");
  Fake idle;Bridge idle_bridge(idle,6,[]{return false;});stopped=false;waits=0;
  auto stop_after_two=[&]{if(++waits==2)stopped=true;};
  receive_loop(idle_bridge,stopped,[&](const Bytes&){check(false,"idle never delivers");},stop_after_two);
  check(idle.polls==64 && waits==2,"empty ring still yields every bounded batch");
  Fake broken;broken.fault=true;Bridge bad(broken,6,[]{return false;});stopped=false;waits=0;
  rejects([&]{receive_loop(bad,stopped,[](const Bytes&){},pause);},"receive fault terminates batch immediately");
  check(broken.polls==1 && waits==0,"receive fault does not spin or suppress failure");
}
Bytes rx_record(const Bytes& f,size_t rt=9) {
  Bytes p(8+rt);p[0]=2;put32(p,4,uint32_t(rt+f.size()));
  p[10]=uint8_t(rt);p[11]=uint8_t(rt>>8);p.insert(p.end(),f.begin(),f.end());return p;
}
void rx_diagnostic_tests(){
  Fake device;Bridge bridge(device,6,[]{return false;});
  auto feed=[&](const Bytes& f,size_t rt=9){device.receive.push_back(rx_record(f,rt));return bridge.poll();};
  check(bridge.poll().empty(),"diagnostic pending poll remains empty");
  device.bad_air=true;check(bridge.poll().empty(),"diagnostic malformed air poll remains empty");device.bad_air=false;
  auto unrelated=frame();unrelated[16]^=2;
  check(!feed(unrelated).empty(),"before BSSID unrelated frame still passes original filter");
  check(bridge.rx_diagnostics().classes[RxDiagnostics::Passed].first[0].bytes==0,
        "before BSSID unrelated addresses and payload are not retained");
  auto qos=frame();qos[0]=0x88;std::fill(qos.begin()+26,qos.end(),uint8_t(0xef));
  check(!feed(qos).empty(),"before BSSID AWDL data still passes");
  const auto first=bridge.rx_diagnostics().classes[RxDiagnostics::Passed].first[2];
  check(first.bytes==26 && first.awdl_bssid && !first.local_ra && first.frame_bytes==40,
        "AWDL QoS sample before identity contains only its 26-byte header");
  command(bridge,RADIO_LINK_SET_ADDRESS,Bytes(address.begin(),address.end()));
  command(bridge,RADIO_LINK_SET_BSSID_FILTER,Bytes(bssid.begin(),bssid.end()));
  check(!feed(frame()).empty(),"matching management frame unchanged by diagnostics");
  auto local=unrelated;std::copy(address.begin(),address.end(),local.begin()+4);
  std::fill(local.begin()+24,local.end(),uint8_t(0xab));
  check(feed(local).empty(),"local RA with wrong BSSID remains filtered");
  auto private_data=unrelated;private_data[0]=0x08;
  check(feed(private_data).empty(),"unrelated data with wrong BSSID remains filtered");
  auto ds=qos;ds[1]=0x83;std::copy(address.begin(),address.end(),ds.begin()+4);
  std::fill(ds.begin()+24,ds.begin()+36,uint8_t(0));
  std::fill(ds.begin()+36,ds.end(),uint8_t(0xcd));
  check(feed(ds).empty(),"four-address QoS HT frame remains rejected");
  auto version=local;version[0]|=1;check(feed(version).empty(),"nonzero protocol remains rejected first");
  auto ack=local;ack[0]=0xd4;check(feed(ack).empty(),"control frame remains filtered");
  auto extension=local;extension[0]=0x0c;check(feed(extension).empty(),"extension remains filtered");
  ack.resize(10);check(feed(ack).empty(),"short ACK remains short-header filtered");
  check(feed({},14).empty(),"valid record with empty MAC frame remains filtered");
  // The old bridge accepts 24-byte MAC headers; diagnostics must not tighten it.
  auto short_qos=qos;short_qos.resize(25);check(!feed(short_qos).empty(),"diagnostics do not add QoS length admission");
  for(int i=0;i<40;++i){qos[22]=uint8_t(i);check(!feed(qos).empty(),"repeated related frame preserves consumer output");}
  const auto& d=bridge.rx_diagnostics();
  check(d.poll_calls==54 && d.normal_returns==54 && d.pending_returns==1 && d.invalid_air_returns==1 && d.empty_returns==0,
        "normal poll returns close pending invalid and every valid record");
  check(d.records==52 && d.returned_frames==44 && d.capture_submitted==0,"record and returned frame totals close without a capture sink");
  uint64_t classified=0;for(const auto& c:d.classes)classified+=c.count;
  check(classified==d.records,"every valid record has exactly one final reason");
  const auto& passed=d.classes[RxDiagnostics::Passed];
  check(passed.count==44 && passed.fc[13]==2 && passed.fc[40]==42 && passed.ds[0]==44 && passed.filter_bssid[0]==2 && passed.filter_bssid[1]==42,
        "passed classification includes before-BSSID window and exact FC and DS");
  check(passed.ra[0]==2 && passed.ra[2]==42 && passed.unrelated==1,"identity-unset is distinct from nonlocal RA");
  check(passed.first[2].bytes==26 && passed.first[2].header==first.header,"bounded first header cannot be replaced by later related payloads");
  const auto& b=d.classes[RxDiagnostics::Bssid];
  check(b.count==2 && b.ra[1]==1 && b.ra[2]==1 && b.filter_bssid[2]==2 && b.unrelated==1,
        "BSSID rejects distinguish related local RA from unrelated traffic");
  check(b.first[0].bytes==24 && b.first[0].local_ra && !b.first[0].awdl_bssid && b.first[2].bytes==0,
        "BSSID sample contains local management header only and no unrelated data address");
  const auto& s=d.classes[RxDiagnostics::DsForm].first[2];
  check(s.bytes==32 && s.frame_bytes==40 && s.local_ra && !s.awdl_bssid && std::equal(s.header.begin(),s.header.end(),ds.begin()),
        "related four-address QoS HT header is capped at 32 without payload");
  check(d.classes[RxDiagnostics::FrameType].count==2 && d.classes[RxDiagnostics::FrameType].first[1].bytes==10 && d.classes[RxDiagnostics::FrameType].first[3].bytes==0,
        "padded ACK stores exactly ten header bytes and unknown extension stores none");
  check(d.classes[RxDiagnostics::ProtocolVersion].count==1 && d.classes[RxDiagnostics::ProtocolVersion].first[0].bytes==0,
        "unknown protocol layout never sampled");
  check(d.classes[RxDiagnostics::ShortHeader].count==2 && d.classes[RxDiagnostics::ShortHeader].fc[64]==1 &&
        d.classes[RxDiagnostics::ShortHeader].ra[3]==1 && d.classes[RxDiagnostics::ShortHeader].first[1].bytes==10,
        "empty and short ACK records are classified without out-of-bounds access");
  std::ostringstream json;d.write_json(json);const auto text=json.str();
  check(text.find("\"normal_returns\":54")!=std::string::npos && text.find("\"reason\":\"bssid\",\"count\":2")!=std::string::npos,
        "machine readable output contains closure totals and actual final reason");
  check(text.find("abababab")==std::string::npos && text.find("efefefef")==std::string::npos && text.find("cdcdcdcd")==std::string::npos,
        "diagnostic JSON never emits any of the payload sentinels");
  std::puts(("RX_DIAGNOSTIC_TEST_JSON="+text).c_str());
  device.fault=true;rejects([&]{bridge.poll();},"diagnostic counters do not suppress driver errors");
  check(d.poll_calls==55 && d.normal_returns==54,"throwing poll never counted as normal return");
  {
    Fake other;Bridge b2(other,6,[]{return false;});
    command(b2,RADIO_LINK_SET_BSSID_FILTER,Bytes(bssid.begin(),bssid.end()));
    // From-DS BSSID is addr2, even though addr3 differs. This is relevant but still filtered.
    auto from_ds=unrelated;from_ds[0]=0x08;from_ds[1]=2;std::copy(bssid.begin(),bssid.end(),from_ds.begin()+10);
    other.receive.push_back(rx_record(from_ds));check(b2.poll().empty(),"real BSSID relevance does not loosen DS rejection");
    const auto& c=b2.rx_diagnostics().classes[RxDiagnostics::DsForm];
    check(c.count==1 && c.ds[2]==1 && c.filter_bssid[2]==1 && c.first[2].awdl_bssid && c.first[2].bytes==24,
          "From-DS sample uses actual BSSID while filter statistics explicitly use addr3");
  }
  {
    Fake other;Bridge b2(other,6,[]{return false;});
    other.receive.push_back(rx_record(short_qos));check(!b2.poll().empty(),"truncated QoS retains old accepted behavior");
    check(b2.rx_diagnostics().classes[RxDiagnostics::Passed].first[2].bytes==0,"incomplete QoS header never copies beyond its complete header");
  }
}
struct ShutdownDriver {
  Reply end;
  bool stop_throws=false,diagnostics_throw=false;
  int starts=0,stops=0,reads=0;
  std::string diagnostic_hex;
  ShutdownDriver(){
    end.stopped=true;end.quiescent=true;
    // A complete ABI12 cache with no power query requested, not a parser error.
    Bytes d(DiagnosticBytes);put32(d,0,uint32_t(DiagnosticBytes));put32(d,4,DiagnosticVersion);
    constexpr char digits[]="0123456789abcdef";
    for(auto b:d){diagnostic_hex+=digits[b>>4];diagnostic_hex+=digits[b&15];}
  }
  Reply call(uint32_t op,std::span<const uint8_t>) {
    if(op==Start) {
      ++starts;Reply failed;failed.stage=8;failed.result=-42;failed.error=-42;return failed;
    }
    if(op!=Stop)throw std::runtime_error("unexpected diagnostic test operation");
    ++stops;
    if(stop_throws)throw std::runtime_error("STOP transport failed");
    return end;
  }
  std::string diagnostics(){
    ++reads;
    if(diagnostics_throw)throw std::runtime_error("read-only diagnostics failed");
    return diagnostic_hex;
  }
};
std::string finish_model(ShutdownDriver& driver,bool start_failure,bool capture,
                         std::ostringstream& output,std::ostringstream& errors) {
  Bridge session(driver,6,[]{return false;});std::exception_ptr failure;
  if(start_failure)try {session.start({});}catch(...){failure=std::current_exception();}
  try {finish_session(session,driver,failure,capture,output,errors);}
  catch(const std::exception& e){return e.what();}
  return {};
}
void shutdown_diagnostic_tests(){
  const std::string original="radio not ready: stage=8 result=-42 error=-42 stop_error=0";
  {
    ShutdownDriver driver;driver.end.result=-1;driver.end.stop_error=-14;
    driver.end.stopped=false;driver.end.quiescent=false;driver.end.stage=8;driver.end.error=-42;
    std::ostringstream output,errors;
    check(finish_model(driver,true,false,output,errors)==original,"START error survives unconfirmed STOP");
    check(driver.starts==1 && driver.stops==1 && driver.reads==1,"failed START still attempts STOP and read-only diagnostics exactly once");
    check(errors.str().find(original)<errors.str().find("Radio STOP reply:"),"original error is recorded before cleanup result");
    check(errors.str().find("result=-1 error=-42 stop_error=-14 stopped=0 quiescent=0 ready=0 stage=8")!=std::string::npos,"failed STOP reply retains exact state and error values");
    check(errors.str().find("MCU diagnostics LE hex: "+driver.diagnostic_hex)!=std::string::npos,"failed STOP still records read-only diagnostics");
    check(errors.str().find("\"reason\":\"not_requested\"")!=std::string::npos && errors.str().find("MCU diagnostics unavailable:")==std::string::npos,"failed STOP decodes a complete not-requested cache without fabricating power");
    check(output.str().find("STOP confirmed")==std::string::npos,"failed STOP never claims quiescence");
  }
  {
    ShutdownDriver driver;driver.stop_throws=true;driver.diagnostics_throw=true;
    std::ostringstream output,errors;
    check(finish_model(driver,true,false,output,errors)==original,"START error survives STOP and diagnostics exceptions");
    check(driver.stops==1 && driver.reads==1,"throwing STOP still permits one diagnostic read");
    check(errors.str().find("STOP transport failed")!=std::string::npos && errors.str().find("read-only diagnostics failed")!=std::string::npos,"both secondary failures are preserved");
  }
  // Each existing STOP success condition must independently remain mandatory.
  for(int field=0;field<5;++field) {
    ShutdownDriver driver;
    if(field==0)driver.end.result=-1;
    if(field==1)driver.end.stop_error=-2;
    if(field==2)driver.end.stopped=false;
    if(field==3)driver.end.quiescent=false;
    if(field==4)driver.end.ready=true;
    std::ostringstream output,errors;
    check(finish_model(driver,false,false,output,errors).find("STOP did not confirm quiescence")!=std::string::npos,"each STOP failure remains fatal without an earlier error");
    check(driver.reads==1,"every unconfirmed STOP retains diagnostics without capture enabled");
    check(output.str().empty(),"an independently invalid STOP never emits success");
  }
  {
    ShutdownDriver driver;driver.stop_throws=true;driver.diagnostics_throw=true;
    std::ostringstream output,errors;
    check(finish_model(driver,false,false,output,errors)=="STOP transport failed","diagnostic failure never masks STOP transport error");
    check(driver.reads==1,"STOP transport failure attempts read-only diagnostic retrieval");
  }
  for(bool capture:{false,true}) {
    ShutdownDriver driver;std::ostringstream output,errors;
    check(finish_model(driver,false,capture,output,errors).empty(),"confirmed STOP preserves successful exit");
    check(driver.stops==1 && driver.reads==int(capture),"successful no-capture shutdown does not add a diagnostic call");
    check(output.str().find("STOP confirmed")!=std::string::npos,"confirmed STOP retains success report");
    check(errors.str().find("result=0 error=0 stop_error=0 stopped=1 quiescent=1 ready=0")!=std::string::npos,"successful STOP also records reply values");
  }
  for(bool read_failure:{false,true}) {
    ShutdownDriver driver;driver.diagnostics_throw=read_failure;
    if(!read_failure)driver.diagnostic_hex[8]='f'; // Correct size, wrong ABI version.
    std::ostringstream output,errors;
    check(finish_model(driver,false,true,output,errors).empty(),"diagnostic read or parse failure cannot invalidate a confirmed STOP");
    check(driver.stops==1 && driver.reads==1 && output.str().find("STOP confirmed")!=std::string::npos,"diagnostic failure preserves explicit quiescence and one read");
    check(errors.str().find(read_failure?"read-only diagnostics failed":"TXPOWER diagnostic ABI")!=std::string::npos && errors.str().find("TXPOWER firmware target JSON:")==std::string::npos,"diagnostic failure is explicit and never emits decoded power");
    if(!read_failure)check(errors.str().find("MCU diagnostics LE hex: "+driver.diagnostic_hex)!=std::string::npos,"parse failure preserves the complete raw diagnostic first");
  }
  for(bool start_failure:{false,true}) {
    ShutdownDriver driver;driver.end.quiescent=false;driver.diagnostic_hex[8]='f';
    std::ostringstream output,errors;
    const auto failure=finish_model(driver,start_failure,false,output,errors);
    check(start_failure?failure==original:failure.find("STOP did not confirm quiescence")!=std::string::npos,"parse failure preserves the first operation or STOP error");
    check(driver.stops==1 && driver.reads==1 && output.str().find("STOP confirmed")==std::string::npos,"parse failure cannot promote an unconfirmed STOP to success");
    check(errors.str().find("MCU diagnostics LE hex: "+driver.diagnostic_hex)!=std::string::npos && errors.str().find("TXPOWER diagnostic ABI")!=std::string::npos,"unconfirmed STOP retains both raw data and parsing failure");
  }
  {
    ShutdownDriver driver;driver.diagnostics_throw=true;std::ostringstream output,errors;
    check(finish_model(driver,true,false,output,errors)==original,"diagnostic exception after confirmed STOP preserves START cause");
  }
}
int main(){
  std::setvbuf(stdout,nullptr,_IONBF,0);
  try {
    std::puts("codec");codec_tests();std::puts("bridge");bridge_tests();
    std::puts("hash");hash_tests();std::puts("pipe");pipe_tests();std::puts("stop");stop_tests();
    std::puts("capture");capture_tests();
    std::puts("receive backlog");receive_backlog_tests();
    std::puts("RX classification diagnostics");rx_diagnostic_tests();
    std::puts("shutdown diagnostics");shutdown_diagnostic_tests();
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
  } catch(const std::exception& e){std::printf("ERROR %s\n",e.what());return 2;}
}
