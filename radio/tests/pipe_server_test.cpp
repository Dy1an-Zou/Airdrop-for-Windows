// SPDX-License-Identifier: GPL-2.0-only
/* PipeServer without hardware: a client that only probes the pipe must not end
 * the session, and a real client must then get through in both directions. */
#include <cstdio>
#include <cstring>
#include <thread>

#include "pipe_server.h"

namespace {

constexpr char kPipeName[] = "\\\\.\\pipe\\winawdl-radio-test";
constexpr DWORD kRetryMs = 20;
constexpr int kRetries = 250;
const uint8_t kPayload[] = {0xDE, 0xAD, 0xBE, 0xEF};

int failures = 0;

void expect(bool condition, const char *what) {
  std::printf("%s  %s\n", condition ? "ok  " : "FAIL", what);
  if (!condition) failures++;
}

HANDLE connect_client() {
  for (int attempt = 0; attempt < kRetries; attempt++) {
    HANDLE pipe = CreateFileA(kPipeName, GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) return pipe;
    Sleep(kRetryMs);
  }
  return INVALID_HANDLE_VALUE;
}

/* Sends one TX message, then reads back the hello the server answers with. */
void real_client(bool &got_hello) {
  HANDLE pipe = connect_client();
  if (pipe == INVALID_HANDLE_VALUE) return;
  const radio_link_header header{RADIO_LINK_TX, 0, sizeof(kPayload)};
  DWORD done = 0;
  WriteFile(pipe, &header, sizeof(header), &done, nullptr);
  WriteFile(pipe, kPayload, sizeof(kPayload), &done, nullptr);

  radio_link_header reply{};
  radio_link_hello hello{};
  got_hello = ReadFile(pipe, &reply, sizeof(reply), &done, nullptr) &&
              reply.type == RADIO_LINK_HELLO &&
              ReadFile(pipe, &hello, sizeof(hello), &done, nullptr) &&
              hello.version == RADIO_LINK_VERSION;
  CloseHandle(pipe);
}

} /* namespace */

int main() {
  PipeServer server(kPipeName, [] { return false; });
  expect(server.valid(), "pipe created");

  /* What a shell's "does this pipe exist" check does: connect and hang up. */
  CloseHandle(connect_client());
  expect(server.wait_for_client(), "a probe counts as a connection");
  radio_link_header header{};
  expect(!server.read_exact(&header, sizeof(header)),
         "the probe's departure fails the first read");
  server.drop_client();

  bool got_hello = false;
  std::thread client(real_client, std::ref(got_hello));
  expect(server.wait_for_client(), "a real client connects after the probe");
  expect(server.read_exact(&header, sizeof(header)) &&
             header.type == RADIO_LINK_TX && header.length == sizeof(kPayload),
         "its message header arrives");
  uint8_t payload[sizeof(kPayload)] = {};
  expect(server.read_exact(payload, sizeof(payload)) &&
             std::memcmp(payload, kPayload, sizeof(payload)) == 0,
         "its payload arrives intact");
  const radio_link_hello hello{RADIO_LINK_VERSION, 6, {}};
  expect(
      server.send(RADIO_LINK_HELLO,
                  {reinterpret_cast<const uint8_t *>(&hello), sizeof(hello)}),
      "the server can answer");
  client.join();
  expect(got_hello, "the client reads the answer");

  return failures ? 1 : 0;
}
