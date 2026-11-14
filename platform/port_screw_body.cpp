// The Screw Attack's Samus; see port_screw_body.h.
#include "port_screw_body.h"

#include "port_echoes_anim.h"
#include "port_paths.h"
#include "port_utf8_path.h"

#include "Kyoto/Animation/CCharLayoutInfo.hpp"
#include "Kyoto/Animation/CSegIdList.hpp"
#include "Kyoto/Animation/CSegStatementSet.hpp"
#include "Kyoto/Math/CQuaternion.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "MetroidPrime/CAnimData.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

namespace PortScrewBody {
namespace {

namespace EA = PortEchoesAnim;

// A pose: the rotation of each segment a clip poses, and Skeleton_Root's offset.
struct Pose {
  std::array<EA::Quat, 256> rot;
  std::bitset<256> has;
  EA::Vec3 off;
  uint8_t offSeg = 0;
  bool hasOff = false;
};

enum EStage { kInto, kLoop, kOut, kFall, kLand, kWalk };

// Echoes' CPlayerBodyController::SLocomotionState: the forward clips slow to fast, a clip held at
// least kLocoPrimeTime before another is chosen, idle below kLocoIdleSpeed, and where between two
// clips' speeds the faster takes over (UpdateMedium 0.3, UpdateFast 0.6).
const char* const kLoco[3] = {EA::kWalk, EA::kJog, EA::kRun};
constexpr float kLocoPrimeTime = 0.2f;
constexpr float kLocoIdleSpeed = 0.01f;
constexpr float kLocoMediumSwitch = 0.3f;
constexpr float kLocoFastSwitch = 0.6f;

// Cross-fade between clips, and how fast the body moves between the ball's centre and the
// player's position.
constexpr float kBlendTime = 0.1f;
constexpr float kAnchorTime = 0.2f;

struct State {
  bool loaded = false;
  bool valid = false;
  std::string status = "not loaded";
  EA::File file;
  // The attack.
  const CAnimData* target = nullptr;
  bool showing = false;
  EStage stage = kInto;
  const EA::Baked* clip = nullptr;
  bool looping = false;
  float time = 0.f;
  // The pose the last clip left, fading out over blendLeft.
  Pose from;
  float blendLeft = 0.f;
  float anchor = 0.f;
  // The clip's playback rate (a locomotion clip's follows Samus' speed), each locomotion clip's
  // own speed (its root's travel a second, taken out of its offsets at load; 0 when it has none),
  // which one plays and for how long, and the standing pose to return to when she stops (the
  // landing clip she was in).
  float rate = 1.f;
  float locoSpeed[3] = {};
  int loco = -1;
  float primeTime = 0.f;
  const char* stand = nullptr;
};
State s;

float Duration(const EA::Baked& clip);

void Load() {
  if (s.loaded) {
    return;
  }
  s.loaded = true;
  s.valid = false;
  const std::string path = PortPaths::UserFolder() + EA::kFileName;
  std::ifstream in(PortUtf8::FromUtf8(path), std::ios::binary);
  if (!in) {
    s.status = "not imported (import the Echoes suits, or run --import-echoes-screw)";
    return;
  }
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (!EA::Read(data.data(), data.size(), s.file) || s.file.Find(EA::kLoop) == nullptr) {
    s.status = "unreadable; import it again";
    return;
  }
  s.valid = true;
  // The locomotion clips travel: Echoes' body controller moves the actor by their root motion and
  // takes their speed from it (GetAverageAnimVelocity). Here the player moves itself, so the travel
  // comes out (the loop then moves on the spot) and sets the clip's speed.
  for (float& v : s.locoSpeed) v = 0.f;
  for (EA::Baked& clip : s.file.clips) {
    int which = -1;
    for (int i = 0; i < 3; ++i) {
      if (clip.name == kLoco[i]) which = i;
    }
    if (which < 0 || clip.frames < 2 || clip.off.size() < clip.frames) {
      continue;
    }
    const EA::Vec3 first = clip.off.front();
    const EA::Vec3 last = clip.off[clip.frames - 1];
    const float dx = last.x - first.x;
    const float dy = last.y - first.y;
    const float duration = Duration(clip);
    for (uint32_t f = 0; f < clip.frames; ++f) {
      const float t = float(f) / float(clip.frames - 1);
      clip.off[f].x -= dx * t;
      clip.off[f].y -= dy * t;
    }
    s.locoSpeed[which] = duration > 0.f ? std::sqrt(dx * dx + dy * dy) / duration : 0.f;
  }
  char line[96];
  std::snprintf(line, sizeof(line), "%zu clips from Echoes%s", s.file.clips.size(),
                s.file.Find(EA::kFloor) == nullptr ? " (an older import: import again for the landings)" : "");
  s.status = line;
}

float Duration(const EA::Baked& clip) { return clip.interval * float(clip.frames - 1); }

Pose Sample(const EA::Baked& clip, float time, bool loop) {
  Pose pose;
  uint32_t a = 0;
  uint32_t b = 0;
  float f = 0.f;
  EA::FrameAt(clip, time, loop, a, b, f);
  const size_t n = clip.segs.size();
  for (size_t i = 0; i < n; ++i) {
    pose.rot[clip.segs[i]] = EA::Blend(clip.rot[a * n + i], clip.rot[b * n + i], f);
    pose.has[clip.segs[i]] = true;
  }
  const EA::Vec3& p = clip.off[a];
  const EA::Vec3& q = clip.off[b];
  pose.off = {p.x + (q.x - p.x) * f, p.y + (q.y - p.y) * f, p.z + (q.z - p.z) * f};
  pose.offSeg = clip.offsetSeg;
  pose.hasOff = true;
  return pose;
}

// The pose now: the clip's, with the last clip's fading out.
Pose Current() {
  Pose pose = Sample(*s.clip, s.time, s.looping);
  if (s.blendLeft <= 0.f) {
    return pose;
  }
  float w = 1.f - s.blendLeft / kBlendTime; // the new clip's share
  w = w * w * (3.f - 2.f * w);
  for (size_t i = 0; i < pose.rot.size(); ++i) {
    if (pose.has[i] && s.from.has[i]) {
      pose.rot[i] = EA::Blend(s.from.rot[i], pose.rot[i], w);
    } else if (s.from.has[i]) {
      pose.rot[i] = s.from.rot[i];
      pose.has[i] = true;
    }
  }
  if (pose.hasOff && s.from.hasOff && pose.offSeg == s.from.offSeg) {
    pose.off = {s.from.off.x + (pose.off.x - s.from.off.x) * w, s.from.off.y + (pose.off.y - s.from.off.y) * w,
                s.from.off.z + (pose.off.z - s.from.off.z) * w};
  }
  return pose;
}

// The next stage and its clip; a clip the file lacks leaves the last one held on its last frame.
void Enter(EStage stage, const char* name, bool looping, float at = 0.f) {
  s.stage = stage;
  const EA::Baked* clip = s.file.Find(name);
  if (clip == nullptr) {
    if (s.looping) {
      // Hold the pose it is in rather than keep looping.
      s.looping = false;
      s.time = std::fmod(std::max(s.time, 0.f), std::max(Duration(*s.clip), s.clip->interval));
    }
    return;
  }
  s.from = Current();
  s.blendLeft = kBlendTime;
  s.clip = clip;
  s.looping = looping;
  s.time = at < 0.f ? Duration(*clip) : at;
  s.rate = 1.f;
}

} // namespace

bool Available() {
  Load();
  return s.valid;
}

std::string Status() {
  Load();
  return s.status;
}

void Reload() {
  End();
  s.loaded = false;
}

void Begin(CAnimData* playerAnim) {
  if (!Available() || playerAnim == nullptr) {
    return;
  }
  s.target = playerAnim;
  s.showing = true;
  s.blendLeft = 0.f;
  s.anchor = 0.f;
  s.time = 0.f;
  s.rate = 1.f;
  s.loco = -1;
  s.stand = nullptr;
  const EA::Baked* into = s.file.Find(EA::kInto);
  s.stage = into != nullptr ? kInto : kLoop;
  s.clip = into != nullptr ? into : s.file.Find(EA::kLoop);
  s.looping = into == nullptr;
  playerAnim->PortInvalidatePose();
}

void EndAttack(EEnd reason) {
  if (!s.showing || s.stage == kOut || s.stage == kFall || s.stage == kLand) {
    return;
  }
  if (reason == kEndFloor) {
    Enter(kLand, EA::kFloor, false);
    s.stand = EA::kFloor;
  } else {
    Enter(kOut, reason == kEndWallBump ? EA::kWallBump : EA::kOut, false);
  }
}

void Landed(bool hard) {
  if (s.showing && s.stage != kLand) {
    s.stand = hard && s.file.Find(EA::kHardLand) != nullptr ? EA::kHardLand : EA::kLand;
    Enter(kLand, s.stand, false);
  }
}

void Walk(float speed) {
  if (!s.showing || s.file.Find(EA::kWalk) == nullptr || s.locoSpeed[0] <= 0.f) {
    return;
  }
  // Echoes changes clip (and rate) only once the last has played kLocoPrimeTime.
  if (s.stage == kWalk && s.primeTime < kLocoPrimeTime) {
    return;
  }
  // The clips this import has: an older one has the walk alone.
  const float walk = s.locoSpeed[0];
  const bool hasJog = s.file.Find(EA::kJog) != nullptr && s.locoSpeed[1] > walk;
  const bool hasRun = hasJog && s.file.Find(EA::kRun) != nullptr && s.locoSpeed[2] > s.locoSpeed[1];
  int pick = 0;
  float rate = 1.f;
  if (speed < kLocoIdleSpeed) {
    pick = -1;
  } else if (speed < walk || !hasJog) {
    // UpdateSlow: the walk, slowed by the weight from idle (speed 0) to the walk.
    pick = 0;
    rate = std::clamp(speed / walk, 0.f, 1.f);
    if (!hasJog && speed >= walk) {
      rate = speed / walk;
    }
  } else if (speed < s.locoSpeed[1] || !hasRun) {
    // UpdateMedium: walk at its own speed's ratio until 30% of the way to the jog, then the jog.
    const float jog = s.locoSpeed[1];
    const float weight = std::clamp((speed - walk) / (jog - walk), 0.f, 1.f);
    if (weight < kLocoMediumSwitch) {
      pick = 0;
      rate = speed / walk;
    } else {
      pick = 1;
      rate = std::min(speed / jog, 1.f);
    }
  } else {
    // UpdateFast: the jog until 60% of the way to the run, then the run.
    const float jog = s.locoSpeed[1];
    const float run = s.locoSpeed[2];
    const float weight = std::clamp((speed - jog) / (run - jog), 0.f, 1.f);
    if (weight < kLocoFastSwitch) {
      pick = 1;
      rate = speed / jog;
    } else {
      pick = 2;
      rate = std::min(speed / run, 1.f);
    }
  }
  if (pick < 0) {
    if (s.stage == kWalk) {
      // UpdateIdle; Echoes' ready stance is the landing's last frame.
      Enter(kLand, s.stand != nullptr ? s.stand : EA::kLand, false, -1.f);
      s.loco = -1;
    }
    return;
  }
  if (s.stage != kWalk || s.loco != pick) {
    Enter(kWalk, kLoco[pick], true);
    s.loco = pick;
    s.primeTime = 0.f;
  }
  s.rate = rate;
}

void End() {
  if (s.showing && s.target != nullptr) {
    const_cast<CAnimData*>(s.target)->PortInvalidatePose();
  }
  s.showing = false;
  s.target = nullptr;
  s.clip = nullptr;
}

void Tick(float dt) {
  if (!s.showing) {
    return;
  }
  s.time += dt * s.rate;
  s.primeTime += dt;
  s.blendLeft = std::max(0.f, s.blendLeft - dt);
  if (!s.looping && s.time >= Duration(*s.clip)) {
    if (s.stage == kInto) {
      // The curl's last frame is the spin's first: no fade, and the time past its end carries on.
      const float over = s.time - Duration(*s.clip);
      s.stage = kLoop;
      s.clip = s.file.Find(EA::kLoop);
      s.looping = true;
      s.time = over;
    } else if (s.stage == kOut) {
      Enter(kFall, EA::kFall, true);
    }
  }
  const float target = s.stage == kInto || s.stage == kLoop ? 1.f : 0.f;
  const float step = dt / kAnchorTime;
  s.anchor = target > s.anchor ? std::min(target, s.anchor + step) : std::max(target, s.anchor - step);
  const_cast<CAnimData*>(s.target)->PortInvalidatePose();
}

bool Showing() { return s.showing; }

CVector3f Centre() { return CVector3f(s.file.centre.x, s.file.centre.y, s.file.centre.z); }

float BallAnchor() {
  // Eased, so the body does not start or stop moving abruptly.
  return s.anchor * s.anchor * (3.f - 2.f * s.anchor);
}

void Override(const CAnimData& data, const CSegIdList& segs, CSegStatementSet& set,
              const CCharLayoutInfo& layout) {
  if (!s.showing || &data != s.target || s.clip == nullptr) {
    return;
  }
  const Pose pose = Current();
  // Every posed segment back to the skeleton's offset (a clip before may have moved it).
  for (int i = 0; i < int(segs.size()); ++i) {
    const CSegId& seg = segs[i];
    if (seg.val() != 3) {
      set.Set(seg, layout.GetFromParentUnrotated(seg));
    }
  }
  for (size_t i = 0; i < pose.rot.size(); ++i) {
    if (!pose.has[i]) {
      continue;
    }
    const CSegId seg{int(i)};
    if (!layout.ContainsSegment(seg)) {
      continue;
    }
    const EA::Quat& q = pose.rot[i];
    set.Set(seg, CQuaternion(q.w, q.x, q.y, q.z));
  }
  const CSegId root(int(pose.offSeg));
  if (pose.hasOff && layout.ContainsSegment(root)) {
    set.Set(root, CVector3f(pose.off.x, pose.off.y, pose.off.z));
  }
}

} // namespace PortScrewBody
