// SPDX-License-Identifier: MIT
#include "CnaService/QosProtocol.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
using namespace CnaService;
namespace {
void check(bool value, const char *why) {
  if (!value)
    throw std::runtime_error(why);
}
std::vector<unsigned char> hex(const std::string &text) {
  std::vector<unsigned char> result;
  for (std::size_t i = 0; i < text.size(); i += 2)
    result.push_back(
        static_cast<unsigned char>(std::stoul(text.substr(i, 2), nullptr, 16)));
  return result;
}
void rejected(const std::vector<unsigned char> &bytes,
              const std::string &code) {
  try {
    (void)parseQosFrame(bytes);
  } catch (const QosError &error) {
    check(error.code() == code, "exact rejection code");
    return;
  }
  throw std::runtime_error("malformed frame accepted");
}
} // namespace
int main() {
  try {
    // Literal CNQ v1 field oracle: probe bytes1..16, seq0, count4, nonce42, big
    // endian.
    const auto ping =
        hex("434e5101010004000102030405060708090a0b0c0d0e0f10000000000000002a");
    const auto parsed = parseQosFrame(ping);
    check(parsed.type == QosType::Ping && parsed.sequence == 0 &&
              parsed.count == 4 && parsed.nonce == 42,
          "ping fields");
    for (unsigned i = 0; i < 16; ++i)
      check(parsed.probe[i] == i + 1, "all probe bytes");
    check(encodeQosFrame(parsed) == ping,
          "encoder exact literal, not just roundtrip");
    auto pong = ping;
    pong[4] = 2;
    check(parseQosFrame(pong).type == QosType::Pong, "pong tag");
    auto up =
        hex("434e5101030008000102030405060708090a0b0c0d0e0f100000000000000000");
    up.resize(1200, 0);
    check(parseQosFrame(up).type == QosType::Up, "1200-byte padded train");
    auto report = hex("434e5101050008000102030405060708090a0b0c0d0e0f1000000000"
                      "0000000000000008000000000001117000000000");
    const auto view = parseQosFrame(report);
    check(view.acceptedCount == 8 && view.elapsedUs == 70000,
          "report counts and receiver span");
    check(encodeQosFrame(view) == report, "report encoder exact literal");
    for (unsigned size = 0; size < 32; ++size)
      rejected(std::vector<unsigned char>(size, 0), "QOS_TRUNCATED");
    rejected(std::vector<unsigned char>(1201, 0), "QOS_TOO_LARGE");
    for (auto [offset, value, code] :
         std::vector<std::tuple<unsigned, unsigned, std::string>>{
             {0, 0, "QOS_MAGIC"},
             {3, 2, "QOS_VERSION"},
             {4, 0, "QOS_TYPE"},
             {4, 8, "QOS_TYPE"},
             {7, 1, "QOS_RESERVED"},
             {5, 4, "QOS_FIELDS"},
             {6, 3, "QOS_FIELDS"},
             {31, 0, "QOS_FIELDS"}}) {
      auto bad = ping;
      bad[offset] = static_cast<unsigned char>(value);
      rejected(bad, code);
    }
    auto zero = ping;
    std::fill(zero.begin() + 8, zero.begin() + 24, 0);
    rejected(zero, "QOS_PROBE");
    auto extra = ping;
    extra.push_back(0);
    rejected(extra, "QOS_LENGTH");
    auto shortUp = up;
    shortUp.pop_back();
    rejected(shortUp, "QOS_LENGTH");
    auto pad = up;
    pad[1199] = 1;
    rejected(pad, "QOS_FIELDS");
    auto index = up;
    index[5] = 8;
    rejected(index, "QOS_FIELDS");
    auto count = report;
    count[35] = 9;
    rejected(count, "QOS_FIELDS");
    auto zeroSpan = report;
    std::fill(zeroSpan.begin() + 36, zeroSpan.begin() + 44, 0);
    rejected(zeroSpan, "QOS_FIELDS");
    auto tooLong = report;
    tooLong[40] = 0xff;
    rejected(tooLong, "QOS_FIELDS");
    auto reserved = report;
    reserved[47] = 1;
    rejected(reserved, "QOS_RESERVED");
    // Literal parser corpus also tests allocation-free views (payload points
    // inside original).
    const auto train = parseQosFrame(up);
    check(train.payload.data() == up.data() + 32 &&
              train.payload.size() == 1168,
          "borrowed payload view");
    std::cout << "CNQ v1 literal/boundary assertions passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
