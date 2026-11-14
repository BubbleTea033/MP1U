// The Screw Attack's particle effects from Echoes: serving the baked resources and running the
// effects. See port_screw_fx.h.
#include "port_screw_fx.h"

#include "port_custom_res.h"
#include "port_log.h"
#include "port_paths.h"
#include "port_utf8_path.h"

#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/Particles/CElementGen.hpp"
#include "Kyoto/Particles/CGenDescription.hpp"
#include "Kyoto/SObjectTag.hpp"
#include "Kyoto/TToken.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <vector>

namespace PortScrewFx {
namespace {

constexpr uint32_t kPART = 0x50415254;

struct State {
  bool loaded = false;
  std::string status = "not loaded";
  std::map<uint32_t, Resource> resources;
  uint32_t effects[kEffectCount] = {};
  // Tokens held for the run once used, so a spin does not wait on a load.
  std::unique_ptr<TToken<CGenDescription>> tokens[kEffectCount];
  std::unique_ptr<CElementGen> spin[kSpin3 + 1];
  std::vector<std::unique_ptr<CElementGen>> flashes;
  bool spinning = false;
};
// Never destroyed: the tokens and generators must not outlive the pool at exit.
State& s = *new State;

bool Provide(uint32_t id, PortCustomRes::Resource& out) {
  const auto found = s.resources.find(id);
  if (found == s.resources.end()) {
    return false;
  }
  out.type = found->second.type;
  out.data = found->second.data;
  return true;
}

TToken<CGenDescription>* Token(EEffect effect) {
  if (s.effects[effect] == 0 || gpSimplePool == nullptr) {
    return nullptr;
  }
  if (!s.tokens[effect]) {
    s.tokens[effect].reset(new TToken<CGenDescription>(gpSimplePool->GetObj(SObjectTag(kPART, s.effects[effect]))));
    s.tokens[effect]->Lock();
  }
  return s.tokens[effect].get();
}

std::unique_ptr<CElementGen> Make(EEffect effect) {
  TToken<CGenDescription>* token = Token(effect);
  if (token == nullptr || !token->IsLoaded()) {
    return nullptr;
  }
  return std::unique_ptr<CElementGen>(
      new CElementGen(*token, CElementGen::kMOT_Normal, CElementGen::kOSF_One));
}

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
  for (Resource& r : file.resources) {
    const uint32_t id = r.id;
    s.resources[id] = std::move(r);
  }
  size_t effects = 0;
  for (int i = 0; i < kEffectCount; ++i) {
    s.effects[i] = file.effects[i];
    effects += file.effects[i] != 0;
  }
  PortCustomRes::SetProvider(Provide);
  char line[96];
  std::snprintf(line, sizeof(line), "%zu effects from Echoes (%zu resources)", effects, s.resources.size());
  s.status = line;
  PortLog::Write("screw effects: %s\n", line);
}

const std::string& Status() { return s.status; }

void Prepare() {
  Load();
  for (int i = 0; i < kEffectCount; ++i) {
    Token(EEffect(i));
  }
}

void StartSpin() {
  Prepare();
  s.spinning = true;
  for (int i = kSpin1; i <= kSpin3; ++i) {
    s.spin[i] = Make(EEffect(i));
  }
}

void StopSpin() {
  s.spinning = false;
  for (auto& gen : s.spin) {
    if (gen) {
      gen->SetParticleEmission(false);
    }
  }
}

void JumpFlash() {
  Load();
  if (std::unique_ptr<CElementGen> gen = Make(kJumpFlash)) {
    s.flashes.push_back(std::move(gen));
  }
}

void Update(float dt, const CTransform4f& bone, const CTransform4f& body, const CVector3f& scale) {
  // An effect still loading when the spin began starts once it is in.
  for (int i = kSpin1; i <= kSpin3 && s.spinning; ++i) {
    if (!s.spin[i]) {
      s.spin[i] = Make(EEffect(i));
    }
  }
  const auto step = [&](std::unique_ptr<CElementGen>& gen, const CTransform4f& xf) {
    if (!gen) {
      return;
    }
    if (gen->IsSystemDeletable()) {
      gen.reset();
      return;
    }
    gen->SetGlobalOrientation(xf.GetRotation());
    gen->SetGlobalTranslation(xf.GetTranslation());
    gen->SetGlobalScale(scale);
    gen->Update(dt);
  };
  for (auto& gen : s.spin) {
    step(gen, bone);
  }
  for (auto& gen : s.flashes) {
    step(gen, body);
  }
  s.flashes.erase(std::remove(s.flashes.begin(), s.flashes.end(), nullptr), s.flashes.end());
}

void Render() {
  for (const auto& gen : s.spin) {
    if (gen) {
      gen->Render();
    }
  }
  for (const auto& gen : s.flashes) {
    gen->Render();
  }
}

void Clear() {
  s.spinning = false;
  for (auto& gen : s.spin) {
    gen.reset();
  }
  s.flashes.clear();
}

}  // namespace PortScrewFx
