#pragma once

// The Screw Attack's sounds from Echoes (port_echoes_suits_import.cpp bakes them,
// port_screw_audio.cpp loads and plays them).
//
// Echoes keeps them in three MusyX audio groups (AGSC). Both games use MusyX and the same
// project, pool and sample directory layouts; Echoes' AGSC only puts the four part sizes up
// front and orders the parts pool, project, sample directory, samples, where Prime puts each
// part behind its size and orders them pool, project, samples, sample directory. Their macro
// and sample ids hardly meet Prime's, but their sound (define) ids do: both games number
// sounds from 0 up. So the bake rewrites each group into Prime's layout, keeps only the Screw
// Attack's sounds in its sound table, and moves their define ids past Prime's highest.
//
// The baked file (user folder, kFileName), little-endian: u32 kMagic, u32 kVersion, u32 group
// count, then per group u16 MusyX group id, u16 0, u32 size and that many bytes of a Prime
// layout AGSC; then u32 sound count (kSoundCount) and one u16 define id per ESound (0xFFFF for
// a sound the bake did not find).

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

class CVector3f;

namespace PortScrewAudio {

constexpr const char* kFileName = "echoes_screw_audio.bin";
constexpr uint32_t kMagic = 0x55415345;  // "ESAU"
constexpr uint32_t kVersion = 1;
constexpr uint16_t kNoSound = 0xFFFF;

// In Echoes' sound id table (ATBL) order of mention; see EchoesSoundId.
enum ESound {
  kScrewIn,       // wind-up clip event
  kScrewOut,      // spin-out clip event
  kScrewJump,     // wall-bump clip event, with kScrewOutWall
  kScrewOutWall,  // wall-bump clip event
  kLoop,          // looped, restarted on each jump
  kWallJumpHit,   // Echoes' screw wall jump (not ported)
  kWallHit,       // the attack ends against a wall
  kLoopEnter,     // looped, from entering the ball until the first jump
  kSoundCount
};

// Echoes' game sound ids (its SFXsam_* names), which its ATBL turns into define ids.
inline uint16_t EchoesSoundId(ESound sound) {
  static const uint16_t ids[kSoundCount] = {0x019E, 0x01A3, 0x01DC, 0x01DD, 0x2203, 0x2204, 0x032D, 0x04A1};
  return ids[sound];
}

inline const char* SoundName(ESound sound) {
  static const char* const names[kSoundCount] = {
      "SFXsam_b_screwin_00_oneshot",   "SFXsam_b_screwout_00_oneshot", "SFXsam_b_screwjump_00_oneshot",
      "SFXsam_b_screwout_01_oneshot",  "SFXsam_a_screw_lp_00_looped",  "SFXsam_a_screwhit_00_oneshot",
      "SFXsam_a_screwhit_01_oneshot",  "SFXsam_a_screw_lp_01_looped",
  };
  return names[sound];
}

namespace Detail {
inline uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
inline uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline void PutBe32(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
}
inline void PutBe16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
template <typename T>
void PutLe(std::vector<uint8_t>& out, T v) {
  for (size_t i = 0; i < sizeof(T); ++i) out.push_back(uint8_t(uint64_t(v) >> (8 * i)));
}
template <typename T>
bool GetLe(const uint8_t* data, size_t size, size_t& at, T& v) {
  if (at + sizeof(T) > size) return false;
  uint64_t r = 0;
  for (size_t i = 0; i < sizeof(T); ++i) r |= uint64_t(data[at + i]) << (8 * i);
  v = T(r);
  at += sizeof(T);
  return true;
}
}  // namespace Detail

// One Echoes AGSC as a Prime-layout AGSC whose sound tables hold only the sounds in `keep`
// (old define id -> new define id), renumbered. Sets `groupId` to the project's first group's
// id. False, with `why`, when the bytes are not an Echoes AGSC.
inline bool ConvertGroup(const uint8_t* data, size_t size, const std::vector<std::pair<uint16_t, uint16_t>>& keep,
                         std::vector<uint8_t>& out, uint16_t& groupId, std::string* why = nullptr) {
  using namespace Detail;
  const auto fail = [why](const char* text) {
    if (why) *why = text;
    return false;
  };
  if (size < 8 || Be32(data) != 1) return fail("not an Echoes audio group (version)");
  size_t at = 4;
  std::string name;
  while (at < size && data[at] != 0) name += char(data[at++]);
  ++at;
  if (at + 18 > size) return fail("truncated header");
  at += 2;  // the group id again; the project's is the one MusyX pushes by
  const uint32_t poolSize = Be32(data + at), projectSize = Be32(data + at + 4);
  const uint32_t sdirSize = Be32(data + at + 8), samplesSize = Be32(data + at + 12);
  at += 16;
  const uint64_t total = uint64_t(at) + poolSize + projectSize + sdirSize + samplesSize;
  if (total > size) return fail("parts run past the end");
  const uint8_t* pool = data + at;
  std::vector<uint8_t> project(pool + poolSize, pool + poolSize + projectSize);
  const uint8_t* sdir = pool + poolSize + projectSize;
  const uint8_t* samples = sdir + sdirSize;

  // Each group of the project (a chain ended by 0xFFFFFFFF); a sound group's table is u16
  // count, u16 pad, then 10-byte entries starting with the u16 define id, sorted by it (MusyX
  // searches it by halves).
  bool first = true;
  for (size_t g = 0, guard = 0; g + 0x28 <= project.size() && guard < 64; ++guard) {
    uint8_t* group = project.data() + g;
    const uint32_t next = Be32(group);
    if (next == 0xFFFFFFFF) break;
    if (first) groupId = Be16(group + 4);
    first = false;
    if (Be16(group + 6) == 1) {
      const uint32_t table = Be32(group + 0x1C);
      if (table + 4 > project.size()) return fail("sound table outside the project");
      const uint32_t count = Be16(project.data() + table);
      if (table + 4 + uint64_t(count) * 10 > project.size()) return fail("sound table runs past the project");
      std::vector<std::vector<uint8_t>> kept;
      for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = project.data() + table + 4 + i * 10;
        for (const auto& [from, to] : keep) {
          if (Be16(e) == from) {
            std::vector<uint8_t> entry(e, e + 10);
            PutBe16(entry.data(), to);
            kept.push_back(std::move(entry));
          }
        }
      }
      std::sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) { return Be16(a.data()) < Be16(b.data()); });
      PutBe16(project.data() + table, uint16_t(kept.size()));
      for (size_t i = 0; i < kept.size(); ++i) std::memcpy(project.data() + table + 4 + i * 10, kept[i].data(), 10);
    }
    if (next <= g) break;
    g = next;
  }
  if (first) return fail("the project has no group");

  out.clear();
  const char dir[] = "Audio/";
  out.insert(out.end(), dir, dir + sizeof(dir));
  out.insert(out.end(), name.begin(), name.end());
  out.push_back(0);
  PutBe32(out, poolSize);
  out.insert(out.end(), pool, pool + poolSize);
  PutBe32(out, projectSize);
  out.insert(out.end(), project.begin(), project.end());
  PutBe32(out, samplesSize);
  out.insert(out.end(), samples, samples + samplesSize);
  PutBe32(out, sdirSize);
  out.insert(out.end(), sdir, sdir + sdirSize);
  return true;
}

struct Group {
  uint16_t id = 0;
  std::vector<uint8_t> agsc;  // Prime layout
};

struct File {
  std::vector<Group> groups;
  uint16_t defines[kSoundCount] = {kNoSound, kNoSound, kNoSound, kNoSound, kNoSound, kNoSound, kNoSound, kNoSound};
};

inline std::vector<uint8_t> Write(const File& file) {
  using namespace Detail;
  std::vector<uint8_t> out;
  PutLe(out, kMagic);
  PutLe(out, kVersion);
  PutLe(out, uint32_t(file.groups.size()));
  for (const Group& g : file.groups) {
    PutLe(out, g.id);
    PutLe(out, uint16_t(0));
    PutLe(out, uint32_t(g.agsc.size()));
    out.insert(out.end(), g.agsc.begin(), g.agsc.end());
  }
  PutLe(out, uint32_t(kSoundCount));
  for (const uint16_t d : file.defines) PutLe(out, d);
  return out;
}

inline bool Read(const uint8_t* data, size_t size, File& file) {
  using namespace Detail;
  file = File();
  size_t at = 0;
  uint32_t magic = 0, version = 0, groups = 0;
  if (!GetLe(data, size, at, magic) || !GetLe(data, size, at, version) || !GetLe(data, size, at, groups) ||
      magic != kMagic || version != kVersion || groups > 16) {
    return false;
  }
  for (uint32_t i = 0; i < groups; ++i) {
    Group g;
    uint16_t pad = 0;
    uint32_t length = 0;
    if (!GetLe(data, size, at, g.id) || !GetLe(data, size, at, pad) || !GetLe(data, size, at, length) ||
        at + length > size) {
      return false;
    }
    g.agsc.assign(data + at, data + at + length);
    at += length;
    file.groups.push_back(std::move(g));
  }
  uint32_t count = 0;
  if (!GetLe(data, size, at, count) || count != kSoundCount) return false;
  for (uint16_t& d : file.defines) {
    if (!GetLe(data, size, at, d)) return false;
  }
  return true;
}

// The game side (port_screw_audio.cpp). Load pushes the baked groups into MusyX beside Prime's
// preloaded ones (CGameArchitectureSupport::LoadAudio) and Unload pops them first
// (UnloadAudio), so the group stack stays in order. Without a bake every call does nothing.
void Load();
void Unload();
const std::string& Status();
uint16_t Define(ESound sound);
// One-shot sounds at a point, and the one looped sound (a new one stops the last).
void Play(ESound sound, const CVector3f& at);
void StartLoop(ESound sound, const CVector3f& at);
void UpdateLoop(const CVector3f& at);
void StopLoop();

}  // namespace PortScrewAudio
