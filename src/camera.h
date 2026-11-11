#pragma once

// Hooks the game's camera so the headset's movement is added to it.
// Returns true if the hook was installed.
bool InstallCameraHook();

// How wide the game's camera currently sees, as "tangent of half the angle"
// across and up/down. Returns false until the game has used its camera.
bool CameraGetFieldOfView(float& tanHalfWidth, float& tanHalfHeight);

// Makes the game's camera use this field of view instead of its own:
// `verticalRadians` is the full up-and-down angle, `aspect` is width / height.
// Pass 0, 0 to stop overriding.
void CameraSetFieldOfViewOverride(float verticalRadians, float aspect);

// The hand poses tool's camera on a hand (posetool.cpp). While the headset
// is off, the PC's screen shows the hand (0 left, 1 right) from `direction`
// (in the palm's own directions: x right, y up, z backwards; from the palm
// towards the camera), `distance` meters away, with this field of view
// (degrees, up and down; the window's own shape). `on` false: the view as
// usual. Safe from any thread.
void CameraSetHandView(bool on, int hand, const float direction[3], float distance, float fieldOfViewDegrees);

// Writes the details of the most recently built camera view to the log.
void CameraLogLastView(const char* label);

// The game's scripts ask this mod questions (where is the head, where are the
// hands) many times a second while the game is being played, and stop while it
// is paused or in a menu.
//   CameraScriptsBusy:    did any script ask within the last `milliseconds`?
//   CameraScriptsSeen:    has any script ever asked since the game started?
//   CameraHandScriptBusy: did the script that moves the game's arms ask within
//                         the last `milliseconds`?
bool CameraScriptsBusy(unsigned milliseconds);
bool CameraScriptsSeen();
bool CameraHandScriptBusy(unsigned milliseconds);
// Has the game's body script ever sent its news (roll 7020)? It does so only
// once a level is being played, not in the main menu.
bool CameraBodyNewsSeen();

// Writes to the log, per frame on average over `frames` frames: how many
// questions the scripts asked the mod and how long answering them took, and
// the time of the script work they marked (VRTimeMark in the scripts).
void CameraLogScriptTimings(unsigned long long frames);

// A script's timing mark (its code: 7070 + 2 x the piece of work's number at
// its start, 7071 + 2 x it at its end; VRTimeMark in the scripts) and a
// script's note for the log ("SCRIPT NOTE n: a b c"). The scripts send them
// through cVR_TimeMark and cVR_Note (scriptfuncs.cpp); camera rolls work
// too. The game's main thread.
void CameraScriptTimeMark(int code);
void CameraWatchSoundListener(); // logs the sound listener; test runs with a stand-in runtime only (g_standIn, vr.cpp)
void CameraScriptNote(int note, float a, float b, float c);

// How many updates of the player script have finished since the game started
// (one for each update of the game's logic while a level is played; for the
// log: how many a frame had).
unsigned long long CameraPlayerUpdates();

// What the game's body script last reported: is a gun held ready in the right
// hand, and is the player in the game's normal state (not aiming, reloading,
// holding a door and so on)?
bool CameraGunHeld();
// The same, and the gun is the shotgun (its pump and its shells are worked by hand).
bool CameraShotgunHeld();
// Has the body script reported either within the last 400 ms? If not (as
// while the game's window is in the background), the two above say no for
// want of news, not because there is no gun.
bool CameraGunNewsFresh();
bool CameraPlayerStateNormal();

// The headset frame number for which a camera view was last built to draw the
// 3D world (questions from scripts do not count). While the game draws no
// world (loading screens), this falls behind the current frame number.
unsigned long long CameraLastWorldSerial();

// Did a script report within the last moments that the game is paused (a hint
// waiting for a button, the pause menu)? The world is not drawn then.
bool CameraGamePaused();

// Did the body script report within the last moments that the game's
// inventory is open? It does not always pause the game.
bool CameraInventoryOpen();

// Is the gun script showing the game's own bullet model in the left hand?
bool CameraBulletModelShown();

// The game's screen effects (see DrawScreenEffects in vr.cpp). Fills in how
// strongly each should show, 0 to 1; returns true if any shows at all.
enum { kEffectBloodMajor = 0, kEffectBloodMinor, kEffectBloodEven, kEffectRedEdges, kEffectDarkEdges, kEffectGasMask,
       kEffectHurtPulse, kEffectHealGlow, kEffectUnused2, kScreenEffectCount };
bool CameraGetScreenEffects(float strengths[kScreenEffectCount]);

// What the game's body script last reported about the things in the hands.
// The lighter and the bottles have kinds of their own: the lighter is worked
// by hand and not held "ready" as kItemOther things are; a bottle is held
// ready, for the game's uses of it on things, but thrown by hand.
// kItemLighter in the left hand is the game's own lighter, which it puts
// there while a molotov is held ready. The torch is brandished by holding it
// out and waved by a swing, not held "ready" as kItemOther things are.
enum { kItemNone = 0, kItemFlashlight = 1, kItemHealth = 2, kItemGrenade = 3, kItemLighter = 4, kItemBottle = 5, kItemTorch = 6, kItemOther = 9 };
int CameraRightItemKind();
int CameraLeftItemKind();

// The hands-on items' shared moves: which moves the item in the right hand
// takes part in, as the body script last reported them from that item's
// profile (roll 7093; BunkerVR_ItemMoves in the scripts'
// helper_bunkervr_items.hps), as flags:
//   kMoveThrow: the right grip takes hold of it to throw; let go in a swing,
//     it is thrown (script command 7);
//   kMoveLetGoPutsDown: let go with a still hand, it is put down where it is
//     (script command 15); otherwise it stays in the hand.
// 0 for none, or without news. item (if not null) is set to that item's place
// in the game's item list (grips.h), or -1.
enum { kMoveThrow = 1, kMoveLetGoPutsDown = 2 };
int CameraRightItemMoves(int* item);

// What each hand touches, as the game's body script last reported it (roll
// 7083): kTouchNothing, kTouchOwnHold (a loose thing a hand can carry by
// itself, or what the game's grab holds: to help hold it), or
// kTouchGameOnly (a door, a lever, a pick-up, a note, a crate for both hands:
// only the game's own interact button can use it), or kTouchHeldDoor (the
// door the other hand holds with the game's interact button: this hand takes
// hold of it too). hand: 0 left, 1 right.
enum { kTouchNothing = 0, kTouchOwnHold = 1, kTouchGameOnly = 2, kTouchHeldDoor = 3 };
int CameraTouchKind(int hand);

// How far the game has faded its picture to black: 0 (not at all) to 1.
float CameraFadeAlpha();

// Hooks the game's own fade-drawing function, so that CameraFadeAlpha has
// the engine's fade every frame, not only the body script's news. False
// (and logged) if it could not be found.
bool CameraInstallFadeHook();

// Is the game's loading screen up (as its menu script last reported)?
bool CameraLoadingScreen();

// The shape of the game's window (width divided by height), for cutscenes
// shown on the screen.
void CameraSetWindowAspect(float aspect);

// Is a cutscene being shown on a flat screen right now (the game drawing its
// picture as for a monitor)? This is the screen as shown: vr.cpp
// switches it while the view is black (CameraSetTheatreShown), a
// moment after the body script's news asks for it (CameraTheatreWanted).
bool CameraTheatre();
bool CameraTheatreWanted();
void CameraSetTheatreShown(bool shown);

// Makes the engine rebuild this camera's view the next time it is used, even
// though the game has not moved the camera. Used when the world is drawn a
// second time inside one game frame, for the other eye (scene.cpp).
void CameraForceRebuild(void* camera);

// The revolver's script (Prop_Gun.hps) loads bullets and tips out spent
// shells itself when the hands make the moves, and reports what happened.
//   CameraTakeRevolverEvent: 1 = a bullet went in, 2 = the shells were tipped
//     out, 3 = a bullet was refused, 0 = nothing new. Asking takes it away.
//   CameraRevolverScriptActive: has that script reported within the last moments?
int CameraTakeRevolverEvent();
bool CameraRevolverScriptActive();
//   CameraRevolverHasSpent: are spent shells in the cylinder (as it last reported)?
bool CameraRevolverHasSpent();
//   CameraRevolverCounts: live bullets in the cylinder and spare ones in the
//     inventory, as it last reported. False if it has not reported lately.
bool CameraRevolverCounts(int& loaded, int& spare);
// The shotgun's press-check shows its shells (A held and its pump drawn back;
// the script's news, roll 7028, its first number plus 10).
bool CameraShotgunCheckShown();

// Does the game have the player crouched right now (as its body script last
// reported)? The right stick pushed up then stands up instead of jumping.
bool CameraGameCrouched();

// The player camera's own left-right turn as the game last had it (radians,
// a larger number turns left). False if the camera hook has not seen it lately.
bool CameraGetGameYaw(float& yaw);

// The camera's own left-right turn as it is right now (radians). Read by the
// scene hook as the world's drawing starts, after the game's logic has turned
// the camera for this frame.
bool CameraReadYaw(void* camera, float& yaw);

// While the world is drawn for an eye (scene.cpp, on before, off after), the
// camera holds the eye's own position (the head included) instead of the
// game's, so that the drawing's own decisions (is the camera inside a
// light's reach?) are taken from where the eye is. The game's position is
// put back after, and whenever the view is built again.
void CameraUseEyePosition(void* camera, bool on);

// For the log (watching a snap turn): the last view built with the head in
// it: the game camera's own position, where the eye was in the world, the
// game's turn (radians) and the headset frame it was built for.
bool CameraLastViewNumbers(float gamePosition[3], float eyePosition[3], float& gameYaw, unsigned long long& serial);
// Which way the eye looked in the game's world in the last view built
// (degrees about the up axis; for the walking turn's log).
bool CameraLastEyeFacing(float& degrees);

// A place in the game's world, measured as the head and hands are handed to
// the game (from the game's camera, level, before the head is put in: see
// ApplyPose), by the last view built for the world's picture (for the hints'
// labels, which vr.cpp places in the room from there). False if no world
// has been drawn in the last few frames, or on the cutscene screen.
bool CameraWorldToPlayer(const float world[3], float player[3]);
