// The Screw Attack's sounds from Echoes: loading the baked groups and playing them. See
// port_screw_audio.h.
#include "port_screw_audio.h"

#include "port_log.h"
#include "port_paths.h"
#include "port_utf8_path.h"

#include "Kyoto/Audio/CAudioGroupSet.hpp"
#include "Kyoto/Audio/CSfxHandle.hpp"
#include "Kyoto/Audio/CSfxManager.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "musyx/musyx.h"
#include "rstl/auto_ptr.hpp"

#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>

namespace PortScrewAudio {
namespace {

struct State {
  File file;
  // Prime's own loader converts each group to native layout; MusyX reads samples from these
  // buffers while the group is pushed, so they live until Unload has popped it.
  std::vector<std::unique_ptr<CAudioGrpSetLoc>> groups;
  size_t pushed = 0;
  bool loaded = false;
  std::string status = "not loaded";
  CSfxHandle loop;
};
State s;

}  // namespace

void Load() {
  if (s.loaded) {
    return;
  }
  s.loaded = true;
  const std::string path = PortPaths::UserFolder() + kFileName;
  std::ifstream in(PortUtf8::FromUtf8(path), std::ios::binary);
  if (!in) {
    s.status = "not imported (import the Echoes suits, or run --import-echoes-screw)";
    return;
  }
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  File file;
  if (!Read(data.data(), data.size(), file)) {
    s.status = "unreadable; import it again";
    return;
  }
  for (const Group& g : file.groups) {
    rstl::auto_ptr<uchar[]> bytes(new uchar[g.agsc.size()]);
    std::memcpy(bytes.get(), g.agsc.data(), g.agsc.size());
    std::unique_ptr<CAudioGrpSetLoc> loc;
    try {
      loc.reset(new CAudioGrpSetLoc(bytes, int(g.agsc.size())));
    } catch (const std::exception& e) {
      PortLog::Write("screw audio: group %u: %s\n", g.id, e.what());
      break;
    }
    if (!sndPushGroup(loc->GetProject(), g.id, loc->GetSample(), loc->GetSampleDir(), loc->GetPool())) {
      PortLog::Write("screw audio: MusyX would not take group %u\n", g.id);
      break;
    }
    s.groups.push_back(std::move(loc));
    ++s.pushed;
  }
  if (s.pushed != file.groups.size()) {
    Unload();
    s.loaded = true;
    s.status = "MusyX refused a group; see the log";
    return;
  }
  s.file = file;
  size_t sounds = 0;
  for (const uint16_t d : s.file.defines) sounds += d != kNoSound;
  char line[96];
  std::snprintf(line, sizeof(line), "%zu sounds from Echoes in %zu groups", sounds, s.pushed);
  s.status = line;
  PortLog::Write("screw audio: %s\n", line);
}

void Unload() {
  StopLoop();
  for (; s.pushed > 0; --s.pushed) {
    sndPopGroup();
  }
  s.groups.clear();
  s.file = File();
  s.loaded = false;
  s.status = "not loaded";
}

const std::string& Status() { return s.status; }

uint16_t Define(ESound sound) { return sound < kSoundCount ? s.file.defines[sound] : kNoSound; }

void Play(ESound sound, const CVector3f& at) {
  const uint16_t id = Define(sound);
  if (id != kNoSound) {
    CSfxManager::AddEmitter(id, at, CVector3f::Zero(), true, false, CSfxManager::kMedPriority);
  }
}

void StartLoop(ESound sound, const CVector3f& at) {
  StopLoop();
  const uint16_t id = Define(sound);
  if (id != kNoSound) {
    s.loop = CSfxManager::AddEmitter(id, at, CVector3f::Zero(), true, true);
  }
}

void UpdateLoop(const CVector3f& at) {
  if (s.loop) {
    CSfxManager::UpdateEmitter(s.loop, at, CVector3f::Zero(), 0x7f);
  }
}

void StopLoop() {
  if (s.loop) {
    CSfxManager::SfxStop(s.loop);
    s.loop = CSfxHandle();
  }
}

}  // namespace PortScrewAudio
