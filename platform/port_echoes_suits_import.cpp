// The Echoes suits import in the game (port_remastered_import.h, StartEchoesSuitsImport): the user's
// Metroid Prime 2: Echoes GameCube image (or a Trilogy image's MP2 folder) and the open disc, through PortEchoesSuits::Import
// (port_echoes_suits.h), into <mods>/echoes-suits. Like the Dread Samus import it shares the
// Remastered import's state, thread and cancel flag; only one of them runs at a time.

#include "port_remastered_import_internal.h"

#include "port_anim_inspect.h"
#include "port_echoes_anim.h"
#include "port_screw_audio.h"
#include "port_screw_fx.h"
#include "port_paths.h"
#include "port_echoes_suits.h"
#include "port_utf8_path.h"

#include <aurora/dvd.h>

#include <cctype>
#include <cstring>

namespace PortRemastered {
namespace ImportDetail {
namespace {

constexpr const char* kEchoesStaging = ".echoes-suits.staging";
constexpr const char* kEchoesOld = ".echoes-suits.old";
constexpr const char* kEchoesDone = ".done";  // in the staging folder once it is complete

using PortUtf8::FromUtf8;

using PortUtf8::ToUtf8;

bool WriteFile(const fs::path& path, const std::vector<uint8_t>& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  return bool(out);
}

// The Echoes image's PAKs, indexed by resource; entries read and decompressed on demand.
class EchoesDisc {
public:
  ~EchoesDisc() {
    if (m_image != nullptr) {
      aurora_disc_image_close(m_image);
    }
  }

  bool Open(const std::string& path, std::string& error) {
    char gameId[6] = {};
    m_image = aurora_disc_image_open(path.c_str(), gameId, nullptr, nullptr);
    if (m_image == nullptr) {
      error = "Could not open the image (not a GameCube or Wii disc image, or unreadable).";
      return false;
    }
    // Metroid Prime Trilogy (R3ME01, R3MP01) carries Echoes' PAKs, the same files as the GameCube
    // disc's, in its MP2 folder; MP1 and MP3 have PAKs of the same names.
    const bool trilogy = std::memcmp(gameId, "R3M", 3) == 0;
    if (!trilogy && std::memcmp(gameId, "G2M", 3) != 0) {
      error = "This is not Metroid Prime 2: Echoes for GameCube or Metroid Prime Trilogy (game " +
              std::string(gameId, 6) + ").";
      return false;
    }
    struct Pak {
      uint32_t index;
      uint32_t size;
      std::string name;
    };
    std::vector<Pak> paks;
    aurora_disc_image_list(
        m_image,
        [](u32 index, const char* name, u32 size, void* user) {
          const size_t length = std::strlen(name);
          if (length > 4) {
            std::string ext(name + length - 4);
            for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".pak") {
              static_cast<std::vector<Pak>*>(user)->push_back({index, size, name});
            }
          }
        },
        &paks);
    for (const auto& [index, size, name] : paks) {
      if (trilogy && aurora_disc_image_find(m_image, ("MP2/" + name).c_str(), nullptr) != s32(index)) {
        continue;
      }
      std::vector<uint8_t> header;
      PortMods::PakTable table;
      size_t needed = 0x10000;
      bool parsed = false;
      while (needed <= (64u << 20)) {
        header.resize(std::min<size_t>(needed, size));
        if (!ReadAt(index, 0, header.data(), header.size())) {
          break;
        }
        if (PortMods::ParsePakTable(header.data(), header.size(), table, needed)) {
          parsed = true;
          break;
        }
        if (header.size() >= size || needed <= header.size()) {
          break;
        }
      }
      if (!parsed) {
        continue;
      }
      for (const PortMods::PakResource& resource : table.resources) {
        m_resources.try_emplace(Key(resource.type, resource.id), Entry{index, resource});
      }
      // The named resources (the header before the table): u32 count at 8, then type, id,
      // u32 length and the name.
      if (header.size() >= 12) {
        size_t at = 12;
        for (uint32_t n = PortAnimInspect::U32(header.data() + 8); n > 0 && at + 12 <= header.size(); --n) {
          const uint32_t length = PortAnimInspect::U32(header.data() + at + 8);
          if (at + 12 + length > header.size()) break;
          std::string name(reinterpret_cast<const char*>(header.data() + at + 12), length);
          for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
          m_names.try_emplace(name, Key(PortAnimInspect::U32(header.data() + at), PortAnimInspect::U32(header.data() + at + 4)));
          at += 12 + length;
        }
      }
    }
    if (m_resources.empty()) {
      error = "The image has no Echoes PAKs.";
      return false;
    }
    for (const auto& [key, entry] : m_resources) m_types.try_emplace(uint32_t(key), uint32_t(key >> 32));
    return true;
  }

  bool Has(uint32_t type, uint32_t id) const { return m_resources.count(Key(type, id)) != 0; }

  // A named resource's id (names as the game asks for them, any case); 0 when none.
  uint32_t Named(uint32_t type, std::string name) const {
    for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
    const auto found = m_names.find(name);
    return found != m_names.end() && uint32_t(found->second >> 32) == type ? uint32_t(found->second) : 0;
  }

  // The type of the resource with this id, 0 when none (ids are unique across types in practice).
  uint32_t TypeOf(uint32_t id) const {
    const auto found = m_types.find(id);
    if (found != m_types.end()) return found->second;
    return 0;
  }

  // The ids of every resource of `type`, in id order.
  std::vector<uint32_t> Ids(uint32_t type) const {
    std::vector<uint32_t> ids;
    for (auto it = m_resources.lower_bound(Key(type, 0)); it != m_resources.end() && (it->first >> 32) == type; ++it) {
      ids.push_back(uint32_t(it->first));
    }
    return ids;
  }

  bool Read(uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
    const auto found = m_resources.find(Key(type, id));
    if (found == m_resources.end()) {
      return false;
    }
    const Entry& entry = found->second;
    std::vector<uint8_t> raw(entry.resource.size);
    if (!ReadAt(entry.file, entry.resource.offset, raw.data(), raw.size())) {
      return false;
    }
    if (entry.resource.compressed == 0) {
      out = std::move(raw);
      return true;
    }
    std::string why;
    if (!PortEchoesSuits::Decompress(raw.data(), raw.size(), out, &why)) {
      char id8[9];
      std::snprintf(id8, sizeof(id8), "%08X", id);
      PortLog::Write("echoes: %s: %s\n", id8, why.c_str());
      return false;
    }
    return true;
  }

private:
  struct Entry {
    uint32_t file;
    PortMods::PakResource resource;
  };
  static uint64_t Key(uint32_t type, uint32_t id) { return (uint64_t(type) << 32) | id; }

  bool ReadAt(uint32_t file, uint64_t offset, uint8_t* out, size_t size) {
    void* handle = aurora_disc_image_file_open(m_image, file);
    if (handle == nullptr) {
      return false;
    }
    size_t done = 0;
    if (aurora_dvd_base_seek(handle, int64_t(offset), 0) == int64_t(offset)) {
      while (done < size) {
        const int64_t got = aurora_dvd_base_read(handle, out + done, size - done);
        if (got <= 0) {
          break;
        }
        done += size_t(got);
      }
    }
    aurora_dvd_base_close(handle);
    return done == size;
  }

  AuroraDiscImage* m_image = nullptr;
  std::map<uint64_t, Entry> m_resources;
  std::map<std::string, uint64_t> m_names;
  std::map<uint32_t, uint32_t> m_types;
};

// The finished staging folder in place of <mods>/echoes-suits. False (staging kept, for the next
// start's ApplyPendingImport) when a rename fails, as it can on Windows with a file still open.
bool MoveEchoesIntoPlace(const fs::path& mods) {
  std::error_code ec;
  const fs::path staging = mods / kEchoesStaging;
  if (!fs::exists(staging / kEchoesDone, ec)) {
    return false;
  }
  const fs::path target = mods / PortEchoesSuits::kModName;
  const fs::path old = mods / kEchoesOld;
  fs::remove_all(old, ec);
  ec.clear();
  const bool had = fs::exists(target, ec);
  if (had) {
    fs::rename(target, old, ec);
    if (ec) {
      PortLog::Write("echoes: could not set aside %s: %s\n", ToUtf8(target).c_str(), ec.message().c_str());
      return false;
    }
  }
  fs::rename(staging, target, ec);
  if (ec) {
    PortLog::Write("echoes: could not move the import to %s: %s\n", ToUtf8(target).c_str(), ec.message().c_str());
    if (had) {
      std::error_code restore;
      fs::rename(old, target, restore);
    }
    return false;
  }
  fs::remove(target / kEchoesDone, ec);
  fs::remove_all(old, ec);
  return true;
}

constexpr uint32_t kAncsType = 0x414E4353;
constexpr uint32_t kEchoesGameCinf = 0xF86193FE;  // Echoes in-game Samus (port_echoes_suits.h "game20")
constexpr uint32_t kPrimeGameCinf = 0xA496DA02;   // Prime's BallTransitionsANCS

// The Screw Attack's clips (port_echoes_anim.h): found by name in Echoes' in-game Samus sets,
// decoded, retargeted onto Prime's in-game skeleton and written to the user folder. `summary`
// says what was baked.
// The Screw Attack's sounds (port_screw_audio.h): the Echoes groups that hold them, in Prime's
// layout with only these sounds left in their tables, renumbered past Prime's define ids.
// Returns what was baked, or why not; a failure here does not fail the animation bake.
std::string BakeScrewAudio(EchoesDisc& echoes, Retail& retail) {
  namespace SA = PortScrewAudio;
  using PortAnimInspect::AudioGroup;
  std::vector<uint16_t> table;
  for (const uint32_t id : echoes.Ids(PortAnimInspect::kATBL)) {
    std::vector<uint8_t> bytes;
    if (echoes.Read(PortAnimInspect::kATBL, id, bytes) && bytes.size() >= 4) {
      const uint32_t count = PortAnimInspect::U32(bytes.data());
      for (uint32_t i = 0; i < count && 4 + (i + 1) * 2 <= bytes.size(); ++i) {
        table.push_back(uint16_t((bytes[4 + i * 2] << 8) | bytes[5 + i * 2]));
      }
      break;
    }
  }
  if (table.empty()) return "no sounds: Echoes' sound id table is unreadable";
  uint16_t echoesDefine[SA::kSoundCount];
  for (int i = 0; i < SA::kSoundCount; ++i) {
    const uint16_t id = SA::EchoesSoundId(SA::ESound(i));
    echoesDefine[i] = id < table.size() ? table[id] : SA::kNoSound;
  }

  // Prime's ids: the new define ids go past its highest; a group, macro or sample id Prime
  // already uses would make MusyX keep Prime's.
  uint32_t primeTop = 0;
  std::vector<bool> primeMacro(65536), primeSample(65536), primeGroup(65536);
  for (const uint32_t id : retail.Ids(PortAnimInspect::kAGSC)) {
    std::vector<uint8_t> bytes;
    AudioGroup group;
    if (retail.Read(PortAnimInspect::kAGSC, id, bytes) &&
        PortAnimInspect::ParseAudioGroup(bytes.data(), bytes.size(), false, group)) {
      primeGroup[group.groupId & 0xFFFF] = true;
      for (const uint16_t m : group.macros) primeMacro[m] = true;
      for (const uint16_t m : group.sampleIds) primeSample[m] = true;
      for (const auto& fx : group.sfx) primeTop = std::max<uint32_t>(primeTop, fx.define);
    }
  }
  if (primeTop == 0) return "no sounds: Prime's audio groups are unreadable";
  const uint32_t base = primeTop + 1;
  if (base + SA::kSoundCount >= SA::kNoSound) return "no sounds: no free define ids";

  SA::File file;
  std::string notes;
  for (const uint32_t id : echoes.Ids(PortAnimInspect::kAGSC)) {
    std::vector<uint8_t> bytes;
    AudioGroup group;
    if (!echoes.Read(PortAnimInspect::kAGSC, id, bytes) ||
        !PortAnimInspect::ParseAudioGroup(bytes.data(), bytes.size(), true, group)) {
      continue;
    }
    std::vector<std::pair<uint16_t, uint16_t>> keep;
    for (int i = 0; i < SA::kSoundCount; ++i) {
      if (file.defines[i] != SA::kNoSound || echoesDefine[i] == SA::kNoSound) continue;
      for (const auto& fx : group.sfx) {
        if (fx.define == echoesDefine[i]) {
          keep.push_back({fx.define, uint16_t(base + i)});
          file.defines[i] = uint16_t(base + i);
          break;
        }
      }
    }
    if (keep.empty()) continue;
    if (primeGroup[group.groupId & 0xFFFF]) {
      notes += "group " + std::to_string(group.groupId) + " is a Prime group id; ";
      for (const auto& k : keep) {
        for (int i = 0; i < SA::kSoundCount; ++i) {
          if (file.defines[i] == k.second) file.defines[i] = SA::kNoSound;
        }
      }
      continue;
    }
    // MusyX keeps the first macro or sample pushed under an id, so a shared id plays Prime's.
    std::string clashes;
    for (const uint16_t m : group.macros) {
      if (!primeMacro[m]) continue;
      clashes += " macro " + std::to_string(m);
      for (const auto& fx : group.sfx) {
        for (const auto& k : keep) {
          if (fx.define == k.first && fx.object == m) clashes += " (a screw sound's own)";
        }
      }
    }
    for (const uint16_t m : group.sampleIds) {
      if (primeSample[m]) clashes += " sample " + std::to_string(m);
    }
    if (!clashes.empty()) notes += "'" + group.name + "' shares ids with Prime:" + clashes + "; ";
    SA::Group baked;
    std::string why;
    if (!SA::ConvertGroup(bytes.data(), bytes.size(), keep, baked.agsc, baked.id, &why)) {
      notes += "'" + group.name + "': " + why + "; ";
      for (const auto& k : keep) {
        for (int i = 0; i < SA::kSoundCount; ++i) {
          if (file.defines[i] == k.second) file.defines[i] = SA::kNoSound;
        }
      }
      continue;
    }
    file.groups.push_back(std::move(baked));
  }
  size_t sounds = 0;
  for (int i = 0; i < SA::kSoundCount; ++i) {
    if (file.defines[i] != SA::kNoSound) {
      ++sounds;
    } else {
      notes += std::string(SA::SoundName(SA::ESound(i))) + " not found; ";
    }
  }
  if (file.groups.empty()) return "no sounds: " + notes;
  const fs::path out = FromUtf8(PortPaths::UserFolder()) / SA::kFileName;
  if (!WriteFile(out, SA::Write(file))) return "no sounds: cannot write " + ToUtf8(out);
  char line[160];
  std::snprintf(line, sizeof(line), "%zu screw attack sounds in %zu groups (define ids from %u), to %s", sounds,
                file.groups.size(), base, ToUtf8(out).c_str());
  return line + (notes.empty() ? std::string() : " (" + notes + ")");
}

// The Screw Attack's particle effects (port_screw_fx.h): the spin clip's three and the jump
// flash, with every resource they reach (child effects, textures), renumbered into the port's
// provided id range and written to the user folder. Returns what was baked, or why not.
std::string BakeScrewEffects(EchoesDisc& echoes) {
  namespace FX = PortScrewFx;
  constexpr uint32_t kPART = 0x50415254, kTXTR = 0x54585452, kCMDL = 0x434D444C, kSWHC = 0x53574843,
                     kELSC = 0x454C5343;
  uint32_t roots[FX::kEffectCount] = {FX::kSpinA, FX::kSpinB, FX::kSpinC, 0};
  roots[FX::kJumpFlash] = echoes.Named(kPART, "ScrewAttackJumpFlash");
  std::string notes;
  if (roots[FX::kJumpFlash] == 0) notes += "ScrewAttackJumpFlash not found; ";

  // Breadth first from the roots: each resource's bytes, scanned at every offset for the ids of
  // resources on the Echoes disc (an id is four random bytes, so a stray match is unlikely).
  FX::File file;
  std::map<uint32_t, uint32_t> renumber;
  std::vector<std::pair<uint32_t, uint32_t>> queue;  // (id, type)
  const auto add = [&](uint32_t id, uint32_t type) {
    if (renumber.count(id) != 0) return;
    renumber[id] = FX::kFirstId + uint32_t(renumber.size());
    queue.push_back({id, type});
  };
  for (const uint32_t root : roots) {
    if (root != 0) add(root, kPART);
  }
  std::vector<std::vector<uint8_t>> bytes;
  std::vector<std::vector<size_t>> refs;
  for (size_t q = 0; q < queue.size() && renumber.size() < FX::kMaxResources; ++q) {
    const auto [id, type] = queue[q];
    std::vector<uint8_t> data;
    if (!echoes.Read(type, id, data)) {
      char text[64];
      std::snprintf(text, sizeof(text), "%08X unreadable; ", id);
      notes += text;
    }
    std::vector<size_t> at;
    if (type == kCMDL) {
      // Echoes' version 4 model as Prime's version 2 (as the suits import does); its textures
      // follow, their ids patched where the converted material sets name them.
      std::vector<uint8_t> converted;
      std::vector<uint32_t> textures;
      std::string why;
      if (!PortEchoesSuits::ConvertCmdl(data, converted, textures, &why)) {
        char text[96];
        std::snprintf(text, sizeof(text), "model %08X: %s; ", id, why.c_str());
        notes += text;
      }
      data = std::move(converted);
      for (size_t i = 0; i + 4 <= data.size(); ++i) {
        const uint32_t ref = PortAnimInspect::U32(data.data() + i);
        if (std::find(textures.begin(), textures.end(), ref) != textures.end() && echoes.TypeOf(ref) == kTXTR) {
          add(ref, kTXTR);
          at.push_back(i);
        }
      }
    }
    if (type == kPART || type == kSWHC || type == kELSC) {
      for (size_t i = 0; i + 4 <= data.size(); ++i) {
        const uint32_t ref = PortAnimInspect::U32(data.data() + i);
        const uint32_t refType = ref != 0 && ref != 0xFFFFFFFF && ref != id ? echoes.TypeOf(ref) : 0;
        if (refType == kPART || refType == kTXTR || refType == kSWHC || refType == kELSC || refType == kCMDL) {
          add(ref, refType);
          at.push_back(i);
        }
      }
    }
    bytes.push_back(std::move(data));
    refs.push_back(std::move(at));
  }
  for (size_t q = 0; q < queue.size() && q < bytes.size(); ++q) {
    std::vector<uint8_t>& data = bytes[q];
    for (const size_t i : refs[q]) {
      const uint32_t to = renumber[PortAnimInspect::U32(data.data() + i)];
      for (int b = 0; b < 4; ++b) data[i + b] = uint8_t(to >> (24 - 8 * b));
    }
    file.resources.push_back({renumber[queue[q].first], queue[q].second, std::move(data)});
  }
  for (int i = 0; i < FX::kEffectCount; ++i) {
    file.effects[i] = roots[i] != 0 ? renumber[roots[i]] : 0;
  }
  const fs::path out = FromUtf8(PortPaths::UserFolder()) / FX::kFileName;
  if (!WriteFile(out, FX::Write(file))) return "no effects: cannot write " + ToUtf8(out);
  size_t parts = 0, textures = 0, models = 0;
  for (const auto& r : file.resources) {
    parts += r.type == kPART;
    textures += r.type == kTXTR;
    models += r.type == kCMDL;
  }
  char line[200];
  std::snprintf(line, sizeof(line), "%zu screw attack effects (%zu resources: %zu PART, %zu CMDL, %zu TXTR), to %s",
                parts, file.resources.size(), parts, models, textures, ToUtf8(out).c_str());
  return line + (notes.empty() ? std::string() : " (" + notes + ")");
}

bool BakeScrewAttack(EchoesDisc& echoes, Retail& retail, std::string& summary, std::string& error) {
  namespace EA = PortEchoesAnim;
  std::vector<uint8_t> data;
  std::vector<PortEchoesSuits::EchoesJoint> echoesCinf;
  PortDread::PrimeSkeleton primeCinf;
  std::string why;
  if (!echoes.Read(PortAnimInspect::kCINF, kEchoesGameCinf, data) ||
      !PortEchoesSuits::ParseCinf(data.data(), data.size(), echoesCinf, &why)) {
    error = "Echoes' Samus skeleton is unreadable" + (why.empty() ? std::string() : ": " + why);
    return false;
  }
  if (!retail.Read(PortAnimInspect::kCINF, kPrimeGameCinf, data) ||
      !PortDread::ParseCinf(data.data(), data.size(), primeCinf, &why)) {
    error = "Prime's Samus skeleton is unreadable" + (why.empty() ? std::string() : ": " + why);
    return false;
  }
  std::vector<EA::Joint> echoesJoints;
  for (const auto& j : echoesCinf) echoesJoints.push_back({j.id, j.parent, j.name, {}});
  std::vector<EA::Joint> primeJoints;
  for (const auto& j : primeCinf.joints) {
    primeJoints.push_back({j.id, j.parent, j.name, {j.head[0], j.head[1], j.head[2]}});
  }

  // Each clip's ANIM, from the first set on Echoes' in-game skeleton that names it.
  const char* const names[] = {EA::kInto,     EA::kLoop,  EA::kOut,  EA::kWallBump,
                               EA::kLand,     EA::kFall,  EA::kFloor, EA::kHardLand,
                               EA::kWalk,     EA::kJog,   EA::kRun};
  constexpr size_t kClips = sizeof(names) / sizeof(names[0]);
  uint32_t ids[kClips] = {};
  const PortAnimInspect::Known known = [&echoes](uint32_t type, uint32_t id) { return echoes.Has(type, id); };
  for (const uint32_t id : echoes.Ids(kAncsType)) {
    std::vector<uint8_t> ancs;
    if (!echoes.Read(kAncsType, id, ancs) || !PortAnimInspect::Mentions(ancs.data(), ancs.size(), kEchoesGameCinf)) {
      continue;
    }
    for (const auto& play : PortAnimInspect::FindPlays(ancs.data(), ancs.size(), known)) {
      for (size_t i = 0; i < kClips; ++i) {
        if (ids[i] == 0 && play.primitive == names[i]) ids[i] = play.anim;
      }
    }
  }
  EA::File file;
  for (size_t i = 0; i < kClips; ++i) {
    if (ids[i] == 0) {
      summary += std::string(names[i]) + " not found; ";
      continue;
    }
    EA::Anim anim;
    EA::Baked baked;
    if (!echoes.Read(PortAnimInspect::kANIM, ids[i], data) || !EA::Decode(data.data(), data.size(), anim, &why) ||
        !EA::Retarget(anim, echoesJoints, primeJoints, names[i], baked, &why)) {
      summary += std::string(names[i]) + ": " + why + "; ";
      continue;
    }
    file.clips.push_back(std::move(baked));
  }
  const EA::Baked* loop = file.Find(EA::kLoop);
  if (loop == nullptr) {
    error = "Echoes' screw attack spin (" + std::string(EA::kLoop) + ") could not be converted: " + summary;
    return false;
  }
  EA::Vec3 sum;
  for (uint32_t f = 0; f < loop->frames; ++f) sum = EA::Add(sum, EA::Middle(*loop, primeJoints, f));
  file.centre = {sum.x / loop->frames, sum.y / loop->frames, sum.z / loop->frames};
  const fs::path out = FromUtf8(PortPaths::UserFolder()) / EA::kFileName;
  if (!WriteFile(out, EA::Write(file))) {
    error = "cannot write " + ToUtf8(out);
    return false;
  }
  char line[160];
  std::snprintf(line, sizeof(line), "%zu screw attack clips, centre %.2f %.2f %.2f, to %s", file.clips.size(),
                file.centre.x, file.centre.y, file.centre.z, ToUtf8(out).c_str());
  summary = line + (summary.empty() ? std::string() : " (" + summary + ")");
  summary += "; " + BakeScrewAudio(echoes, retail);
  summary += "; " + BakeScrewEffects(echoes);
  return true;
}

void RunEchoes(std::string imagePath, fs::path mods) {
  auto fail = [](const std::string& message) { Finish(false, sCancel ? std::string("Cancelled.") : message); };
  SetMessage("Opening the Echoes image");
  std::string error;
  EchoesDisc echoes;
  if (!echoes.Open(imagePath, error)) {
    fail(error);
    return;
  }
  SetMessage("Reading the disc");
  Retail retail;
  if (!retail.Index(error)) {
    fail(error);
    return;
  }
  const fs::path staging = mods / kEchoesStaging;
  std::error_code ec;
  fs::remove_all(staging, ec);
  ec.clear();
  fs::create_directories(staging, ec);
  if (ec) {
    fail("Cannot write " + ToUtf8(staging) + ": " + ec.message());
    return;
  }
  PortEchoesSuits::Io io;
  io.echoes = [&echoes](uint32_t type, uint32_t id, std::vector<uint8_t>& out) { return echoes.Read(type, id, out); };
  io.retail = [&retail](uint32_t type, uint32_t id, std::vector<uint8_t>& out) { return retail.Read(type, id, out); };
  io.retailId = [&retail](uint32_t id) { return retail.HasId(id); };
  io.write = [&staging](const std::string& name, const std::vector<uint8_t>& data) {
    return WriteFile(staging / FromUtf8(name), data);
  };
  io.log = [](const std::string& line) {
    PortLog::Write("%s\n", line.c_str());
    AddLine(line);
  };
  io.cancelled = [] { return sCancel.load(); };
  io.progress = [](size_t done, size_t total) {
    {
      std::lock_guard<std::mutex> lock(sStateMutex);
      sState.done = int(done);
      sState.total = int(total);
    }
    SetMessage("Converting Samus's suits (" + std::to_string(done) + " of " + std::to_string(total) + ")");
  };
  {
    std::string screwSummary;
    std::string screwError;
    if (BakeScrewAttack(echoes, retail, screwSummary, screwError)) {
      PortLog::Write("echoes: %s\n", screwSummary.c_str());
    } else {
      PortLog::Write("echoes: screw attack animation: %s\n", screwError.c_str());
    }
  }
  PortEchoesSuits::Result result;
  if (!PortEchoesSuits::Import(io, result, error)) {
    fs::remove_all(staging, ec);
    fail(error);
    return;
  }
  const std::string stamp =
      std::to_string(PortEchoesSuits::kVersion) + "\n" + std::string(MP_BUILD_REVISION) + "\n";
  if (!WriteFile(staging / PortEchoesSuits::kStampName, std::vector<uint8_t>(stamp.begin(), stamp.end())) ||
      !WriteFile(staging / kEchoesDone, {})) {
    fs::remove_all(staging, ec);
    fail("Cannot write " + ToUtf8(staging));
    return;
  }
  if (sCancel.load()) {
    fs::remove_all(staging, ec);
    fail("cancelled");
    return;
  }
  const bool placed = MoveEchoesIntoPlace(mods);
  std::string message = std::to_string(result.converted + result.arms) + " suit models, " +
                        std::to_string(result.balls) + " Morph Balls and the arm cannon converted";
  message += placed ? "." : "; it is installed at the next start.";
  Finish(true, message);
}

}  // namespace

bool ApplyPendingEchoesImport() {
  const std::string mods = PortMods::Folder();
  return !mods.empty() && MoveEchoesIntoPlace(FromUtf8(mods));
}

}  // namespace ImportDetail

using namespace ImportDetail;

bool StartEchoesSuitsImport(const std::string& imagePath) {
  {
    std::lock_guard<std::mutex> lock(sStateMutex);
    if (sState.running) {
      return false;
    }
  }
  if (sThread.joinable()) {
    sThread.join();
  }
  const std::string mods = PortMods::Folder();
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState = {};
  if (mods.empty()) {
    sState.finished = true;
    sState.message = "There is no mods folder to write to.";
    return false;
  }
  if (imagePath.empty()) {
    sState.finished = true;
    sState.message = "Pick the Metroid Prime 2: Echoes disc image.";
    return false;
  }
  PortLog::Write("echoes: importing Samus's suits from %s\n", imagePath.c_str());
  sCancel = false;
  sState.running = true;
  sState.message = "Starting";
  sThread = std::thread(RunEchoes, imagePath, FromUtf8(mods));
  return true;
}

// `--inspect-echoes-anims`: what the Screw Attack's animation import needs to know, written to
// echoes_anims_report.txt in the working folder (port_anim_inspect.h): Echoes Samus's animation
// sets (those naming her in-game or cutscene skeleton) with their characters and animations, the
// headers of every animation named for the Screw Attack, Prime's in-game Samus animation set, and
// both in-game skeletons' joints side by side.
int RunEchoesAnimInspectFromCommandLine(const std::string& imagePath) {
  using namespace PortAnimInspect;
  constexpr uint32_t kAncs = 0x414E4353;
  constexpr uint32_t kEchoesGameCinf = 0xF86193FE;  // port_echoes_suits.h kTargets "game20"
  constexpr uint32_t kEchoesCineCinf = 0xA27AFE83;
  constexpr uint32_t kPrimeGameCinf = 0xA496DA02;   // BallTransitionsANCS
  std::string error;
  EchoesDisc echoes;
  if (!echoes.Open(imagePath, error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  Retail retail;
  if (!retail.Index(error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  const Known echoesKnown = [&echoes](uint32_t type, uint32_t id) { return echoes.Has(type, id); };
  const Known primeKnown = [&retail](uint32_t type, uint32_t id) { return retail.HasResource(type, id); };
  std::string body;
  std::string summary;
  char line[512];
  const auto add = [&line](std::string& to) { to += line; to += '\n'; };

  const auto listSet = [&](const char* game, uint32_t id, const std::vector<uint8_t>& ancs, const Known& known,
                           bool isEchoes) {
    std::snprintf(line, sizeof(line), "%s ANCS %08X, %zu bytes", game, id, ancs.size());
    add(body);
    for (const Character& c : FindCharacters(ancs.data(), ancs.size(), known)) {
      std::snprintf(line, sizeof(line), "  character '%s': CMDL %08X CSKR %08X CINF %08X", c.name.c_str(), c.cmdl,
                    c.cskr, c.cinf);
      add(body);
    }
    for (const Play& p : FindPlays(ancs.data(), ancs.size(), known)) {
      std::snprintf(line, sizeof(line), "  ANIM %08X  entry '%s'  primitive '%s' #%u", p.anim, p.entry.c_str(),
                    p.primitive.c_str(), p.primitiveIndex);
      add(body);
      if (isEchoes && (ContainsNoCase(p.entry, "screw") || ContainsNoCase(p.primitive, "screw") ||
                       ContainsNoCase(p.primitive, "jumpland") || ContainsNoCase(p.primitive, "ballforwardto") ||
                       ContainsNoCase(p.primitive, "jumploop_fromready"))) {
        std::vector<uint8_t> anim;
        const std::string header =
            echoes.Read(kANIM, p.anim, anim) ? DescribeAnim(anim.data(), anim.size(), true) : "unreadable";
        std::snprintf(line, sizeof(line), "ANCS %08X ANIM %08X '%s' / '%s': %s", id, p.anim, p.entry.c_str(),
                      p.primitive.c_str(), header.c_str());
        add(summary);
      }
    }
    // The events of the Screw Attack's clips and of the jump clips its exit goes through: what
    // each clip starts (user events, sounds, particles) and when. Echoes embeds one event set per
    // entry at the end of the animation set.
    if (isEchoes) {
      const std::vector<SetEntry> entries = FindEntries(ancs.data(), ancs.size());
      std::vector<std::vector<Event>> sets;
      const bool found = !entries.empty() && FindEventSets(ancs.data(), ancs.size(), entries.size(), sets);
      std::snprintf(line, sizeof(line), "  %zu entries, event sets %s", entries.size(), found ? "found" : "not found");
      add(body);
      // Each character's selection tables for the states the Screw Attack's exits go through
      // (0 Locomotion, 1 MorphTransition, 3 Jump): which clip each set of parameters picks.
      if (Mentions(ancs.data(), ancs.size(), kEchoesGameCinf)) {
        int table = 0;
        for (size_t at = 0; at + 12 <= ancs.size(); ++at) {
          std::vector<PasState> states;
          size_t pasEnd = 0;
          if (U32(ancs.data() + at) != 0x50415334 || !ParsePas(ancs.data(), ancs.size(), at, states, &pasEnd)) {
            continue;
          }
          ++table;
          std::vector<std::string> own;
          if (!FindCharacterAnims(ancs.data(), ancs.size(), at, own)) {
            std::snprintf(line, sizeof(line), "  PAS table %d: the character's animation list was not found", table);
            add(body);
            continue;
          }
          for (const PasState& state : states) {
            for (const PasAnim& anim : state.anims) {
              std::string values;
              for (const uint32_t value : anim.values) values += " " + std::to_string(value);
              std::snprintf(line, sizeof(line), "  PAS table %d state %u:%s -> '%s'", table, state.id, values.c_str(),
                            anim.entry < own.size() ? own[anim.entry].c_str() : "?");
              add(body);
            }
          }
        }
      }
      for (size_t i = 0; found && i < entries.size(); ++i) {
        const SetEntry& entry = entries[i];
        if (!ContainsNoCase(entry.name, "screw") && !ContainsNoCase(entry.primitive, "screw") &&
            !ContainsNoCase(entry.primitive, "jumploop") && !ContainsNoCase(entry.primitive, "jumpland") &&
            !ContainsNoCase(entry.primitive, "ballforwardto")) {
          continue;
        }
        std::snprintf(line, sizeof(line), "ANCS %08X entry %zu '%s' ('%s'): %zu events", id, i, entry.name.c_str(),
                      entry.primitive.c_str(), sets[i].size());
        add(summary);
        for (const Event& e : sets[i]) {
          const char tag[5] = {char(e.fourcc >> 24), char(e.fourcc >> 16), char(e.fourcc >> 8), char(e.fourcc), 0};
          std::snprintf(line, sizeof(line), "    %c %.3f s  type %u  '%s'  value %d (0x%X)  %s %08X  '%s'", e.kind,
                        e.time, e.type, e.name.c_str(), e.value, uint32_t(e.value), e.fourcc != 0 ? tag : "-",
                        e.resource, e.locator.c_str());
          if (e.kind == 'P') {
            char more[64];
            std::snprintf(more, sizeof(more), "  scale %.3f  parent mode %u", e.scale, e.parent);
            std::strncat(line, more, sizeof(line) - std::strlen(line) - 1);
          }
          add(summary);
        }
      }
    }
  };

  size_t echoesSets = 0;
  for (const uint32_t id : echoes.Ids(kAncs)) {
    std::vector<uint8_t> ancs;
    if (!echoes.Read(kAncs, id, ancs) || (!Mentions(ancs.data(), ancs.size(), kEchoesGameCinf) &&
                                          !Mentions(ancs.data(), ancs.size(), kEchoesCineCinf))) {
      continue;
    }
    ++echoesSets;
    listSet("Echoes", id, ancs, echoesKnown, true);
  }
  size_t primeSets = 0;
  for (const uint32_t id : retail.Ids(kAncs)) {
    std::vector<uint8_t> ancs;
    if (!retail.Read(kAncs, id, ancs) || !Mentions(ancs.data(), ancs.size(), kPrimeGameCinf)) {
      continue;
    }
    ++primeSets;
    listSet("Prime", id, ancs, primeKnown, false);
  }

  // The two in-game skeletons, Echoes' joints matched to Prime's by name.
  std::vector<uint8_t> data;
  std::vector<PortEchoesSuits::EchoesJoint> echoesJoints;
  PortDread::PrimeSkeleton primeSkeleton;
  std::string why;
  if (echoes.Read(kCINF, kEchoesGameCinf, data) &&
      PortEchoesSuits::ParseCinf(data.data(), data.size(), echoesJoints, &why) &&
      retail.Read(kCINF, kPrimeGameCinf, data) && PortDread::ParseCinf(data.data(), data.size(), primeSkeleton, &why)) {
    std::snprintf(line, sizeof(line), "Echoes skeleton %08X, %zu joints (id parent name -> Prime id):",
                  kEchoesGameCinf, echoesJoints.size());
    add(body);
    size_t matched = 0;
    for (const auto& joint : echoesJoints) {
      const int index = primeSkeleton.Find(joint.name);
      matched += index >= 0;
      std::snprintf(line, sizeof(line), "  %3u %3u %-24s -> %s", joint.id, joint.parent, joint.name.c_str(),
                    index >= 0 ? std::to_string(primeSkeleton.joints[size_t(index)].id).c_str() : "-");
      add(body);
    }
    std::snprintf(line, sizeof(line), "Prime skeleton %08X, %zu joints (id parent name):", kPrimeGameCinf,
                  primeSkeleton.joints.size());
    add(body);
    for (const auto& joint : primeSkeleton.joints) {
      std::snprintf(line, sizeof(line), "  %3u %3u %s", joint.id, joint.parent, joint.name.c_str());
      add(body);
    }
    std::snprintf(line, sizeof(line), "Skeletons: %zu of Echoes' %zu joints have a Prime namesake (Prime has %zu)",
                  matched, echoesJoints.size(), primeSkeleton.joints.size());
    add(summary);
  } else {
    std::snprintf(line, sizeof(line), "Skeletons unreadable: %s", why.c_str());
    add(summary);
  }

  // The player's state machines (the ones with a ScrewAttack state): what CPlayerBodyController
  // runs, so which state, and so which clip, follows which.
  for (const uint32_t id : echoes.Ids(kAFSM)) {
    std::vector<uint8_t> bytes;
    std::vector<MachineState> states;
    if (!echoes.Read(kAFSM, id, bytes) || !ParseMachine(bytes.data(), bytes.size(), states)) {
      continue;
    }
    bool player = false;
    for (const MachineState& state : states) player = player || state.name == "ScrewAttack";
    if (!player) {
      continue;
    }
    std::snprintf(line, sizeof(line), "Echoes AFSM %08X, %zu states (state: conditions -> next):", id, states.size());
    add(body);
    for (const MachineState& state : states) {
      if (state.transitions.empty()) {
        std::snprintf(line, sizeof(line), "  %s: no way out", state.name.c_str());
        add(body);
      }
      for (const MachineTransition& transition : state.transitions) {
        std::string conditions;
        for (const MachineCondition& condition : transition.conditions) {
          char text[96];
          std::snprintf(text, sizeof(text), "%s%s(%g)", conditions.empty() ? "" : " & ", condition.name.c_str(),
                        condition.arg);
          conditions += text;
        }
        std::snprintf(line, sizeof(line), "  %s: %s -> %s", state.name.c_str(), conditions.c_str(),
                      states[transition.target].name.c_str());
        add(body);
      }
    }
  }

  // The Screw Attack's sounds: the audio groups that hold them (through Echoes' sound id table),
  // and how those groups' MusyX ids sit against Prime's own groups, which they would have to
  // share one sound system with.
  {
    struct Wanted {
      const char* name;
      uint32_t id;
    };
    const Wanted wanted[] = {
        {"SFXsam_b_screwin_00_oneshot", 0x019E},  {"SFXsam_b_screwout_00_oneshot", 0x01A3},
        {"SFXsam_b_screwjump_00_oneshot", 0x01DC}, {"SFXsam_b_screwout_01_oneshot", 0x01DD},
        {"SFXsam_a_screw_lp_00_looped", 0x2203},  {"SFXsam_a_screwhit_00_oneshot", 0x2204},
        {"SFXsam_a_screwhit_01_oneshot", 0x032D}, {"SFXsam_a_screw_lp_01_looped", 0x04A1},
    };
    std::vector<uint16_t> table;
    for (const uint32_t id : echoes.Ids(kATBL)) {
      std::vector<uint8_t> bytes;
      if (echoes.Read(kATBL, id, bytes) && bytes.size() >= 4) {
        const uint32_t count = U32(bytes.data());
        for (uint32_t i = 0; i < count && 4 + (i + 1) * 2 <= bytes.size(); ++i) {
          table.push_back(uint16_t((bytes[4 + i * 2] << 8) | bytes[5 + i * 2]));
        }
        std::snprintf(line, sizeof(line), "Echoes ATBL %08X: %u sound ids", id, count);
        add(body);
        break;
      }
    }
    std::vector<AudioGroup> primeGroups;
    std::vector<bool> primeMacro(65536), primeSample(65536), primeDefine(65536);
    for (const uint32_t id : retail.Ids(kAGSC)) {
      std::vector<uint8_t> bytes;
      AudioGroup group;
      if (retail.Read(kAGSC, id, bytes) && ParseAudioGroup(bytes.data(), bytes.size(), false, group)) {
        for (const uint16_t m : group.macros) primeMacro[m] = true;
        for (const uint16_t m : group.sampleIds) primeSample[m] = true;
        for (const auto& fx : group.sfx) primeDefine[fx.define] = true;
        primeGroups.push_back(std::move(group));
      }
    }
    std::snprintf(line, sizeof(line), "Audio: Prime has %zu audio groups; Echoes' sound id table has %zu ids",
                  primeGroups.size(), table.size());
    add(summary);
    size_t echoesGroups = 0;
    for (const uint32_t id : echoes.Ids(kAGSC)) {
      std::vector<uint8_t> bytes;
      AudioGroup group;
      if (!echoes.Read(kAGSC, id, bytes) || !ParseAudioGroup(bytes.data(), bytes.size(), true, group)) {
        continue;
      }
      ++echoesGroups;
      std::string found;
      for (const Wanted& w : wanted) {
        if (w.id >= table.size()) continue;
        for (const auto& fx : group.sfx) {
          if (fx.define == table[w.id]) {
            char text[160];
            std::snprintf(text, sizeof(text), "\n    %s (0x%04X) -> define %u, macro %u%s", w.name, w.id, fx.define,
                          fx.object, primeDefine[fx.define] ? " (Prime uses this define id)" : "");
            found += text;
          }
        }
      }
      if (found.empty()) continue;
      size_t macroClash = 0, sampleClash = 0;
      for (const uint16_t m : group.macros) macroClash += primeMacro[m];
      for (const uint16_t m : group.sampleIds) sampleClash += primeSample[m];
      bool groupClash = false;
      for (const AudioGroup& g : primeGroups) groupClash = groupClash || g.groupId == group.groupId;
      std::snprintf(line, sizeof(line),
                    "Audio: Echoes AGSC %08X '%s' group %u%s: pool %zu, project %zu, sample directory %zu, samples %zu "
                    "bytes; %zu macros (%zu ids Prime uses), %zu samples (%zu ids Prime uses), %zu sounds",
                    id, group.name.c_str(), group.groupId, groupClash ? " (a Prime group id)" : "", group.pool,
                    group.project, group.sampleDirectory, group.samples, group.macros.size(), macroClash,
                    group.sampleIds.size(), sampleClash, group.sfx.size());
      summary += line + found + "\n";
    }
    std::snprintf(line, sizeof(line), "Audio: Echoes has %zu audio groups", echoesGroups);
    add(summary);
  }

  // The Screw Attack's particle effects (the spin clip's three and the landing clip's one, from
  // their events): each file's size and the four-letter keys in it, to hold against the keys
  // Prime's particle loader knows (CParticleDataFactory.cpp).
  {
    constexpr uint32_t kPART = 0x50415254;
    const uint32_t effects[] = {0xAB3E7D01, 0x82056882, 0xDD0ACDCB, 0x1EEF8A44};
    for (const uint32_t id : effects) {
      std::vector<uint8_t> bytes;
      if (!echoes.Read(kPART, id, bytes)) {
        std::snprintf(line, sizeof(line), "Particles: Echoes PART %08X is not on the disc", id);
        add(summary);
        continue;
      }
      std::vector<std::string> keys;
      for (size_t at = 0; at + 4 <= bytes.size(); ++at) {
        bool key = true;
        for (size_t i = 0; i < 4; ++i) {
          const uint8_t c = bytes[at + i];
          key = key && ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_');
        }
        if (!key) continue;
        const std::string text(reinterpret_cast<const char*>(bytes.data() + at), 4);
        bool seen = false;
        for (const std::string& k : keys) seen = seen || k == text;
        if (!seen) keys.push_back(text);
        at += 3;
      }
      std::string list;
      for (const std::string& k : keys) list += " " + k;
      std::snprintf(line, sizeof(line), "Particles: Echoes PART %08X, %zu bytes, %zu keys:", id, bytes.size(), keys.size());
      summary += line + list + "\n";
    }
  }

  std::string report = "Echoes animation inspection (" + imagePath + ")\n";
  std::snprintf(line, sizeof(line), "%zu Echoes and %zu Prime Samus animation sets", echoesSets, primeSets);
  add(report);
  report += "== Summary ==\n" + summary + "\n== Detail ==\n" + body;
  const fs::path out = fs::current_path() / "echoes_anims_report.txt";
  if (!WriteFile(out, std::vector<uint8_t>(report.begin(), report.end()))) {
    std::fprintf(stderr, "cannot write %s\n", ToUtf8(out).c_str());
    return 1;
  }
  std::printf("%s== Summary ==\n%s\nwritten to %s\n", report.substr(0, report.find("== Summary ==")).c_str(),
              summary.c_str(), ToUtf8(out).c_str());
  return 0;
}

int RunEchoesScrewImportFromCommandLine(const std::string& imagePath) {
  std::string error;
  EchoesDisc echoes;
  if (!echoes.Open(imagePath, error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  Retail retail;
  if (!retail.Index(error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  std::string summary;
  if (!BakeScrewAttack(echoes, retail, summary, error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  std::printf("%s\n", summary.c_str());
  return 0;
}

int RunEchoesSuitsImportFromCommandLine(const std::string& imagePath) {
  if (!StartEchoesSuitsImport(imagePath)) {
    std::fprintf(stderr, "%s\n", ImportStatus().message.c_str());
    return 1;
  }
  std::string shown;
  for (;;) {
    const ImportState state = ImportStatus();
    if (state.message != shown) {
      shown = state.message;
      std::printf("%s\n", shown.c_str());
      std::fflush(stdout);
    }
    if (!state.running) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  StopImport();
  const ImportState state = ImportStatus();
  for (const std::string& line : state.lines) {
    std::printf("  %s\n", line.c_str());
  }
  return state.ok ? 0 : 1;
}

}  // namespace PortRemastered
