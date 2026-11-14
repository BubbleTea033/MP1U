#pragma once

// Port-only KEYF: Echoes' keyframe input (CREKeyframeInput, CIEKeyframeInput, CVEKeyframeInput,
// CCEKeyframeInput in the Echoes decomp, Kyoto/Particles). Prime has no KEYF; the Screw Attack's
// effects from Echoes use it (port_screw_fx.h). The keys are read at an input element's value
// rather than the age: key time = max(0, int(rate * (input - start))), wrapped from the loop
// start past the loop end when looping, else held at the last key, and blended with the next key
// by clamp((input - start) - index / rate, 0, 1) as Echoes does. Echoes only evaluates mode 2;
// the others leave the value as it was.
//
// Stream: u32 mode, u32 unused, bool loop, bool unused, u32 loop end (the key count), u32 loop
// start, float start, float rate, u32 key count and the keys, then the input real element.
// Header only, so a test can build it alone.

#include "Kyoto/Graphics/CColor.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/Particles/CParticleGlobals.hpp"
#include "Kyoto/Particles/IElement.hpp"
#include "Kyoto/Streams/CInputStream.hpp"

#include <algorithm>
#include <vector>

namespace PortEchoesKeyframe {

inline void ReadKey(CInputStream& in, float& v) { v = in.ReadFloat(); }
inline void ReadKey(CInputStream& in, int& v) { v = static_cast< int >(in.ReadLong()); }
inline void ReadKey(CInputStream& in, CVector3f& v) { v = CVector3f(in); }
inline void ReadKey(CInputStream& in, CColor& v) { v = CColor(in); }

inline float Blend(float a, float b, float t) { return (1.f - t) * a + t * b; }
inline int Blend(int a, int b, float t) {
  return static_cast< int >((1.f - t) * static_cast< float >(a) + t * static_cast< float >(b));
}
inline CVector3f Blend(const CVector3f& a, const CVector3f& b, float t) { return (1.f - t) * a + t * b; }
inline CColor Blend(const CColor& a, const CColor& b, float t) { return CColor::Lerp(a, b, t); }

// Echoes' GetKeyframeIndex.
inline int Index(int frame, bool loop, int loopStart, int loopEnd) {
  if (loop) {
    if (frame >= loopEnd && loopEnd > loopStart) {
      frame -= loopStart;
      frame = frame % (loopEnd - loopStart);
      frame += loopStart;
    }
  } else {
    frame = std::min(frame, loopEnd - 1);
  }
  return frame;
}

template < class Base, class T >
class CKeyframeInput : public Base {
  int mMode;
  bool mLoop;
  int mLoopEnd;
  int mLoopStart;
  float mStart;
  float mRate;
  std::vector< T > mKeys;
  CRealElement* mInput;

public:
  // `input` is read by the caller after the keys (CParticleDataFactory::GetRealElement).
  explicit CKeyframeInput(CInputStream& in) : mInput(nullptr) {
    mMode = static_cast< int >(in.ReadLong());
    in.ReadLong();
    mLoop = in.ReadBool();
    in.ReadBool();
    mLoopEnd = static_cast< int >(in.ReadLong());
    mLoopStart = static_cast< int >(in.ReadLong());
    mStart = in.ReadFloat();
    mRate = in.ReadFloat();
    const unsigned count = in.ReadLong();
    // Echoes' keyframe counts are small; a wild one means the stream is not what we think.
    mKeys.resize(count <= 0x10000 ? count : 0);
    for (T& key : mKeys) {
      ReadKey(in, key);
    }
    // Echoes' colour variant resets a loop start past the end; the same guard keeps Index sane.
    if (mLoopStart >= mLoopEnd) {
      mLoopStart = 0;
    }
  }
  ~CKeyframeInput() override { delete mInput; }
  void SetInput(CRealElement* input) { mInput = input; }

  bool GetValue(int frame, T& valOut) const override {
    if (mMode != 2 || mKeys.empty() || mInput == nullptr) {
      return false;
    }
    float input = 0.f;
    mInput->GetValue(frame, input);
    const int time = std::max(0, static_cast< int >(mRate * (input - mStart)));
    int idx = Index(time, mLoop, mLoopStart, mLoopEnd);
    idx = std::max(0, std::min(idx, static_cast< int >(mKeys.size()) - 1));
    const bool lerp = idx > 0 && idx < mLoopEnd - 1 && idx + 1 < static_cast< int >(mKeys.size());
    if (lerp && mRate != 0.f) {
      const float t =
          std::max(0.f, std::min((input - mStart) - static_cast< float >(idx) / mRate, 1.f));
      valOut = Blend(mKeys[idx], mKeys[idx + 1], t);
    } else {
      valOut = mKeys[idx];
    }
    return false;
  }
};

// Echoes' ISWT in the kinds Prime lacks (int, vector, colour; Prime has the real one): the
// first element at a particle's birth (frame 0), the second one frame behind after it.
template < class Base, class T >
class CInitialSwitch : public Base {
  Base* mA;
  Base* mB;

public:
  CInitialSwitch(Base* a, Base* b) : mA(a), mB(b) {}
  ~CInitialSwitch() override {
    delete mA;
    delete mB;
  }
  bool GetValue(int frame, T& valOut) const override {
    Base* const e = frame == 0 ? mA : mB;
    if (e != nullptr) {
      e->GetValue(frame == 0 ? 0 : frame - 1, valOut);
    }
    return false;
  }
};

// Echoes' vector PAP1 and PAP2: the access parameters 0-2 and 3-5 as a vector.
template < int First >
class CVEAccessParameters : public CVectorElement {
public:
  bool GetValue(int, CVector3f& valOut) const override {
    const float* p = CParticleGlobals::GetParticleAccessParameters();
    valOut = p != nullptr ? CVector3f(p[First], p[First + 1], p[First + 2]) : CVector3f::Zero();
    return false;
  }
};

}  // namespace PortEchoesKeyframe
