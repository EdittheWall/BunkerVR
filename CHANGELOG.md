# Changelog

## 2.10 Steady Hands (23 November 2026)

Everything below is new since 2.09. To update, run the 2.10 Setup over your current installation: your saves and
settings are kept.

### Added

- **Medkit, used by hand.** The medkit is a two-stage item: drinking from its bottle restores the first half of its
  healing, wrapping its dressing round the right forearm the second. Either half can be done on its own. See
  [Using the medkit](#using-the-medkit) below.
- **Medkit grips.** The right hand holds the bag by the middle of its bottom edge with its own finger pose (fingers
  round the back, thumb on the front, the opening facing the left hand), so the fingers never close into the bag
  however the controller is squeezed. The left hand has its own grips for the bottle and the dressing roll.
- **Medkit cues.** While Interaction Hints are set to Until Learned or Always, the part to take next pulses with the
  game's own pickup highlight, and the left controller ticks when the hand comes within reach of it.
- **Number locks (lockers).** Point at a roller and pull the trigger to turn it one step.
- **Haptics.** The revolver's shot and dry fire; the game's own rumble events (levers, landings, traps) passed to the
  controllers; a tick at the hip, shoulder and chest slots; a short tap when an action is refused.
- **Comfort for forced camera motion.** For knockdowns, the Beast's kills and deaths: a blink at the impact and at the
  landing, the edges of the view darkened while you are thrown, and darkness while the Beast covers your eyes. With
  Comfort, the game's own crouch and crawl camera moves also happen in a blink.
- **VR session recovery.** If the OpenXR session is lost mid-game (the runtime restarting, a cable coming loose), the
  game pauses and resumes by itself when VR is back.
- **Start-up without a ready headset.** The mod waits for the headset, shows a message if VR cannot start, and
  otherwise lets the game run normally on the monitor.
- **Keyboard-layout-aware input.** The controllers press the keys the game has its actions bound to, resolved through
  the active Windows keyboard layout (AZERTY, QWERTZ, Dvorak and others), and follow any key bindings you have changed.
- **Stick-jump hint.** A one-time hint at the right hand explains jumping with the right stick.
- **Lantern switch with an item held.** The lantern can be switched at the left chest while the hand holds something.

### Changed

- **Comfort is the default** for knockdowns, kills and deaths (Options > VR > Comfort > Knockdown View). No Comfort is
  unchanged, except that the view now fades out as the body hits the floor in the Beast's kills (the kills through a
  hole or on a ladder stay fully visible).
- **Standing play: the view is your real head while crouched.** In the game's crawl (hiding places, crawlspaces) the
  view stays at your real head and only the game's collision body shrinks. Leaving a crawl still crouched keeps you
  crouched. With the stick's crouch the view is lowered and your real head movement adds to it, clamped above the floor.
- **Body under the head.** The torso follows your head's height to within 2 cm, so the body no longer trails behind
  when you stand up.
- **Arm IK.** The elbow no longer snaps straight at full extension, the shoulder rises and comes forward for high or far
  reaches, and wrist roll is spread along the forearm.
- **Leg IK.** The feet ease into a walk, knee flicks are gone, and the legs straighten smoothly when you stand tall.
- **Stairs.** The view's height on stairs and steps is smoothed like a ramp, during falls, jumps and knockdowns too.
  Room-scale walking steps the body up stairs and never off an edge.
- **Body slots.** The hip, shoulder and chest slots follow your body's facing instead of your head's.
- **Right stick.** Jump and crouch need a deliberate vertical push and never fire on the spring-back of a turn. A crouch
  ends a sprint. Clicking the stick no longer takes out the flashlight: take it from your left chest.
- **Throws and put-downs** use the tracked hand velocity, independent of snap or smooth turning.
- **Held objects while turning** follow your rotation and slide along what they touch; one lost from both hands mid-turn
  drops instead of being thrown.
- **Heavy objects.** Crates and barrels pushed with open hands have the same weight as when gripped and dragged.
- **Fuel can.** Thrown fuel lands where it is aimed: at your feet on a downward shove, up to 4 m on a hard throw.
- **Dressings.** Bandages and the medkit's dressing wind on as one continuous dressing that grows under the hand.
- **At full health** bandages and medkits stay closed, with the game's own message.
- **Physics handles.** Doors, drawers, levers and wheels track the hand as closely at low frame rates as at high ones.
- **Frame-rate independence.** Arms, body and legs behave the same at 45 and 90 frames per second; the hands are drawn
  one frame sooner.
- **Accessibility toggles.** The game's toggle options (use item, sprint, interact and the rest) act as holds in VR.
- **Menu pointer.** The dot marks exactly where a press will land, with a firmer tick.
- **Rendering.** Both eyes are drawn through the same path on every runtime. This should resolve one eye looking
  brighter on some setups away from SteamVR (reported with the Meta runtime on AMD graphics and with Virtual Desktop's
  own runtime).
- **PC keys and logging.** Keyboard shortcuts work only while the game window has focus; log output is much reduced.
- **Hints** no longer mention the A button for the gun: draw it from your hip or shoulder.

### Fixed

- Fixed walking with a French (AZERTY) keyboard: pushing the stick forward or left did nothing or leaned you (issues
  #2 and #4).
- Fixed the body trailing below the view when standing up quickly or leaving a hiding place.
- Fixed the view ducking below your real head, and the game standing you up, when crouching under furniture.
- Fixed the right hand's fingers covering the medkit's opening.
- Fixed a hand caught on a door frame or a wall not coming back once the real hand was clear.
- Fixed the hands lagging behind at a sprint at low frame rates.
- Fixed the legs staying bent as if kneeling after a jump to another place or standing on stairs.
- Fixed knockdowns and re-centring behaving differently away from the middle of the play area.
- Fixed looking over your shoulder putting the shotgun away.
- Fixed A jumping as you leave the pause menu.
- Fixed the shotgun's pump and the revolver's support hand not letting go when the hand is pulled away.
- Fixed sideways flings after a snap turn, gentle put-downs being pushed away, and thrown objects passing through walls.
- Fixed carried objects swinging through closed doors.
- Fixed held objects being left behind or flung while turning.
- Fixed the trigger grabbing or using things while a gun is out but lowered, and the shotgun's next shot waiting after
  the pump has been worked.
- Fixed pausing, the headset's menu, Steam's overlay and switching windows dropping or throwing what you hold.
- Fixed a short stutter every few seconds.
- Fixed objects the mod makes for your hands (a dressing on your arm, the medkit's bag, the gas mask in your hand) being
  written into saves; any found in an older save are removed as the level loads.
- Fixed a grip you are already holding clicking a menu button as the pointer moves onto it.
- Fixed the hint labels and the bullet counter staying visible while the view fades.
- Fixed items losing their ready state during a sprint.
- Fixed smaller item issues: the hip and shoulder holsters acting when they could not, a burning flare left behind at a
  level's door, the fuel can forgetting how much it had spilt, a full bottle that could not be opened with the lantern
  out.

### Using the medkit

The medkit heals in two halves: drinking from its bottle, then wrapping its dressing round your right forearm.

1. **Take it out.** Choose the medkit in the inventory (B). It appears in your right hand, held by its bottom edge with
   the opening towards your left hand. At full health it stays shut, and the game tells you why.
2. **Open the flap.** Bring your left hand to the flap's edge by the press stud, hold Grip and lift it about 8 cm. Let
   go once it is most of the way up and it falls open; let go earlier and it falls shut again.
3. **Take the bottle.** Reach into the bag with your left hand and hold Grip. Pull the left Trigger to take out the cork.
4. **Drink.** Bring the bottle to your lips and tip it towards your face, as you would a real bottle, for about a
   second. This restores the first half. Let go of Grip and the bottle goes back into the bag.
5. **Take the dressing.** Grip the dressing roll in the bag with your left hand. The empty bag drops to the floor.
6. **Wrap your forearm.** Circle the roll around your right forearm three times, keeping your left hand close to the arm:
   away from it for more than a second, the wrapping starts again. The third turn restores the second half, and the
   dressing stays on your arm.

- Press **X** to put the medkit away at any point. Nothing is lost: put away after drinking, it keeps its dressing for
  later.
- With **Options > VR > Accessibility > Interaction Hints** set to Until Learned or Always, each step shows a hint and
  the part to take next glows.

## 2.09 Stand-To (12 November 2026)

The first public release. Stand-to was the hour before dawn when every soldier in the trench stood ready on the
fire-step. Everything below is new to anyone who hasn't been part of the private test.

**The player**
- Full arms and tracked hands with finger poses, room-scale movement, real crouching and leaning.
- Player Setup on first start: seated or standing, floor, height and body-slot positions. Recalibrate any time from
  Options > VR.
- Body slots for the revolver, ammunition, shotgun, flashlight and one chest item.
- Ladders climbed hand over hand.
- Real-body stealth: the creatures see you at your real head height, and your real pace decides how loud your steps
  are.
- Hit feedback: the edge of your view reddens on the side you were hit from, with a pulse in that hand.
- The game hears from where your head really is, so your own sounds stay put as you walk around the room.

**Items and mechanics by hand**
- Revolver, shotgun, grenades, molotovs, flares and every throwable.
- Keys, fuses, combination locks, the wrench, the bolt cutters, door and drawer handles, levers, wheels and buttons.
- Nailed planks, torn off with one hand or both.
- The fuel can, the fuel tap, the lighter, the pocket watch, the gas mask, dog tags and the bandage.
- The ending's final sequence, played by hand.

**Comfort and interface**
- Smooth or snap turning, a movement vignette, head bob options, and three ways to see knockdowns and deaths.
- Cutscenes on a cinema screen. The game's menus and messages on a panel in front of you.
- Interaction hints next to whatever your hands can reach, in the game's own lettering.
- A steady spectator view on the monitor for streaming and recording.
- The game's Gamma, Brightness and Contrast work in the headset and on the monitor.
- The game pauses when you take the headset off or open SteamVR's dashboard or the Meta menu, and Quit there closes
  it.
- A first install starts the game's graphics at lighter settings recommended for VR, once. Your own changes are
  never touched, and updates keep them.

**Installer**
- BunkerVR Setup finds the game through Steam, applies the mod to your own copy of the game's files, keeps the
  originals aside and rolls back if anything fails. Uninstalling puts the game back as it was.
- **BunkerVR Logs** on the desktop collects everything needed for a bug report into one zip.
- If a Steam update or a check of the game's files puts the game's own files back, BunkerVR says so when the game
  starts and asks you to run Setup again, instead of starting a broken VR session.

**Still in progress**
- Shotgun ammo checking.
- Fuel barrel polish and sounds.
- The heaviest areas (No Man's Land, the arena) can drop to half the headset's frame rate.
- A few scenes are still the flat game's: putting the lockdown wheel on its spindle, climbing the arena's ledge and
  the No Man's Land story scenes.
- The mod's own texts are English only. There is no left-handed mode yet.
