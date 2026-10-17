// SPDX-License-Identifier: GPL-2.0-only
/* FrameCapture without hardware: the file it writes is read back as pcap. */
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "frame_capture.h"

namespace {

constexpr size_t kPcapHeaderBytes = 24;
constexpr size_t kRecordHeaderBytes = 16;

int failures = 0;

void expect(bool condition, const char *what) {
  std::printf("%s  %s\n", condition ? "ok  " : "FAIL", what);
  if (!condition) failures++;
}

uint32_t read32(const std::vector<uint8_t> &bytes, size_t offset) {
  uint32_t value = 0;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

std::vector<uint8_t> frame_of(uint8_t frame_control, size_t size) {
  std::vector<uint8_t> frame(size, 0xAB);
  frame[0] = frame_control;
  return frame;
}

} // namespace

int main() {
  const auto path =
      (std::filesystem::temp_directory_path() / "winawdl-capture-test.pcap")
          .string();
  const auto data = frame_of(0x88, 1500);
  const auto action = frame_of(0xD0, 300);
  /* A client message: an 8-byte radiotap header, then a data frame. */
  auto sent = frame_of(0x00, 8 + 400);
  sent[2] = 8;
  sent[3] = 0;
  sent[8] = 0x08;
  {
    FrameCapture capture(path);
    expect(capture.is_open(), "the capture file opens");
    capture.received(data, {.rate = 0x10C,
                            .bandwidth = 2,
                            .ppdu_type = 3,
                            .dbm = -40,
                            .fcs_at_end = true,
                            .bad_fcs = false});
    capture.received(action, {.rate = 4,
                              .bandwidth = 0,
                              .ppdu_type = 1,
                              .dbm = -55,
                              .fcs_at_end = true,
                              .bad_fcs = true});
    capture.transmitted(sent);
  }

  std::ifstream file(path, std::ios::binary);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),
                                   std::istreambuf_iterator<char>()};
  file.close();
  std::filesystem::remove(path);

  const size_t radiotap = FrameCapture::kRxRadiotapBytes;
  const size_t kept = FrameCapture::kDataFrameBytes;
  const size_t first = kPcapHeaderBytes;
  const size_t second = first + kRecordHeaderBytes + radiotap + kept;
  const size_t third = second + kRecordHeaderBytes + radiotap + action.size();
  expect(bytes.size() == third + kRecordHeaderBytes + 8 + kept,
         "three records fill the file exactly");
  expect(read32(bytes, 0) == 0xa1b2c3d4 && read32(bytes, 20) == 127,
         "the header says pcap with radiotap frames");

  expect(read32(bytes, first + 8) == radiotap + kept &&
             read32(bytes, first + 12) == radiotap + data.size(),
         "a data frame is cut short and keeps its real length");
  const size_t fields = first + kRecordHeaderBytes;
  expect(bytes[fields + 2] == radiotap && bytes[fields + 8] == 0x10 &&
             static_cast<int8_t>(bytes[fields + 9]) == -40,
         "the radiotap header carries the flags and the signal");
  expect(bytes[fields + 16] == 0x0C && bytes[fields + 17] == 0x01 &&
             bytes[fields + 18] == 2 && bytes[fields + 19] == 3,
         "the vendor field carries rate, bandwidth and PPDU type");
  expect(bytes[fields + radiotap] == 0x88, "the frame follows the header");

  expect(read32(bytes, second + 8) == radiotap + action.size(),
         "a management frame is kept whole");
  expect(bytes[second + kRecordHeaderBytes + 8] == 0x50,
         "a failed checksum is flagged");

  expect(read32(bytes, third + 8) == 8 + kept &&
             read32(bytes, third + 12) == sent.size(),
         "a sent data frame is cut after the client's own header");

  return failures ? 1 : 0;
}
