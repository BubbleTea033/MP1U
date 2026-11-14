#pragma once

// The Screw Attack's particle effects from Echoes (port_echoes_suits_import.cpp bakes them,
// port_screw_fx.cpp serves and draws them).
//
// Echoes starts three effects from the spin clip's events (on Skeleton_Root, at its first frame)
// and a jump flash (by name, ScrewAttackJumpFlash) on each jump. The port's particle loader reads
// Echoes' PART keys (it gained KEYF and INDM for these), and Echoes' textures are Prime's format,
// so the bake copies the effects and everything they reach, renumbered into a range of ids
// PortCustomRes answers from here (no PAK has them, and Echoes' ids could meet Prime's).
//
// The baked file (user folder, kFileName), little-endian: u32 kMagic, u32 kVersion, u32 effect
// count (kEffectCount) and one u32 id per EEffect (0 for one the bake did not find), u32
// resource count, then per resource u32 id, u32 type (FourCC), u32 size and its bytes.

#include <cstdint>
#include <string>
#include <vector>

class CTransform4f;
class CVector3f;

namespace PortScrewFx {

constexpr const char* kFileName = "echoes_screw_effects.bin";
constexpr uint32_t kMagic = 0x58465345;  // "ESFX"
constexpr uint32_t kVersion = 1;

// PortCustomRes ids handed out from here (port_custom_res.h, kProvidedBase).
constexpr uint32_t kFirstId = 0xDEAF4000;
constexpr uint32_t kMaxResources = 0x1000;

// Echoes' ids of the spin clip's effects.
constexpr uint32_t kSpinA = 0xAB3E7D01;
constexpr uint32_t kSpinB = 0x82056882;
constexpr uint32_t kSpinC = 0xDD0ACDCB;

enum EEffect { kSpin1, kSpin2, kSpin3, kJumpFlash, kEffectCount };

struct Resource {
  uint32_t id = 0;
  uint32_t type = 0;
  std::vector<uint8_t> data;
};

struct File {
  uint32_t effects[kEffectCount] = {};
  std::vector<Resource> resources;
};

namespace Detail {
template <typename T>
void PutLe(std::vector<uint8_t>& out, T v) {
  for (size_t i = 0; i < sizeof(T); ++i) out.push_back(uint8_t(uint64_t(v) >> (8 * i)));
}
inline bool GetLe32(const uint8_t* data, size_t size, size_t& at, uint32_t& v) {
  if (at + 4 > size) return false;
  v = uint32_t(data[at]) | (uint32_t(data[at + 1]) << 8) | (uint32_t(data[at + 2]) << 16) |
      (uint32_t(data[at + 3]) << 24);
  at += 4;
  return true;
}
}  // namespace Detail

inline std::vector<uint8_t> Write(const File& file) {
  using Detail::PutLe;
  std::vector<uint8_t> out;
  PutLe(out, kMagic);
  PutLe(out, kVersion);
  PutLe(out, uint32_t(kEffectCount));
  for (const uint32_t e : file.effects) PutLe(out, e);
  PutLe(out, uint32_t(file.resources.size()));
  for (const Resource& r : file.resources) {
    PutLe(out, r.id);
    PutLe(out, r.type);
    PutLe(out, uint32_t(r.data.size()));
    out.insert(out.end(), r.data.begin(), r.data.end());
  }
  return out;
}

inline bool Read(const uint8_t* data, size_t size, File& file) {
  using Detail::GetLe32;
  file = File();
  size_t at = 0;
  uint32_t magic = 0, version = 0, effects = 0, count = 0;
  if (!GetLe32(data, size, at, magic) || !GetLe32(data, size, at, version) || !GetLe32(data, size, at, effects) ||
      magic != kMagic || version != kVersion || effects != kEffectCount) {
    return false;
  }
  for (uint32_t& e : file.effects) {
    if (!GetLe32(data, size, at, e)) return false;
  }
  if (!GetLe32(data, size, at, count) || count > kMaxResources) return false;
  for (uint32_t i = 0; i < count; ++i) {
    Resource r;
    uint32_t length = 0;
    if (!GetLe32(data, size, at, r.id) || !GetLe32(data, size, at, r.type) || !GetLe32(data, size, at, length) ||
        at + length > size || r.id < kFirstId || r.id >= kFirstId + kMaxResources) {
      return false;
    }
    r.data.assign(data + at, data + at + length);
    at += length;
    file.resources.push_back(std::move(r));
  }
  return true;
}

// The game side (port_screw_fx.cpp). Load reads the bake and offers its resources to
// PortCustomRes; without a bake every call does nothing.
void Load();
const std::string& Status();
// The spin's three effects, from the attack's start until it ends (they then stop emitting and
// fade out), and a jump flash on each jump. As Echoes' particle database places an event's effect
// (kPM_ContinuousSystem), the spin follows `bone` (Skeleton_Root posed, in the world: position
// and rotation) and the flashes follow `body`; both are scaled by Samus' model scale.
// Starts loading the effects (the wind-up calls it, so they are in when the spin begins).
void Prepare();
void StartSpin();
void StopSpin();
void JumpFlash();
void Update(float dt, const CTransform4f& bone, const CTransform4f& body, const CVector3f& scale);
void Render();
void Clear();

}  // namespace PortScrewFx
