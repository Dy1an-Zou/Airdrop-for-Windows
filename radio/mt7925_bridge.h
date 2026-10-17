// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <filesystem>
#include <mutex>
#include <thread>
#include "mt7925_codec.h"
#include "pipe_server.h"
#include "frame_capture.h"
#include "mt7925_rx_diagnostics.h"

namespace mt7925 {
class StopRequested : public std::exception {
public:
  const char* what() const noexcept override {return "radio stop requested";}
};
inline bool stop_file_present(const std::filesystem::path& path) {
  return !path.empty() && std::filesystem::exists(path);
}
// The production Device and offline model share the exact bridge consumer.
// Only one command reader is allowed; POLL is serviced by a separate thread.
template<class Driver> class Bridge {
  Driver& driver_;
  std::mutex io_, policy_;
  uint32_t primary_;
  bool fixed_channel_;
  std::array<uint8_t,6> source_{}, bssid_{};
  bool identity_=false;
  std::function<bool()> stopping_;
  std::chrono::milliseconds budget_;
  FrameCapture* capture_;
  RxDiagnostics rx_;
  Reply call(uint32_t op,std::span<const uint8_t> p={}) {
    std::lock_guard lock(io_);return driver_.call(op,p);
  }
  static void healthy(const Reply& r) {
    if(!r.ready || r.error || r.stop_error)throw std::runtime_error(
      "radio not ready: stage="+std::to_string(r.stage)+" result="+std::to_string(r.result)+
      " error="+std::to_string(r.error)+" stop_error="+std::to_string(r.stop_error));
  }
  Reply retry(uint32_t op,const Bytes& input,int32_t pending) {
    const auto until=std::chrono::steady_clock::now()+budget_;
    for(;;) {
      if(stopping_())throw StopRequested();
      auto r=call(op,input);healthy(r);
      if(!r.result)return r;
      if(r.result!=pending)throw std::runtime_error("radio operation failed: "+std::to_string(r.result));
      if(std::chrono::steady_clock::now()>=until)throw std::runtime_error("radio backpressure timeout");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
public:
  Bridge(Driver& driver,uint32_t primary,std::function<bool()> stopping,
         std::chrono::milliseconds budget=std::chrono::milliseconds(2000),bool fixed_channel=false,
         FrameCapture* capture=nullptr):
    driver_(driver),primary_(primary),fixed_channel_(fixed_channel),stopping_(std::move(stopping)),budget_(budget),capture_(capture){}
  Reply start(const Bytes& request) {
    auto r=call(Start,request);healthy(r);
    if(r.result || r.primary!=primary_ || r.center!=primary_ || r.width!=20)
      throw std::runtime_error("unexpected start result");
    return r;
  }
  Reply stop(){return call(Stop);}
  // Only inspect after the receive worker has joined (serve guarantees this).
  const RxDiagnostics& rx_diagnostics() const noexcept {return rx_;}
  static uint32_t length(const radio_link_header& h) {
    if(h.reserved || h.length>RADIO_LINK_MAX_PAYLOAD ||
       !((h.type==RADIO_LINK_TX && h.length>=32) ||
         (h.type==RADIO_LINK_SET_CHANNEL && h.length==1) ||
         ((h.type==RADIO_LINK_SET_BSSID_FILTER || h.type==RADIO_LINK_SET_ADDRESS) && h.length==6)))
      throw std::runtime_error("invalid radio-link command envelope");
    return h.length;
  }
  void command(const radio_link_header& h,std::span<const uint8_t> p) {
    if(length(h)!=p.size())throw std::runtime_error("short radio-link command");
    if(h.type==RADIO_LINK_SET_ADDRESS) {
      retry(Address,address_request(p),-3);
      std::lock_guard lock(policy_);std::copy(p.begin(),p.end(),source_.begin());identity_=true;
      // Publish local identity only after the firmware command is acknowledged.
      // This does not prove that a peer observed an automatic receiver ACK.
    } else if(h.type==RADIO_LINK_SET_BSSID_FILTER) {
      std::lock_guard lock(policy_);std::copy(p.begin(),p.end(),bssid_.begin());
    } else if(h.type==RADIO_LINK_SET_CHANNEL) {
      const uint32_t ch=p[0];
      if(!ch || ch>165)throw std::runtime_error("invalid channel");
      if(fixed_channel_ && ch!=primary_)throw std::runtime_error("trial channel is fixed; channel change refused");
      auto r=retry(Channel,channel_request(ch,ch,20),-3);
      if(r.primary!=ch || r.center!=ch || r.width!=20)throw std::runtime_error("channel was not applied");
      primary_=ch;
    } else {
      auto input=tx_request(p,primary_);
      {
        std::lock_guard lock(policy_);
        if(!identity_ || !std::equal(source_.begin(),source_.end(),input.begin()+26))
          throw std::runtime_error("TX source differs from declared address");
      }
      auto r=retry(Tx,input,-2);
      if(r.primary!=primary_ || r.token>=256)throw std::runtime_error("invalid TX acceptance");
      if(capture_)capture_->transmitted(p);
    }
  }
  Bytes poll() {
    ++rx_.poll_calls;
    auto r=call(Poll);healthy(r);
    // With a fixed valid POLL envelope and full-sized buffers, INVALID while
    // still healthy is a malformed on-air packet, not a failed transport.
    if((r.result==-3 || r.result==-1) && r.record.empty()){rx_.empty(r.result);return {};}
    if(r.result)throw std::runtime_error("radio polling failed");
    if(r.record.empty()){rx_.empty(0);return {};}
    auto p=std::span<const uint8_t>(r.record).subspan(8);
    const size_t rt=le16(p,2);
    std::lock_guard lock(policy_);
    auto dropped=[&](RxDiagnostics::Reason reason,std::span<const uint8_t> frame) {
      rx_.record(reason,frame,identity_,source_,bssid_);++rx_.normal_returns;return Bytes{};
    };
    if(rt>p.size() || p.size()-rt<24)
      return dropped(RxDiagnostics::ShortHeader,rt<=p.size()?p.subspan(rt):std::span<const uint8_t>{});
    auto frame=p.subspan(rt);
    // AWDL management/data frames use the three-address, non-DS form.
    if(frame[0]&3)return dropped(RxDiagnostics::ProtocolVersion,frame);
    if(frame[1]&3)return dropped(RxDiagnostics::DsForm,frame);
    if((frame[0]&12)==4 || (frame[0]&12)==12)return dropped(RxDiagnostics::FrameType,frame);
    if(std::any_of(bssid_.begin(),bssid_.end(),[](auto b){return b!=0;}) &&
       !std::equal(bssid_.begin(),bssid_.end(),frame.begin()+16))return dropped(RxDiagnostics::Bssid,frame);
    rx_.record(RxDiagnostics::Passed,frame,identity_,source_,bssid_);
    // Do not capture unrelated traffic before the client installs its BSSID.
    if(capture_ && std::any_of(bssid_.begin(),bssid_.end(),[](auto b){return b!=0;})) {
      ++rx_.capture_submitted;capture_->radiotap_frame(p);
    }
    Bytes result(p.begin(),p.end());++rx_.normal_returns;++rx_.returned_frames;return result;
  }
};

// Windows Sleep(1) can take a full scheduler tick. Sleeping after every poll
// capped a busy monitor ring at ~64 packets/s, including discarded traffic.
// Drain bounded batches, then yield with a high-resolution 1 ms timer.
class PollPause {
  HANDLE timer_;
public:
  PollPause():timer_(CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS)) {
    if(!timer_)throw std::runtime_error("cannot create receive pacing timer");
  }
  ~PollPause(){CloseHandle(timer_);}
  PollPause(const PollPause&)=delete;
  PollPause& operator=(const PollPause&)=delete;
  void operator()() {
    LARGE_INTEGER due;due.QuadPart=-10000;
    if(!SetWaitableTimer(timer_,&due,0,nullptr,nullptr,FALSE) || WaitForSingleObject(timer_,1000)!=WAIT_OBJECT_0)
      throw std::runtime_error("receive pacing timer failed");
  }
};
template<class Session,class Deliver,class Pause>
void receive_loop(Session& session,std::atomic<bool>& stopped,Deliver deliver,Pause& pause) {
  while(!stopped) {
    // Empty output includes filtered frames, so it cannot mean the ring is empty.
    for(unsigned i=0;i<32 && !stopped;++i) {
      auto rx=session.poll();
      if(!rx.empty())deliver(rx);
    }
    if(!stopped)pause();
  }
}

// A probe that sends no complete command may reconnect. Once a client sends
// commands, departure ends this one hardware generation. No automatic restart.
template<class Session> void serve(PipeServer& pipe,Session& session,uint8_t primary,
                                  std::atomic<bool>& stopped) {
  std::mutex route;
  bool deliver=false;
  std::exception_ptr worker_error;
  std::thread worker([&] {
    try {
      PollPause pause;
      receive_loop(session,stopped,[&](const Bytes& rx) {
          std::lock_guard lock(route);
          if(deliver && !pipe.send(RADIO_LINK_RX,rx,500)) {
            if(!stopped)throw std::runtime_error("client RX pipe closed or stalled");
          }
      },pause);
    } catch(...) {worker_error=std::current_exception();stopped=true;}
  });
  std::exception_ptr reader_error;
  try {
    while(!stopped) {
      if(!pipe.wait_for_client()) {if(!stopped)throw std::runtime_error("pipe accept failed");break;}
      bool hello=false,active=false;
      {
        std::lock_guard lock(route);
        const radio_link_hello h{RADIO_LINK_VERSION,primary,20,{0,0}};
        hello=pipe.send(RADIO_LINK_HELLO,{reinterpret_cast<const uint8_t*>(&h),sizeof(h)},500);
      }
      while(hello && !stopped) {
        radio_link_header h{};
        if(!pipe.read_exact(&h,sizeof(h)))break;
        active=true;
        Bytes p(Session::length(h));
        if(!pipe.read_exact(p.data(),static_cast<DWORD>(p.size()))) {
          if(stopped)throw StopRequested();
          throw std::runtime_error("truncated client command");
        }
        session.command(h,p);
        std::lock_guard lock(route);deliver=true;
      }
      {std::lock_guard lock(route);deliver=false;pipe.drop_client();}
      if(active)break;
    }
  } catch(...) {reader_error=std::current_exception();}
  stopped=true;worker.join();
  if(worker_error)std::rethrow_exception(worker_error);
  if(reader_error)std::rethrow_exception(reader_error);
}
} // namespace mt7925
