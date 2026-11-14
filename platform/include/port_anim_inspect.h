// Reading animation sets (ANCS) and animations (ANIM) of Metroid Prime and Echoes by their shapes,
// for `--inspect-echoes-anims` (port_echoes_suits_import.cpp): the facts the Screw Attack's
// animation import needs (which animation it is, its format, the two skeletons), gathered without
// a full ANCS parser. The PAS and transition tables between the parts are not read; instead:
//
// - a character is a name followed by its model, skin and skeleton ids, all three ids the disc
//   has (Character: id, version, name, CMDL, CSKR, CINF, ...);
// - an animation is a meta-animation "Play" (type 0) whose ANIM id the disc has, followed by its
//   primitive index and name (MetaAnimation Play: type, ANIM, primitive, name, CharAnimTime);
//   when a name ends just before it, that is the animation set entry's name.
//
// Pure: bytes in, findings out; `known(type, id)` says whether the disc has that resource.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace PortAnimInspect {

constexpr uint32_t kCMDL = 0x434D444C;
constexpr uint32_t kCSKR = 0x43534B52;
constexpr uint32_t kCINF = 0x43494E46;
constexpr uint32_t kANIM = 0x414E494D;

using Known = std::function<bool(uint32_t type, uint32_t id)>;

inline uint32_t U32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline float F32(const uint8_t* p) {
  const uint32_t bits = U32(p);
  float f;
  static_assert(sizeof(f) == sizeof(bits));
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

inline bool Printable(uint8_t c) { return c >= 0x20 && c < 0x7F; }

// The null-terminated name of at least 2 printable characters starting at `at`; empty otherwise.
// `end` gets the offset after its terminator.
inline std::string NameAt(const uint8_t* data, size_t size, size_t at, size_t& end) {
  size_t i = at;
  while (i < size && Printable(data[i])) ++i;
  if (i >= size || data[i] != 0 || i - at < 2) return {};
  end = i + 1;
  return std::string(reinterpret_cast<const char*>(data + at), i - at);
}

// The name ending (with its terminator) just before `at`, if any.
inline std::string NameBefore(const uint8_t* data, size_t at) {
  if (at < 3 || data[at - 1] != 0) return {};
  size_t i = at - 1;
  while (i > 0 && Printable(data[i - 1])) --i;
  if (at - 1 - i < 2) return {};
  return std::string(reinterpret_cast<const char*>(data + i), at - 1 - i);
}

struct Character {
  size_t offset;
  std::string name;
  uint32_t cmdl, cskr, cinf;
};

struct Play {
  size_t offset;
  std::string entry;      // the animation set entry's name, or empty when nested
  std::string primitive;  // the primitive's name
  uint32_t anim;
  uint32_t primitiveIndex;
};

inline std::vector<Character> FindCharacters(const uint8_t* data, size_t size, const Known& known) {
  std::vector<Character> out;
  for (size_t at = 0; at + 2 < size; ++at) {
    if (!Printable(data[at]) || (at > 0 && Printable(data[at - 1]))) continue;
    size_t end = 0;
    const std::string name = NameAt(data, size, at, end);
    if (name.empty() || end + 12 > size) continue;
    const uint32_t cmdl = U32(data + end), cskr = U32(data + end + 4), cinf = U32(data + end + 8);
    if (known(kCMDL, cmdl) && known(kCSKR, cskr) && known(kCINF, cinf)) {
      out.push_back({at, name, cmdl, cskr, cinf});
    }
  }
  return out;
}

inline std::vector<Play> FindPlays(const uint8_t* data, size_t size, const Known& known) {
  std::vector<Play> out;
  for (size_t at = 0; at + 12 < size; ++at) {
    if (U32(data + at) != 0) continue;
    const uint32_t anim = U32(data + at + 4);
    if (!known(kANIM, anim)) continue;
    size_t end = 0;
    const std::string primitive = NameAt(data, size, at + 12, end);
    if (primitive.empty()) continue;
    out.push_back({at, NameBefore(data, at), primitive, anim, U32(data + at + 8)});
  }
  return out;
}

// An ANIM's header as text: the version, and for the compressed format (version 2 in Prime,
// with Echoes' extra scale multiplier) its timing, root, channel and key counts.
inline std::string DescribeAnim(const uint8_t* data, size_t size, bool echoes) {
  char line[256];
  if (size < 4) return "too short";
  const uint32_t version = U32(data);
  if (version == 0) {
    if (size < 4 + 24) return "version 0 (uncompressed), too short";
    std::snprintf(line, sizeof(line), "version 0 (uncompressed): duration %.3f s, interval %.4f s, %u keys, root %u",
                  F32(data + 4), F32(data + 12), U32(data + 20), U32(data + 24));
    return line;
  }
  // Compressed: scratch size, then Prime's event id and 1, or Echoes' u16 0x0101.
  size_t at = 8;
  uint32_t event = 0;
  if (echoes) {
    at += 2;
  } else {
    event = U32(data + at);
    at += 8;
  }
  const size_t need = at + (echoes ? 40 : 36);
  if (size < need) {
    std::snprintf(line, sizeof(line), "version %u, %zu bytes, too short for its header", version, size);
    return line;
  }
  const float duration = F32(data + at), interval = F32(data + at + 4);
  const uint32_t root = U32(data + at + 8), looping = U32(data + at + 12), divisor = U32(data + at + 16);
  const float translation = F32(data + at + 20);
  at += 24;
  float scale = 0.f;
  if (echoes) {
    scale = F32(data + at);
    at += 4;
  }
  const uint32_t channels = U32(data + at), keys = U32(data + at + 8);
  std::snprintf(line, sizeof(line),
                "version %u (compressed), %zu bytes: duration %.3f s, interval %.4f s, root %u, looping %u, "
                "rotation divisor %u, translation x%.5f%s, %u bone channels, %u key bitmap bits%s",
                version, size, duration, interval, root, looping, divisor, translation,
                echoes ? (", scale x" + std::to_string(scale)).c_str() : "", channels, keys,
                echoes ? "" : (", event " + std::to_string(event)).c_str());
  return line;
}

inline bool ContainsNoCase(const std::string& text, const char* needle) {
  for (size_t i = 0; i < text.size(); ++i) {
    size_t j = 0;
    for (; needle[j] != 0 && i + j < text.size(); ++j) {
      char a = text[i + j], b = needle[j];
      if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
      if (a != b) break;
    }
    if (needle[j] == 0) return true;
  }
  return false;
}

// Whether the bytes hold this big-endian u32 anywhere (an ANCS naming a skeleton).
inline bool Mentions(const uint8_t* data, size_t size, uint32_t id) {
  for (size_t at = 0; at + 4 <= size; ++at) {
    if (U32(data + at) == id) return true;
  }
  return false;
}

constexpr uint32_t kEVNT = 0x45564E54;

// One event of an Echoes EVNT. kind: 'B' bool, 'I' int32 (user events), 'P' particle, 'S' sound.
struct Event {
  char kind = 0;
  std::string name;
  uint32_t type = 0;      // EPOIType
  float time = 0.f;
  int32_t value = 0;      // bool: the flag; int32: the value; sound: the sound id
  uint32_t resource = 0;  // particle: its id
  uint32_t fourcc = 0;    // particle: its type
  std::string locator;    // int32: the locator's name; particle: the bone id as text
  float scale = 1.f;      // particle: the effect's scale
  uint32_t parent = 0;    // particle: how it follows the bone (CParticleData::EParentedMode)
};

// An Echoes EVNT's events (version 1 or 2: bool, int32, particle, then sound nodes), in file
// order. False when the bytes do not have that shape.
inline bool ParseEvents(const uint8_t* data, size_t size, std::vector<Event>& out, size_t* end = nullptr) {
  out.clear();
  size_t at = 0;
  const auto need = [&](size_t n) { return at <= size && size - at >= n; };
  if (!need(4)) return false;
  const uint32_t version = U32(data);
  at = 4;
  if (version != 1 && version != 2) return false;
  const char kinds[4] = {'B', 'I', 'P', 'S'};
  for (int k = 0; k < (version >= 2 ? 4 : 3); ++k) {
    if (!need(4)) return false;
    const uint32_t count = U32(data + at);
    at += 4;
    if (count > 4096) return false;
    for (uint32_t i = 0; i < count; ++i) {
      Event e;
      e.kind = kinds[k];
      if (!need(2)) return false;
      at += 2;
      while (at < size && data[at] != 0) e.name += char(data[at++]);
      ++at;
      // type u16, CharAnimTime (f32, u32), index u32, unique u8, weight f32, character s32, flags u32
      if (!need(27)) return false;
      e.type = (uint32_t(data[at]) << 8) | data[at + 1];
      e.time = F32(data + at + 2);
      at += 27;
      if (k == 0) {
        if (!need(1)) return false;
        e.value = data[at++];
      } else if (k == 1) {
        if (!need(4)) return false;
        e.value = int32_t(U32(data + at));
        at += 4;
        while (at < size && data[at] != 0) e.locator += char(data[at++]);
        ++at;
      } else if (k == 2) {
        // duration u32, tag (fourcc, id), bone id u32, scale f32, transform type u32
        if (!need(24)) return false;
        e.fourcc = U32(data + at + 4);
        e.resource = U32(data + at + 8);
        e.locator = std::to_string(U32(data + at + 12));
        e.scale = F32(data + at + 16);
        e.parent = U32(data + at + 20);
        at += 24;
      } else {
        // sound id u32, fall-off f32, max distance f32, then Echoes' u32, u16, u16, f32
        if (!need(24)) return false;
        e.value = int32_t(U32(data + at));
        at += 24;
      }
      out.push_back(std::move(e));
    }
  }
  if (end != nullptr) *end = at;
  return at <= size;
}

// An animation set's entries in order: each one's name and the first clip its meta-animation
// plays (Play, or the first branch of a blend, random or sequence).
struct SetEntry {
  std::string name;
  uint32_t anim = 0;
  std::string primitive;
};

namespace detail {
// A meta-animation at `at`: 0 Play (ANIM, primitive index, name, CharAnimTime), 1 Blend and
// 2 PhaseBlend (two metas, f32, u8), 3 Random (count, then meta and u32 each), 4 Sequence (count,
// metas). Advances `at`; `first` gets the first Play met.
inline bool SkipMeta(const uint8_t* data, size_t size, size_t& at, SetEntry& first, int depth = 0) {
  if (depth > 8 || at + 4 > size) return false;
  const uint32_t type = U32(data + at);
  at += 4;
  if (type == 0) {
    if (at + 8 > size) return false;
    const uint32_t anim = U32(data + at);
    at += 8;
    std::string name;
    while (at < size && data[at] != 0) {
      if (!Printable(data[at])) return false;
      name += char(data[at++]);
    }
    at += 1 + 8;
    if (at > size) return false;
    if (first.anim == 0) {
      first.anim = anim;
      first.primitive = name;
    }
    return true;
  }
  if (type == 1 || type == 2) {
    if (!SkipMeta(data, size, at, first, depth + 1) || !SkipMeta(data, size, at, first, depth + 1)) return false;
    at += 5;
    return at <= size;
  }
  if (type == 3 || type == 4) {
    if (at + 4 > size) return false;
    const uint32_t count = U32(data + at);
    at += 4;
    if (count == 0 || count > 256) return false;
    for (uint32_t i = 0; i < count; ++i) {
      if (!SkipMeta(data, size, at, first, depth + 1)) return false;
      if (type == 3) at += 4;
    }
    return at <= size;
  }
  return false;
}
}  // namespace detail

// The animation set's entry list: the first place a count is followed by that many (name,
// meta-animation) entries. Empty when there is none.
inline std::vector<SetEntry> FindEntries(const uint8_t* data, size_t size) {
  for (size_t start = 0; start + 4 <= size; ++start) {
    const uint32_t count = U32(data + start);
    if (count < 2 || count > 2000) continue;
    std::vector<SetEntry> entries;
    size_t at = start + 4;
    bool ok = true;
    for (uint32_t i = 0; i < count && ok; ++i) {
      SetEntry e;
      while (at < size && data[at] != 0 && Printable(data[at])) e.name += char(data[at++]);
      ok = at < size && data[at] == 0 && !e.name.empty();
      ++at;
      ok = ok && detail::SkipMeta(data, size, at, e);
      entries.push_back(std::move(e));
    }
    if (ok) return entries;
  }
  return {};
}

// Echoes' animation sets end with one embedded event set per entry (a count, then that many
// EVNTs back to back, to the end of the file). `sets[i]` is entry i's. False when not found.
inline bool FindEventSets(const uint8_t* data, size_t size, size_t entries, std::vector<std::vector<Event>>& sets) {
  for (size_t start = 0; start + 4 <= size; ++start) {
    if (U32(data + start) != entries) continue;
    sets.clear();
    size_t at = start + 4;
    bool ok = true;
    for (size_t i = 0; i < entries && ok; ++i) {
      std::vector<Event> events;
      size_t used = 0;
      ok = at <= size && ParseEvents(data + at, size - at, events, &used);
      at += used;
      sets.push_back(std::move(events));
    }
    if (ok && at == size) return true;
  }
  sets.clear();
  return false;
}

// A character's animation-selection tables (its PAS database, "PAS4"): per state, the
// animations (indices into the set's entries) and the parameter values each is chosen by.
struct PasAnim {
  uint32_t entry = 0;  // an index into the character's own animation list (FindCharacterAnims)
  std::vector<uint32_t> values;  // a Real's bits, a Bool as 0 or 1
};
struct PasState {
  uint32_t id = 0;
  std::vector<PasAnim> anims;
};

// The PAS database at `at` ("PAS4", u32 states, u32 default; per state: id, parameter count,
// animation count, the parameters' (type, weight function, f32 weight, minimum, maximum), then
// per animation its entry index and one value per parameter; a Bool is one byte, the rest four).
inline bool ParsePas(const uint8_t* data, size_t size, size_t at, std::vector<PasState>& out,
                     size_t* end = nullptr) {
  out.clear();
  const auto need = [&](size_t n) { return at <= size && size - at >= n; };
  if (!need(12) || U32(data + at) != 0x50415334) return false;
  const uint32_t states = U32(data + at + 4);
  at += 12;
  if (states > 256) return false;
  for (uint32_t i = 0; i < states; ++i) {
    if (!need(12)) return false;
    PasState state;
    state.id = U32(data + at);
    const uint32_t parms = U32(data + at + 4);
    const uint32_t anims = U32(data + at + 8);
    at += 12;
    if (parms > 16 || anims > 1024) return false;
    std::vector<uint32_t> types;
    for (uint32_t j = 0; j < parms; ++j) {
      if (!need(12)) return false;
      const uint32_t type = U32(data + at);
      if (type > 4) return false;
      types.push_back(type);
      at += 12 + (type == 3 ? 2 : 8);
    }
    for (uint32_t j = 0; j < anims; ++j) {
      if (!need(4)) return false;
      PasAnim anim;
      anim.entry = U32(data + at);
      at += 4;
      for (const uint32_t type : types) {
        if (!need(type == 3 ? 1 : 4)) return false;
        anim.values.push_back(type == 3 ? data[at] : U32(data + at));
        at += type == 3 ? 1 : 4;
      }
      state.anims.push_back(std::move(anim));
    }
    out.push_back(std::move(state));
  }
  if (end != nullptr) *end = at;
  return true;
}

// A character's own animation list, which its PAS tables index: it ends where the tables start
// ("PAS4" at `pas`), a count and that many (index, name) pairs. `list[i]` is the name of the
// set entry the character's animation i plays.
inline bool FindCharacterAnims(const uint8_t* data, size_t size, size_t pas, std::vector<std::string>& list) {
  for (size_t back = 9; back <= pas && back < 65536; ++back) {
    const size_t start = pas - back;
    const uint32_t count = U32(data + start);
    if (count == 0 || count > 4096 || count * 6 > back) continue;
    list.clear();
    size_t at = start + 4;
    bool ok = true;
    for (uint32_t i = 0; i < count && ok; ++i) {
      ok = at + 5 <= pas;
      if (!ok) break;
      const uint32_t index = U32(data + at);
      at += 4;
      std::string name;
      while (at < pas && data[at] != 0 && Printable(data[at])) name += char(data[at++]);
      ok = at < pas && data[at] == 0 && index == i && !name.empty();
      ++at;
      list.push_back(std::move(name));
    }
    if (ok && at == pas) return true;
  }
  list.clear();
  return false;
}

constexpr uint32_t kATBL = 0x4154424C;
constexpr uint32_t kAGSC = 0x41475343;

// An audio group (AGSC, MusyX): its name and id, the sizes of its four parts, the ids of the
// sound macros and samples it holds, and its sound effects (the define id the game's sound id
// translates to through the ATBL, and the macro it starts).
struct AudioGroup {
  std::string name;
  uint32_t groupId = 0;
  size_t pool = 0, project = 0, sampleDirectory = 0, samples = 0;
  std::vector<uint16_t> macros, sampleIds;
  struct Sfx {
    uint16_t define = 0;
    uint16_t object = 0;
  };
  std::vector<Sfx> sfx;
};

// Echoes: u32 1, name, u16 group id, the four sizes (pool, project, sample directory, samples),
// then the parts in that order. Prime: directory and name strings, then each part behind its
// size (pool, project, samples, sample directory). Only the project's first group is read.
inline bool ParseAudioGroup(const uint8_t* data, size_t size, bool echoes, AudioGroup& out) {
  out = AudioGroup();
  size_t at = 0;
  const auto text = [&](std::string& to) {
    while (at < size && data[at] != 0) to += char(data[at++]);
    return at++ < size;
  };
  size_t projectAt = 0;
  if (echoes) {
    if (size < 8 || U32(data) != 1) return false;
    at = 4;
    if (!text(out.name) || at + 18 > size) return false;
    out.groupId = (uint32_t(data[at]) << 8) | data[at + 1];
    at += 2;
    out.pool = U32(data + at);
    out.project = U32(data + at + 4);
    out.sampleDirectory = U32(data + at + 8);
    out.samples = U32(data + at + 12);
    at += 16;
    projectAt = at + out.pool;
  } else {
    std::string directory;
    if (!text(directory) || !text(out.name) || at + 4 > size) return false;
    out.pool = U32(data + at);
    projectAt = at + 4 + out.pool;
    if (projectAt + 4 > size) return false;
    out.project = U32(data + projectAt);
    projectAt += 4;
    const size_t samplesAt = projectAt + out.project;
    if (samplesAt + 4 > size) return false;
    out.samples = U32(data + samplesAt);
    const size_t directoryAt = samplesAt + 4 + out.samples;
    if (directoryAt + 4 > size) return false;
    out.sampleDirectory = U32(data + directoryAt);
  }
  if (out.project < 32 || projectAt + out.project > size) return false;
  const uint8_t* project = data + projectAt;
  if (!echoes) out.groupId = (uint32_t(project[4]) << 8) | project[5];
  const auto ids = [&](uint32_t offset, std::vector<uint16_t>& to) {
    for (size_t i = offset; i + 2 <= out.project; i += 2) {
      const uint16_t id = uint16_t((project[i] << 8) | project[i + 1]);
      if (id == 0xFFFF) break;
      to.push_back(id);
    }
  };
  ids(U32(project + 8), out.macros);
  ids(U32(project + 12), out.sampleIds);
  const uint16_t type = uint16_t((project[6] << 8) | project[7]);
  const uint32_t sfxAt = U32(project + 28);
  if (type == 1 && sfxAt + 4 <= out.project) {
    const uint32_t count = (uint32_t(project[sfxAt]) << 8) | project[sfxAt + 1];
    for (uint32_t i = 0; i < count && sfxAt + 4 + (i + 1) * 10 <= out.project; ++i) {
      const uint8_t* e = project + sfxAt + 4 + i * 10;
      out.sfx.push_back({uint16_t((e[0] << 8) | e[1]), uint16_t((e[2] << 8) | e[3])});
    }
  }
  return true;
}

constexpr uint32_t kAFSM = 0x4146534D;

// A state machine (AFSM, as CStateMachine reads it): each state's name and its transitions, each
// a list of conditions (all must hold; '!' negates one) and the state it leads to.
struct MachineCondition {
  std::string name;
  float arg = 0.f;
};
struct MachineTransition {
  std::vector<MachineCondition> conditions;
  uint32_t target = 0;
};
struct MachineState {
  std::string name;
  std::vector<MachineTransition> transitions;
};

// u32 states, their names; u32 triggers in all; per state u32 transitions, per transition u32
// conditions, each a name and an f32, the first followed by the u32 target state.
inline bool ParseMachine(const uint8_t* data, size_t size, std::vector<MachineState>& out) {
  out.clear();
  size_t at = 0;
  const auto need = [&](size_t n) { return at <= size && size - at >= n; };
  const auto name = [&](std::string& to) {
    while (at < size && data[at] != 0) {
      if (!Printable(data[at]) || to.size() > 64) return false;
      to += char(data[at++]);
    }
    return at++ < size;
  };
  if (!need(4)) return false;
  const uint32_t states = U32(data);
  at = 4;
  if (states == 0 || states > 512) return false;
  out.resize(states);
  for (MachineState& state : out) {
    if (!name(state.name)) return false;
  }
  if (!need(4)) return false;
  at += 4;
  for (MachineState& state : out) {
    if (!need(4)) return false;
    const uint32_t transitions = U32(data + at);
    at += 4;
    if (transitions > 256) return false;
    state.transitions.resize(transitions);
    for (MachineTransition& transition : state.transitions) {
      if (!need(4)) return false;
      const uint32_t conditions = U32(data + at);
      at += 4;
      if (conditions == 0 || conditions > 32) return false;
      for (uint32_t k = 0; k < conditions; ++k) {
        MachineCondition condition;
        if (!name(condition.name) || !need(4)) return false;
        condition.arg = F32(data + at);
        at += 4;
        if (k == 0) {
          if (!need(4)) return false;
          transition.target = U32(data + at);
          at += 4;
          if (transition.target >= states) return false;
        }
        transition.conditions.push_back(std::move(condition));
      }
    }
  }
  return true;
}

}  // namespace PortAnimInspect
