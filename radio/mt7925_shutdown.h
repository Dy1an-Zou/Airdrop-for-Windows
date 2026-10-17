// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <exception>
#include <ostream>
#include <stdexcept>
#include "mt7925_txpower.h"
#include "mt7925_txs.h"

namespace mt7925 {
inline void report_exception(std::ostream& errors,const char* prefix,std::exception_ptr failure) {
  try {std::rethrow_exception(failure);}
  catch(const std::exception& e){errors<<prefix<<e.what()<<std::endl;}
  catch(...){errors<<prefix<<"non-standard exception"<<std::endl;}
}
// The executable and offline model share the same explicit STOP consumer.
template<class Session,class Driver>
void finish_session(Session& session,Driver& driver,std::exception_ptr failure,
                    bool capture,std::ostream& output,std::ostream& errors) {
  if(failure)report_exception(errors,"Radio operation failure: ",failure);
  std::exception_ptr stop_failure;
  try {
    auto end=session.stop();
    errors<<"Radio STOP reply: result="<<end.result<<" error="<<end.error
          <<" stop_error="<<end.stop_error<<" stopped="<<int(end.stopped)
          <<" quiescent="<<int(end.quiescent)<<" ready="<<int(end.ready)
          <<" stage="<<end.stage<<" attempted="<<int(end.attempted)
          <<" published="<<int(end.published)<<" inflight="<<end.inflight<<std::endl;
    if(end.result || end.stop_error || !end.stopped || !end.quiescent || end.ready)
      throw std::runtime_error("radio STOP did not confirm quiescence; retain logs and restore driver");
    output<<"Radio STOP confirmed: stopped=1 quiescent=1 ready=0."<<std::endl;
  } catch(...) {
    stop_failure=std::current_exception();
    report_exception(errors,"Radio STOP failure: ",stop_failure);
  }
  // Diagnostics are read-only; neither their availability nor an earlier
  // START failure weakens the explicit STOP confirmation above.
  if(failure || capture || stop_failure) {
    try {
      const auto diagnostic=driver.diagnostics();
      errors<<"MCU diagnostics LE hex: "<<diagnostic<<std::endl;
      write_txpower_diagnostic(errors,diagnostic);
      write_txs_diagnostic(errors,diagnostic);
    } catch(...) {report_exception(errors,"MCU diagnostics unavailable: ",std::current_exception());}
  }
  if(failure)std::rethrow_exception(failure);
  if(stop_failure)std::rethrow_exception(stop_failure);
}
} // namespace mt7925
