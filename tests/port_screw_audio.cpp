// The Screw Attack's sounds from Echoes (platform/include/port_screw_audio.h): an Echoes audio
// group rewritten into Prime's layout with its sound table cut down and renumbered, and the
// baked file's round trip. The group is built here; no game data.

#include "port_screw_audio.h"
#include "port_test.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace PortScrewAudio;

void Be32(std::vector<uint8_t>& v, uint32_t x) {
  for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s));
}
void Be16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(uint8_t(x >> 8));
  v.push_back(uint8_t(x));
}
uint32_t Get32(const std::vector<uint8_t>& v, size_t at) {
  return (uint32_t(v[at]) << 24) | (uint32_t(v[at + 1]) << 16) | (uint32_t(v[at + 2]) << 8) | v[at + 3];
}
uint16_t Get16(const std::vector<uint8_t>& v, size_t at) { return uint16_t((v[at] << 8) | v[at + 1]); }

// A one-group sound project (group 302) whose table holds defines 10, 20, 30 (macros 100, 200,
// 300); the table sits at 0x28, after the group header.
std::vector<uint8_t> Project() {
  std::vector<uint8_t> p;
  Be32(p, 0x28 + 4 + 3 * 10 + 2);  // next group: the terminator after the table
  Be16(p, 302);
  Be16(p, 1);  // sound group
  for (int i = 0; i < 5; ++i) Be32(p, 0x28 + 4 + 30);  // id lists: all the empty list after
  Be32(p, 0x28);  // sound table
  while (p.size() < 0x28) p.push_back(0);
  Be16(p, 3);
  Be16(p, 0);
  for (uint16_t i = 1; i <= 3; ++i) {
    Be16(p, uint16_t(i * 10));
    Be16(p, uint16_t(i * 100));
    for (int b = 0; b < 6; ++b) p.push_back(uint8_t(i));
  }
  Be16(p, 0xFFFF);  // the empty id list
  Be32(p, 0xFFFFFFFF);
  return p;
}

std::vector<uint8_t> EchoesGroup(const std::vector<uint8_t>& project) {
  const std::vector<uint8_t> pool = {1, 2, 3, 4, 5};
  const std::vector<uint8_t> sdir = {6, 7, 8};
  const std::vector<uint8_t> samples = {9, 10, 11, 12};
  std::vector<uint8_t> g;
  Be32(g, 1);
  for (const char c : std::string("Weapons3")) g.push_back(uint8_t(c));
  g.push_back(0);
  Be16(g, 302);
  Be32(g, uint32_t(pool.size()));
  Be32(g, uint32_t(project.size()));
  Be32(g, uint32_t(sdir.size()));
  Be32(g, uint32_t(samples.size()));
  g.insert(g.end(), pool.begin(), pool.end());
  g.insert(g.end(), project.begin(), project.end());
  g.insert(g.end(), sdir.begin(), sdir.end());
  g.insert(g.end(), samples.begin(), samples.end());
  return g;
}

void TestConvert() {
  const std::vector<uint8_t> project = Project();
  const std::vector<uint8_t> echoes = EchoesGroup(project);
  std::vector<uint8_t> out;
  uint16_t id = 0;
  std::string why;
  // Keep 30 and 10, renumbered so they swap order: the table must come out sorted.
  REQUIRE_MSG(ConvertGroup(echoes.data(), echoes.size(), {{30, 5001}, {10, 5002}}, out, id, &why), why);
  CHECK_EQ(id, 302);

  // Prime's layout: directory, name, then pool, project, samples, sample directory, each sized.
  size_t at = 0;
  CHECK_EQ(std::string(reinterpret_cast<const char*>(out.data())), "Audio/");
  at += 7;
  CHECK_EQ(std::string(reinterpret_cast<const char*>(out.data() + at)), "Weapons3");
  at += 9;
  CHECK_EQ(Get32(out, at), 5u);
  CHECK_EQ(out[at + 4], 1);
  at += 4 + 5;
  REQUIRE(Get32(out, at) == project.size());
  const size_t p = at + 4;
  at = p + project.size();
  CHECK_EQ(Get32(out, at), 4u);
  CHECK_EQ(out[at + 4], 9);
  at += 4 + 4;
  CHECK_EQ(Get32(out, at), 3u);
  CHECK_EQ(out[at + 4], 6);
  CHECK_EQ(at + 4 + 3, out.size());

  // The table: two entries, sorted by the new ids, each keeping its macro and fields.
  CHECK_EQ(Get16(out, p + 0x28), 2);
  CHECK_EQ(Get16(out, p + 0x28 + 4), 5001);
  CHECK_EQ(Get16(out, p + 0x28 + 6), 300);
  CHECK_EQ(out[p + 0x28 + 8], 3);
  CHECK_EQ(Get16(out, p + 0x28 + 14), 5002);
  CHECK_EQ(Get16(out, p + 0x28 + 16), 100);
  // The rest of the project is as it was.
  CHECK_EQ(Get32(out, p + 0x1C), 0x28u);
  CHECK_EQ(Get16(out, p + 4), 302);

  CHECK_MSG(!ConvertGroup(echoes.data(), echoes.size() - 1, {}, out, id, &why), "a cut group is refused");
  std::vector<uint8_t> prime = echoes;
  prime[3] = 2;
  CHECK_MSG(!ConvertGroup(prime.data(), prime.size(), {}, out, id, &why), "not version 1 is refused");
}

void TestFile() {
  File file;
  file.groups.push_back({302, {1, 2, 3}});
  file.groups.push_back({321, {4}});
  file.defines[kScrewIn] = 4000;
  file.defines[kLoop] = 4004;
  const std::vector<uint8_t> bytes = Write(file);
  File back;
  REQUIRE(Read(bytes.data(), bytes.size(), back));
  REQUIRE(back.groups.size() == 2);
  CHECK_EQ(back.groups[0].id, 302);
  CHECK(back.groups[0].agsc == file.groups[0].agsc);
  CHECK_EQ(back.groups[1].id, 321);
  CHECK_EQ(back.defines[kScrewIn], 4000);
  CHECK_EQ(back.defines[kLoop], 4004);
  CHECK_EQ(back.defines[kWallHit], kNoSound);
  CHECK_MSG(!Read(bytes.data(), bytes.size() - 1, back), "a cut file is refused");
}

}  // namespace

int main() {
  TestConvert();
  TestFile();
  return PORT_TEST_RESULT();
}
