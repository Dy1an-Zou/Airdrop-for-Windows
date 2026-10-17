// SPDX-License-Identifier: GPL-2.0-only
/* Writes frames to a pcap file with radiotap headers, to study a session
 * afterwards. Data frames are cut short: their headers say how the link
 * behaved, and what follows is someone's file. */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <string>

class FrameCapture {
public:
  /* What the chip reported about a received frame. */
  struct RxInfo {
    uint16_t rate;     /* the chip's rate code */
    uint8_t bandwidth; /* 0 = 20 MHz, 1 = 40, 2 = 80 */
    uint8_t ppdu_type;
    int8_t dbm;
    bool fcs_at_end;
    bool bad_fcs;
  };

  /* Bytes of the radiotap header put on received frames. */
  static constexpr uint16_t kRxRadiotapBytes = 20;
  /* How much of a data frame is kept: up to the end of the longest TCP
   * header, whose options date a segment. */
  static constexpr size_t kDataFrameBytes = 144;

  explicit FrameCapture(const std::string &path, size_t byte_budget = 32 * 1024 * 1024)
      : _file(nullptr), _budget(byte_budget) {
#ifdef _WIN32
    fopen_s(&_file, path.c_str(), "wbx");
#else
    _file = std::fopen(path.c_str(), "wbx");
#endif
    if (!_file) return;
    put32(kPcapMagic);
    put16(2);
    put16(4);
    put32(0);
    put32(0);
    put32(kSnapBytes);
    put32(kLinkTypeRadiotap);
  }
  ~FrameCapture() {
    if (_file) std::fclose(_file);
  }
  FrameCapture(const FrameCapture &) = delete;
  FrameCapture &operator=(const FrameCapture &) = delete;

  bool is_open() const { return _file != nullptr; }

  void received(std::span<const uint8_t> frame, const RxInfo &info) {
    if (!_file) return;
    const std::lock_guard<std::mutex> lock(_mutex);
    const size_t kept = kept_bytes(frame);
    if (!record_header(kRxRadiotapBytes + kept, kRxRadiotapBytes + frame.size())) return;
    put16(0);
    put16(kRxRadiotapBytes);
    put32(kPresentFlags | kPresentDbm | kPresentVendor);
    put8(static_cast<uint8_t>((info.fcs_at_end ? kFlagFcsAtEnd : 0) |
                              (info.bad_fcs ? kFlagBadFcs : 0)));
    put8(static_cast<uint8_t>(info.dbm));
    /* A vendor namespace: OUI, sub-namespace, then the length of its data. */
    put8(kVendorOui[0]);
    put8(kVendorOui[1]);
    put8(kVendorOui[2]);
    put8(0);
    put16(4);
    put16(info.rate);
    put8(info.bandwidth);
    put8(info.ppdu_type);
    std::fwrite(frame.data(), 1, kept, _file);
  }

  /* `message` is what the client asked to send: its own radiotap header, then
   * the frame. */
  void transmitted(std::span<const uint8_t> message) {
    radiotap_frame(message);
  }

  /* Native MT7925 RX already has a radiotap header. TX records only prove
   * local driver acceptance; they do not prove RF delivery or peer ACK. */
  void radiotap_frame(std::span<const uint8_t> message) {
    if (!_file || message.size() < 8) return;
    const size_t radiotap = message[2] | (message[3] << 8);
    if (radiotap < 8 || radiotap > message.size() || message.size() - radiotap < 24) return;
    const std::lock_guard<std::mutex> lock(_mutex);
    const size_t kept = radiotap + kept_bytes(message.subspan(radiotap));
    if (!record_header(kept, message.size())) return;
    std::fwrite(message.data(), 1, kept, _file);
  }

private:
  static constexpr uint32_t kPcapMagic = 0xa1b2c3d4;
  static constexpr uint32_t kSnapBytes = 65535;
  static constexpr uint32_t kLinkTypeRadiotap = 127;
  static constexpr uint32_t kPresentFlags = 1u << 1;
  static constexpr uint32_t kPresentDbm = 1u << 5;
  static constexpr uint32_t kPresentVendor = 1u << 30;
  static constexpr uint8_t kFlagFcsAtEnd = 0x10;
  static constexpr uint8_t kFlagBadFcs = 0x40;
  static constexpr uint8_t kVendorOui[3] = {0x02, 0x77, 0x61};
  static constexpr uint8_t kFrameTypeMask = 0x0C;
  static constexpr uint8_t kFrameTypeData = 0x08;

  static size_t kept_bytes(std::span<const uint8_t> frame) {
    const bool is_data =
        !frame.empty() && (frame[0] & kFrameTypeMask) == kFrameTypeData;
    return is_data ? std::min(frame.size(), kDataFrameBytes) : frame.size();
  }

  bool record_header(size_t kept, size_t original) {
    if (_written > _budget || kept + 16 > _budget - _written) return false;
    _written += kept + 16;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto micros =
        std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    put32(static_cast<uint32_t>(micros / 1000000));
    put32(static_cast<uint32_t>(micros % 1000000));
    put32(static_cast<uint32_t>(kept));
    put32(static_cast<uint32_t>(original));
    return true;
  }

  void put8(uint8_t value) { std::fputc(value, _file); }
  void put16(uint16_t value) {
    put8(static_cast<uint8_t>(value));
    put8(static_cast<uint8_t>(value >> 8));
  }
  void put32(uint32_t value) {
    put16(static_cast<uint16_t>(value));
    put16(static_cast<uint16_t>(value >> 16));
  }

  std::FILE *_file;
  size_t _budget;
  size_t _written = 24;
  std::mutex _mutex;
};
