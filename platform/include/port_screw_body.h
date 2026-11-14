// The Screw Attack's Samus (Screw Attack, CMorphBall): from the attack's wind-up until Samus is
// back in first person, Prime's in-game Samus model (the player's own, BallTransitionsANCS) is
// drawn in place of the ball, posed by Echoes' screw attack clips. The clips come from the
// player's Echoes disc, retargeted onto Prime's skeleton by the Echoes import
// (port_echoes_anim.h, echoes_screw_attack.bin in the user folder); without that file the ball
// shows as before. Game thread only.
//
// This is a small sequencer standing in for the states Echoes' CPlayerBodyController gives the
// attack (the state each stage mirrors in brackets):
//   Into  [MorphToScrewAttack]            the curl into the spin, from the trigger
//   Loop  [ScrewAttack]                   the spin, looped
//   Out   [TransitionOutOfScrewAttack]    the uncurl (plain, or the bounce off a wall), then
//   Fall  [the same state's jump loop]    the fall, looped until the ground
//   Land  [WallSlideLand]                 the landing after a fall (the heavy one after a long
//                                         fall), or [TransitionOutOfScrewAttackFloor] Echoes'
//                                         moving unmorph when the attack itself met the floor;
//                                         held on its last frame
// CMorphBall decides when each starts (EndAttack, Landed) and how long the player stays locked;
// nothing here changes the mechanic, so it plays the same without the clips.
#pragma once

#include <string>

class CAnimData;
class CCharLayoutInfo;
class CSegIdList;
class CSegStatementSet;
class CVector3f;

namespace PortScrewBody {

// Why the attack ended, which picks the exit clip.
enum EEnd {
  kEndOut,      // a mistimed press, the last jump, the drop limit, a ceiling
  kEndWallBump, // a wall: Samus bounces back off it
  kEndFloor,    // the ground: straight to the landing
};

// Whether the clips are loaded (read once, the first time this is asked).
bool Available();
// "6 clips from Echoes", or why there are none.
std::string Status();
// Reads the file again (after an import).
void Reload();

// The wind-up starts for the player's animation data (whose pose this replaces).
void Begin(CAnimData* playerAnim);
// The attack is over: the exit clip, then the fall.
void EndAttack(EEnd reason);
// Samus is on the ground after falling out of the attack: the landing, heavy after a long fall.
void Landed(bool hard);
// During the unmorph after the recovery: Samus' ground speed, in the clips' units (world units
// over the model's scale). Echoes' locomotion picks walk, jog or run by it and plays the clip at
// the rate that matches; stopping, she stands again.
void Walk(float speed);
// Samus is back in first person, or the attack was dropped.
void End();
void Tick(float dt);
// Whether the body stands in for the ball now.
bool Showing();
// The middle of the spinning body in the model's own space (unscaled).
CVector3f Centre();
// How far the body is drawn around the ball's centre (1, the spin) rather than standing on the
// player's position (0, the fall and the landing, whose feet must meet the ground).
float BallAnchor();

// CAnimData::RecalcPoseBuilder, after the animation tree's statements: the clips' rotations in
// their place for the player's animation data while Showing(), each other segment's offset back
// to the skeleton's and Skeleton_Root's from the clip.
void Override(const CAnimData& data, const CSegIdList& segs, CSegStatementSet& set,
              const CCharLayoutInfo& layout);

} // namespace PortScrewBody
