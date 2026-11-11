#pragma once

// Connection to the VR headset through OpenXR, the standard interface that
// the Meta (Oculus) software and SteamVR both provide.

// Both are called once per frame from the game's rendering thread, with the
// game's OpenGL context active.
//
// VrEndFrame: call when the game has finished drawing, before the picture is
//             shown on the monitor. Sends the picture to the headset.
// VrBeginFrame: call right after. Reads where the head is, for the next frame.
//               The first call sets everything up.
void VrEndFrame();
void VrBeginFrame();

// Where the head is, compared with where it was when tracking started
// (or when the view was last re-centered with the Home key).
struct HeadPose
{
    float rotation[9]; // 3x3 grid, row after row: which way the head is turned
    float position[3]; // meters: right (+x), up (+y), backwards (+z)
};

// Fills in the latest head pose. Returns false if the headset is not being
// tracked right now. Safe to call from any thread.
bool VrGetHeadPose(HeadPose& pose);

// Makes the current head position and facing direction the new "straight
// ahead": the eyes darken for a moment, the view is centered while they are
// black, both controllers buzz, and they clear again.
void VrRecenter();

// The game closing: every mouse button and key the mod still holds down in
// Windows is let go (from DllMain; SendInput only).
void VrLetGoAtExit();

// How many times the view has been centered so far (the first time included).
unsigned VrGetRecentreCount();

// The rate (updates a second) the game's own logic should run at: the
// headset's, while the setting is on (half of it while the headset has
// fallen back to half its rate); 0 to leave the game's own.
int VrGetLogicRate();

// The slowest rate the game should keep up with by running more than one
// update to a picture: the rate above itself, so at most one update to a
// picture; 0 to leave the game's own.
int VrGetLogicSlowest();

// A number that goes up by one for every headset frame (and so changes each
// time the eye being drawn changes). Returns 0 while VR is not active.
unsigned long long VrGetFrameSerial();

// Swaps which eye gets which picture. A testing aid; no key calls it.
void VrToggleSwapEyes();

// Makes the game world look smaller (factor above 1) or bigger (factor below 1)
// by changing how big you are compared with it. The result is saved.
void VrAdjustWorldScale(float factor);

// While a controller is being squeezed to grab, fills in where that hand
// points from, relative to "straight ahead" (the same way as the head pose).
// Returns false if no hand is grabbing or it is not being tracked.
bool VrGetGrabHandPose(HeadPose& pose, int& handIndex);

// Fills in where the hand used for pointing is: the grabbing hand while one is
// squeezing, otherwise the right hand, otherwise the left. Returns false if no
// hand is being tracked.
bool VrGetPointingHandPose(HeadPose& pose, int& handIndex);

// Fills in where the middle of the head is (between the eyes), measured the
// same way as the head pose. Unlike VrGetHeadPose it does not hop from one eye
// to the other each frame. Returns false if the headset is not being tracked.
bool VrGetHeadCentrePose(HeadPose& pose);

// Makes turning with the right thumbstick faster (factor above 1) or slower
// (factor below 1). The result is saved.
void VrAdjustTurnSpeed(float factor);

// Which eye the current frame is drawn for: 0 = left, 1 = right, -1 = VR is
// not active. The eyes take turns, left first.
int VrGetFrameEye();

// Fills in where a palm is (hand 0 = left, 1 = right), measured the same way
// as the head pose. Returns false if that controller is not being tracked.
bool VrGetGripHandPose(int hand, HeadPose& pose);

// Whether the game's own arms should follow the controllers (the F4 key
// switches this). When off, plain blocks are drawn for the hands instead.
bool VrGameHandsEnabled();
void VrToggleGameHands();

// How many times bigger than modeled the game's arms should be drawn: always 1.
// Resizing the arm bones on their own turns the arm meshes into ribbons, so the
// arms are only ever scaled with the body.
float VrGetArmScale();

// The game's body is normally drawn at the player's size (the world scale)
// instead of the character's own. The F3 key switches that off and on.
void VrToggleBigBody();
float VrGetBodyScale();

// How far a hand's fingers should be closed (hand 0 = left, 1 = right):
// 0 = open, 1 = a full grip. Follows how hard that controller is squeezed.
float VrGetHandCurl(int hand);

// Asks for one set of measurements about the hands and arms to be written to
// the log (done together with each F9 capture). VrTakeArmNotesRequest returns
// true once after each request.
void VrRequestArmNotes();
bool VrTakeArmNotesRequest();

// Writes to the log how long the mod's own work took per frame since the last call.
void VrLogTimings();

// Which hands are holding a loose object by themselves, without the game's
// interact button: 1 = left, 2 = right, 3 = both. The body script moves those
// objects with the hands.
int VrGetScriptHoldMask();

// About the gun: +1 while the ammo check is held open, +2 while the left hand
// is steadying the gun.
int VrGetGunHandFlags();

// Opens or closes the mod's own menu (Y on the left controller, or F2).
void VrToggleMenu();

// A snap turn that the game's body script should now do: radians, positive =
// to the left, 0 = none. Asking takes it away, so each turn is done once.
float VrTakeSnapTurn();

// Was a snap turn's step in the turn last taken (VrTakeSnapTurn), rather than
// only a smooth turn's small steps, made by the body script while a hand holds
// something? (Told to the script with the turn, roll 7015.)
bool VrLastTurnWasSnap();

// Walking: the left stick's push, turned to the game body's own directions
// (to its right, the way it faces; each -1 to 1, both 0 while the stick is at
// rest). Returns false when the player script should not use it (the "walking
// speed follows the stick" setting is off).
bool VrGetStickMove(float& sideways, float& forwards);

// Crouching by ducking the real head: 0 = switched off, 1 = the head is up,
// 2 = the head is ducked. `maxLift` is the most the body script may raise the
// view again while such a crouch lasts, in game meters.
int VrGetDuck(float& maxLift);

// Both eyes every frame: has the left eye's picture just been taken, with the
// right eye's still to be drawn for the same headset frame? (The window is
// not updated in between.)
bool VrMidPair();

// Has a level just come up after its loading (the world shown again after half
// a second or more of black)? Asking takes it away (frame.cpp, the timings).
bool VrTakeWorldBack();

// Switches "both eyes in step" on or off (the End key).
void VrToggleBothEyes();

// Is "both eyes in step" switched on? (The game's scripts ask, because the
// engine's "limit FPS" setting has to be off for it.)
bool VrBothEyesInStep();

// Is the right hand holding the flashlight's cord right now?
bool VrCordHeld();

// Was the last press in one of the game's menus made with a grip button (a
// drag) rather than a trigger (a click)?
bool VrMenuPressWasGrip();

// Something the game's body script should do now: 1 = draw the gun, 2 = put
// it away, 3 = open the revolver, 0 = nothing. Asking takes it away.
int VrTakeScriptCommand();

// A jump to a place in the game (testlist.h) has arrived: what it wants made
// and pressed goes to the body script, and the jump is over.
void VrJumpArrived();

// The VR page in the game's own options menu: which choice setting `index`
// is on, and choosing another. (The list of settings is above VrGetMenuOption.)
int VrGetMenuOption(int index);
void VrSetMenuOption(int index, int choice);

// A script asks for a controller to buzz (a door's handle clicking open, a
// locked handle rattling). `hand`: 0 = left, 1 = right, 2 = the hand that
// squeezed last (the one holding). `strength` 0 to 1, before the vibration
// setting is applied.
void VrScriptBuzz(int hand, float seconds, float strength);

// Stereo in one game frame (scene.cpp draws the world twice per frame).
// VrStereoCanStart: can the headset frame now in progress be drawn that way?
// VrStereoMidFrame: call after the world has been drawn for the left eye.
//   Takes that picture and moves the headset frame on to the right eye.
//   Returns false if the picture could not be taken (the frame should then be
//   finished as a normal one).
bool VrStereoCanStart();
bool VrStereoMidFrame();

// Which way the game's camera is turned now (radians; haveYaw false if not
// known): told by the camera hook on every use of the player's camera and by
// the scene hook as the world's drawing starts. A turn made since the last
// call is followed about the head at once, for this very frame (see
// PublishHeadPose in vr.cpp).
void VrFollowTurn(float gameYaw, bool haveYaw);

// Is the player walking with the left stick (or its keys) just now?
bool VrWalkingWithStick();

// Did the body script take a snap turn asked for with the right stick within
// the last moment? (Only then is the eye held through a jump of the turn.)
bool VrSnapTurnJustTaken();

// From now on the head's pose, as the camera hook gets it, is moved by
// `shift` (game meters, in the game camera's own directions; sideways only):
// the middle of the play area is moved the opposite way. Used to hold the
// eye still through a snap turn (camera.cpp, HoldEyeThroughSnap).
void VrShiftCentre(const float shift[3]);

// The body script is about to turn the game's camera by `turned` (radians, as
// the camera's own turn counts it), and the view is to stay as it is: the
// character turned under the player while walking (camera.cpp, roll 7095).
// "Straight ahead" is turned with it, by as much.
void VrAbsorbTurn(float turned);

// Pulling things to the hand (see g_pullState in vr.cpp). From
// the body script (roll 7096): which hands point at a thing they may pull.
// And how a hand (0 left, 1 right) stands, for the body script (roll 7097):
// -2 its palm is not known, -1 pulling is off (Settings::pullMode), 0 nothing,
// 1 aimed (its grip held on the thing), 2 pulled (it flicked; for a moment).
void VrPullTargetsNews(bool left, bool right);
int VrPullState(int hand);

// Posing an empty hand (0 = left, 1 = right) from the controller's finger
// sensors. Fills in how far the thumb, the first finger and the other three
// fingers are closed (0 to 1). Returns 1 = pose the hand with these, 2 = the
// hand holds one of the game's things (leave the fingers as animated), 0 =
// hand poses are switched off.
int VrGetFingers(int hand, float curls[3]);

// Healing by hand: 0 = the bandage is still in its tin, 1 = the roll is out
// and being wound round the left forearm, 2 = done. `turns` receives how many
// times it has been wound round so far.
int VrGetBandage(float& turns);

// The game's own window (a HWND), as last seen drawing; nullptr before
// that. For the hand poses tool (posetool.cpp): F11, and which screen it
// opens on.
void* VrGameWindow();

// Has the headset been taken off (head and hands held where they were)?
bool VrHeadsetIsOff();

// "Freeze my hands" in the hand poses tool: the game's hands
// stay where they are in the room, whatever the controllers do (put down on
// the desk), until switched off. Any thread.
bool VrHandsFrozen();
void VrSetHandsFrozen(bool frozen);

// "Hand in front of me" in the hand poses tool (for posing a hand with the
// headset off, when the controllers can't be used to place it): the hand
// (0 left, 1 right) floats in the air in front of the eyes, as the head is
// when this is called (40 cm ahead, a little low, to its own side), whatever
// its controller does; the other hand is unaffected. facing: 0 held out (pointing
// ahead, as an item is held out), 1 the palm towards the eyes, 2 the back of
// the hand towards them. Called again, it is put in front again; on false,
// the hand is back with its controller. Any thread.
void VrSetHandInFront(bool on, int hand, int facing);

// The comfort ring: how far a dark ring closes in from the edges
// of the view, 0 (none) to 1 (clear only straight ahead), for the knockdowns'
// vignette and gentle view (the body script's VRVignetteView and VRGentleView,
// by cVR_ComfortRing). It is drawn while it keeps being set: not set for 0.25 s,
// it is gone.
void VrSetComfortRing(float closed);

// The game's interface in pieces during play: the body script reports every
// update whether the player is in play; while it keeps reporting so (stale
// after 0.25 s) and the game is played (no menu,
// inventory, map, pause, loading or cutscene screen), the subtitles and the
// hint card are shown as pieces instead of the big panel.
void VrSetInterfaceInPieces(bool inPlay);

// How far the view is clear in front during play: the body script reports
// every update how far along the look (and a little below and to each
// side of it) something stands that the pieces must not be seen beyond, in
// the game's own units; -1 for nothing within its reach (cVR_InterfaceRoom).
// The pieces then come nearer, as big to the eye. Stale after 0.25 s.
void VrSetInterfaceRoom(float distance);

// A point of the interface's picture (fractions across and down) to be put in front while the inventory is open, as
// the scripts report it each frame (an opened photo; cVR_PanelFocus). Stale after 0.25 s.
void VrSetPanelFocus(float across, float down);

// Is the left hand waiting for the game's interact button, with the gun or an
// item ready in the right hand? The scripts then leave that
// state at once, not after its animations (cVR_OtherHandWaits).
bool VrOtherHandWaits();

// How far a hand's grip button is squeezed (0 left, 1 right), 0 to 1, as it
// is, also while the hand holds an item (whose fingers keep the game's pose;
// the fuse is twisted while the grip is held; cVR_HandSqueeze).
float VrGetGripSqueeze(int hand);

// How far a hand's trigger is pulled (0 left, 1 right), 0 to 1, as it is (the pocket watch is wound and started by
// the right trigger; cVR_HandTrigger).
float VrGetTrigger(int hand);

// The body script reports, every update while the right hand's item is fitted
// into what it is used on (cVR_RightFitted): a right squeeze begun meanwhile
// is the fit's own, not the game's interact button. Stale after 0.3 s.
void VrRightFitted();

// The shotgun's script reports, every update while the shotgun is held
// (cVR_LeftAtPump): is the left palm at its fore-end? A left squeeze there
// takes the pump. Stale after 0.3 s.
void VrLeftAtPump(bool at);

// The body script reports, every update while a knockdown, a kill or a death
// plays (cVR_NoTurning): the sticks turn nothing. Stale after 0.3 s.
void VrNoTurning(bool none);

// The body script asks, every update while it wants it (cVR_HoldHeadTurn):
// the world's pictures are drawn as from a head that does not
// turn (level, facing as the real head did when first asked; its place still
// followed), shown in front of the eyes wherever they turn. False lets go at once;
// unasked for 1 s it lapses. Returns whether this frame's pictures have it.
bool VrHoldHeadTurn(bool hold);

// The game reports that the player is hurt (cVR_Hurt from Player_Custom.hps OnDamageTaken): how hard
// (the damage, 0 to 1 and more), and where from: kind 1 a direction from the camera at that moment (x right, y up,
// z ahead), 2 from below (a fall), 0 all round. The view's edge reddened on that side, an impact in the hands.
void VrHurt(float amount, int kind, float x, float y, float z);

// The main menu's room (see UpdateMenuRoom in vr.cpp): the menu script
// reports every frame, while the main menu is up, how far the game's
// camera stands above the room's floor, in the game's units (below 0: not
// known; cVR_MenuRoom). Stale after 0.25 s.
void VrSetMenuRoom(float cameraAboveFloor);

// Is the start-up page up? The room behind it is blurred by the scripts
// (cVR_RoomBlurred) and darkened by the mod.
bool VrStartupShown();

// Is the studio's card holding the game back (waiting for the headset to be
// put on, or shown)? The menu script holds the game's own start, its logos,
// until it is not (cVR_StudioCardHolds).
bool VrStudioCardHolds();

// The menu script reports every frame while the main menu is all in view: its
// fade from black over, its title shown (cVR_MenuInView). The start-up page
// comes a moment after.
void VrMenuInView();

// A button of the game's menus has just come into focus (cVR_MenuFocus): a
// light buzz in the hand that points, and the cursor swells.
void VrMenuFocus();

// The storage box's book at the box: while the box is open, the sketchbook
// script reports every frame where the book is to stand
// (cVR_StorageBoxAt): `angle` radians round from the way the head faces
// (plus: to the left), `distance` level and `height` up from the head, in the
// game's own units. While it keeps reporting so (stale after 0.25 s) and the
// inventory is open, the panel stands there instead of in front.
void VrSetStorageBoxAt(float angle, float distance, float height);
