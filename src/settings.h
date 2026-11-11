#pragma once

// Settings that are remembered between runs of the game. They are kept in
// BunkerVR_settings.txt next to the game's exe, which can also be edited by hand.

struct Settings
{
    // How big you are compared with the game's world. 1.0 means one real meter
    // of head movement (and your real eye spacing) equals one meter in the
    // game. Bigger numbers make the world look smaller. ("Calibrate Height"
    // sets it from your eye height: see eyeHeight.)
    float worldScale = 1.0f;

    // The interface panel (HUD and menus) floating in front of your face:
    // how far away it is and how wide it is, in meters.
    float hudDistance = 1.5f;
    float hudWidth = 1.8f;

    // Turning with the right thumbstick is done by moving the game's mouse.
    // This is how many mouse steps per second a fully pushed stick sends.
    float turnSpeed = 3000.0f;

    // How far the game turns for each of those mouse steps (radians; 0 = not
    // measured yet). Measured by the mod from the player's own smooth turns
    // (see "The game's own turning speed" in vr.cpp), so that turning while
    // a hand holds something, which the body script does instead, goes at
    // the same speed.
    float turnRadPerCount = 0.0f;

    // Whether the game's own arms follow the controllers. When off, plain
    // blocks are drawn where the hands are instead. The F4 key switches this.
    bool gameHands = true;

    // There is no separate arm scale: the body, arms and held things are drawn
    // at the world scale alone (VrGetArmScale is always 1), so grip offsets
    // worked out in code are multiplied by the world scale and nothing else.
    // An arm_scale line in an older settings file is ignored.

    // ---- Comfort (changed from the in-headset menu, Y on the left controller)

    // Playing seated or standing. Either way your eyes are put at the
    // character's eye height when the view is re-centered; this tells the mod
    // where your hip is (for the holster).
    bool seated = true;

    // Extra height in real meters: positive makes you taller in the game.
    float heightOffset = 0.0f;

    // Turning with the right stick: 0 = smooth, 1 = in steps (snap).
    int turnMode = 0;
    float snapAngle = 45.0f; // degrees per step

    // Draw the game's body at your size (the world scale) instead of its own.
    bool bigBody = true;

    // ---- Gun and hands

    // The holster: squeeze the right grip at your right hip to draw the gun or
    // put it away. Where the hip is, measured from your head in real meters
    // (to the right, down, forwards), and how big the area is (radius).
    bool holster = true;
    float holsterRight = 0.20f;
    float holsterDown = 0.50f;
    float holsterForward = 0.0f;
    float holsterRadius = 0.22f;

    // How far past the hand the rays reach that find what a held item is used
    // on (a key, the cutters), and what an empty hand takes when its touch is
    // not known (game meters). An empty hand whose touch is known takes only
    // what it touches, so this reach doesn't limit it.
    float handReach = 0.30f;

    // The left hand steadying the gun: where its palm point sits compared with
    // the right hand's (meters to the left, down, forwards) and how far its
    // fingers close (0 to 1).
    float supportLeft = 0.005f;
    float supportDown = 0.100f;
    float supportForward = -0.065f;
    float supportCurl = 0.65f;

    // Thrown things leave the hand this many times faster than the hand moved.
    float throwBoost = 1.3f;

    // How much the elbows move with the hands (0 = a fixed rule, 1 = normal).
    float elbowMove = 1.0f;

    // The gun pushes loose things it touches.
    bool gunPush = true;

    // Cutscenes: 0 = on a large flat screen in a dark room, 1 = through your
    // own eyes (the game moves you; you can still look around). Unused:
    // cutscenes are always on the screen (see cutsceneView, below). Kept so
    // the settings file keeps its cutscene_mode line.
    int cutsceneMode = 0;

    // Pushing the right stick up jumps.
    bool jumpStick = true;

    // Blood, red edges and dark edges drawn across both eyes when hurt, and the
    // gas mask's view when it is on.
    bool faceEffects = true;

    // Whether the PC's own keyboard and mouse work in the game while it is
    // played in the headset. Off: only the controllers (Ctrl+F12 switches it).
    bool pcInput = false;

    // A closed door opens only once its handle has been turned by the hand
    // holding it (twisting the wrist, as on a real handle). Off: taking hold
    // of the handle is enough.
    bool doorTurn = true;

    // The game's own body (what bumps into walls and doors) follows the
    // player's real steps and leaning, where it fits; the view darkens when
    // the head goes into a wall (PlayerBodyHandler.hps, VRFollowHead). Off:
    // it moves only with the stick.
    bool bodyFollow = true;

    // The game's own logic (physics, scripts, AI) runs as many times a second
    // as the headset shows pictures, instead of the monitor's rate the game
    // takes from its display settings (VRMatchLogicRate, PlayerBodyHandler.hps).
    bool logicAtHeadsetRate = true;

    // With the above: while the headset has fallen back to half its rate (a
    // picture every other refresh, 45 a second on a Rift), the game's logic
    // runs at half the rate too, one update to each picture instead of two,
    // which leaves the time to climb back to the full rate (VrGetLogicRate).
    // Settings file only (logic_half_rate).
    bool logicHalfRate = true;

    // Draw both eyes' pictures for every headset frame (the game draws twice
    // per frame, with its clock held still in between). Off: the eyes take
    // turns, one per frame.
    bool bothEyes = false;

    // The monitor (the game's window, for anyone watching or recording): a
    // steady view that follows the head smoothly with the horizon kept level
    // (ComposeMonitorPicture). How slowly it follows, in seconds (0 = off:
    // the left eye's picture as it is, as wide as the game draws it), and how
    // wide the steady view is, in degrees.
    float monitorSmooth = 0.30f;
    float monitorWidth = 70.0f;

    // ---- Movement

    // Which way "forwards" on the left stick goes: 0 = the way your head
    // faces, 1 = the way the left controller points.
    int moveDir = 0;

    // Walking speed and direction follow the stick exactly (any direction,
    // slow for a small push). Off: eight directions at full speed.
    bool analogWalk = true;

    // Lowering your real head crouches, raising it stands up again. How far
    // the head has to come down from where it was when the view was centered,
    // in real meters, playing seated and playing standing.
    bool duckCrouch = true;
    float duckSeated = 0.12f;
    float duckStanding = 0.30f;

    // Experimental: for every headset frame the game draws both eyes' pictures
    // of one moment, one after the other. Meant to be used with the headset
    // software's "Spacewarp" set to 45 frames a second.
    bool pairFrames = false;

    // The world is drawn twice inside one game frame, once for each eye, the
    // way games with VR built in do it (scene.cpp). The game's logic, scripts
    // and flat interface run once per frame. On by default: with the eyes
    // taking turns instead, moving things' depth is wrong, and none of the
    // speed-ups below, which are built on this, apply.
    bool stereoFrame = true;

    // With the above: the right eye keeps its own answers to "is this light or
    // glow hidden?", from its own frame before, instead of waiting for the
    // graphics card to finish the left eye's (scene.cpp). Settings file only
    // (right_eye_no_wait).
    bool rightEyeNoWait = true;

    // With stereo in one frame: the right eye keeps the shadow maps the left
    // eye's drawing has just brought up to date; only lights that have changed
    // since get new ones (scene.cpp, RightEyeShadowsIn). On by default
    // (right_eye_shadows; the menu's Tuning page).
    bool rightEyeShadows = true;

    // With stereo in one frame: the right eye reads the left eye's occlusion
    // queries of the frame before instead of waiting for those it has just
    // sent, still on the graphics card (scene.cpp, RightEyeQueriesIn), which
    // saves about 1.3 ms of processor time a frame. The answers stay paired with
    // the left eye's marks, as the game pairs them: queries of the right eye's
    // own would make its culling cautious in places. On by default. Saved as
    // right_eye_earlier_queries (an old right_eye_own_queries line is ignored);
    // the menu's Tools page.
    bool rightEyeEarlierQueries = true;

    // While the world is drawn, the game's camera holds the eye's own
    // position (the real head included), so that the drawing's decisions
    // about lights are taken from where the eye is (CameraUseEyePosition).
    // Settings file only (eye_position_drawing).
    bool eyePositionForDrawing = true;

    // ---- The revolver

    // Where bullets come from while the revolver is open: true = a pouch on
    // the left hip (squeeze the left hand there to take one), false = any
    // squeeze of the left hand away from the gun.
    bool ammoPouch = true;

    // Where the bullet count is shown: 0 = at the pouch on the left hip (the
    // spare bullets, while the revolver is out and the left hand is near, or
    // the revolver is open), 1 = above the open cylinder (loaded / spare).
    int ammoCounter = 0;

    // ---- Accessibility

    // How strongly the controllers buzz: 0 = not at all, 1 = as designed.
    float buzzScale = 1.0f;

    // Bandages and medkits are always used by wrapping them round the left
    // forearm. A heal_mode line in an older settings file is ignored.

    // How many times the bandage must go round the arm (1 to 3, default 3).
    int bandageTurns = 3;

    // The hints on the objects (hints.h): 0 each kind until it has
    // been used a few times ("Until Learned"), 1 always, 2 never.
    int interactionHints = 0;
    // How big the hints' labels are, as adjustable text size is recommended by the Game Accessibility
    // Guidelines, and like the game's own subtitle Font Size (Small to Very Large): 0 small (0.8 times),
    // 1 normal (the labels' 40 cm picture, letters about 1.3 cm), 2 large (1.25), 3 very large (1.5).
    // Saved as hint_size; the Accessibility page.
    int hintSize = 1;

    // How thick the left forearm is taken to be when the bandage is drawn
    // wound round it: meters from the middle of the arm to where the cloth
    // lies. Too small and the cloth sinks into the sleeve, too big and it
    // floats.
    float bandageRadius = 0.045f;

    // How far the wrap's middle is moved off the line of the arm's bones,
    // two ways across the arm (meters): the sleeve does not hang evenly
    // round that line.
    float bandageShiftA = 0.0f;
    float bandageShiftB = 0.0f;

    // The game's own "open" and "close" animations of the revolver are
    // started this many seconds in, so that the cylinder moves at once
    // instead of after the wind-up. A sound that a later start skips is
    // played at once (PlayerState_UsingGun_Reload.hps). The menu's range stops
    // short of the game's own messages in these animations, at 0.84 and 0.62 s.
    float revolverOpenStart = 0.75f;
    float revolverCloseStart = 0.45f;

    // How the gun sits in the hand, in degrees, on top of pointing its barrel
    // along the controller: barrel turned to the right, barrel tipped up, and
    // the top of the gun leaning to the right (negative = the other way).
    float gunTurn = -2.0f;
    float gunTip = -10.0f;
    float gunLean = -29.0f;

    // ---- Hands and menus

    // Empty hands are posed from the controllers' finger sensors (open hand,
    // fist, pointing, thumbs up) instead of on top of the game's animation.
    bool handPoses = true;

    // How much the menu pointer is steadied against the hand's tremble:
    // 0 = not at all, 0.9 = a great deal. Quick moves always follow at once.
    float pointerSmooth = 0.75f;

    // ---- Feel (the "Feel" page of the in-headset menu): how held things
    // feel, read by the game's scripts on every update (roll 7080 ..
    // 7082, see camera.cpp), so they can be tuned while playing. The values
    // here are the tested defaults; "Restore Tested Defaults" on that page
    // puts them back. (Their meaning, in the scripts' own words:
    // gbBunkerVR_Weight and the rest in helper_bunkervr.hps.)
    bool feelWeight = true;            // weight on held things (the carried place)
    float feelHandStiffness = 1500.0f; // newtons a hand holds a thing at its place with, a meter off it
    float feelWristStiffness = 50.0f;  // newton meters a wrist turns it with, a radian off
    float feelMoveQuickest = 40.0f;    // how quickly a light thing follows, moving (radians a second)
    float feelTurnQuickest = 35.0f;    // ...and turning
    float feelWeightShown = 1.0f;      // how much of a held thing's weight pulls on it, as a force (1: all)
    float feelWristStrength = 10.0f;   // newton meters one wrist can turn a held thing with, at most (a chair held level by its edge needs 12)
    float feelSteadyBelow = 1.2f;      // a hand turning slower than this (radians a second) is steadied (0: never)
    bool feelHandOnThing = true;       // the drawn hand fixed to what it holds
    bool feelEvenTurn = true;          // the stick's smooth turn made even while a hand holds something
    // (Read by the mod itself, not the scripts: the throw shared by the
    // hands-on items, see UpdateGrabInput in vr.cpp. Swinging an item feels
    // better with 2.5 than with 1.5.)
    float feelThrowSpeed = 2.5f;       // the right grip let go of an item to throw with the hand at least this fast (meters a second) throws it

    // Pulling things to the hand, for items the player can't reach: point,
    // hold the grip and flick, and the item flies into the hand. 0 off,
    // 1 only while "Play Mode" is "Seated", 2 always.
    int pullMode = 1;

    // How knockdowns, the Beast's kills and deaths are seen (the body script; saved as knockdown_view):
    // 0 no comfort: through your eyes (the view goes where the animation takes the head, level, facing the
    //   way you faced);
    // 1 the cutscene screen (the game's own view);
    // 2 the vignette: through your eyes, with the view's edges darkened while it moves fast (VRVignetteView
    //   in the body script; the ring is drawn by the mod, VrSetComfortRing).
    // An older file's rough_view (0 through the eyes, 1 the cutscene screen, 2 faded to black, 3 through the
    // eyes, gentle: VRGentleView) is read instead when there is no knockdown_view, and taken as 0, 1, 1
    // and 2.
    int roughView = 0;

    // How strong the knockdown vignette is: 0 none, 1 weak, 2 medium, 3 strong, how far the ring may close
    // (kVignetteStrengths in vr.cpp). VR option 42.
    int vignetteStrength = 2;

    // The movement vignette: the edges of the view darkened while the thumbsticks move or turn you (not while
    // you move yourself), as Meta's locomotion guidelines recommend (its Interaction SDK's tunneling reacts to
    // the locomotor's rotation and speed, sized in degrees). 0 off, 1 weak, 2 medium, 3 strong, the knockdown
    // ring's strengths (kVignetteStrengths in vr.cpp). Weak for a new player, as a comfort-friendly default;
    // a settings file without a move_vignette line keeps it off, so an existing player's game doesn't change
    // (LoadSettings). VR option 43.
    int moveVignette = 1;

    // How far pulling reaches from the hand, in meters: short, so that only
    // close items can be pulled. The body script reads it (VR option 36) and
    // falls back to its own gfVRPullFarthest (6 m) when the mod doesn't answer.
    float pullReach = 1.5f;

    // The drawn body's own sway in the view. The view hangs on the body's
    // eyes, which its walking and standing animations move: 0 reduced
    // (the steps' bob cut to about a quarter, the standing sway taken out),
    // 1 none (the steps' bob taken out as well). The game's full motion is
    // not offered: a 2 in the file is taken as 0. The body script reads it
    // (VR option 37, VRSteadyEyes).
    int viewSway = 0;

    // The game's cutscenes: always 0, on the large flat screen in the dark.
    // The body script also has 1, through your eyes, gentle: level, facing
    // the way the scene puts you, the view's path smoothed, the comfort ring
    // with its speed, an eased return (its VRGentleView, with the body shown).
    // That choice is not offered: the value is not read from the file (its
    // cutscene_view line is still written, as 0) and is in neither menu. The
    // body script reads it (VR option 38). It has its own key so that an old
    // cutscene_mode=1 can't turn it on.
    int cutsceneView = 0;

    // A voice_notes line in an older settings file is ignored.

    // Your real eye height above the floor, standing up straight, in real
    // meters ("Calibrate Height" on the Player page; 0 = not measured).
    // Measuring sets the world scale from it: the soldier's eye height
    // (1.838 m) over yours, which makes your real body his size. And
    // playing standing, the view's height is put at it above the floor at
    // each re-center, so the game's floor is where your real floor is
    // (StandingHeightFromFloor in vr.cpp).
    float eyeHeight = 0.0f;

    // Body measurements, set only by the measuring buttons on the Player and
    // Holsters pages, in real meters.
    //   seatedEyes: your eyes above the floor when seated, from touching the floor;
    //               0 = not measured. The main menu's room stands on it.
    //   armLength:  shoulder to hand, from holding your arms out; 0 = not measured.
    //               Stored but unused: the drawn arms stretch to the controllers.
    //   pouch...:   the bullets' and flashlight's spot on the left hip, measured from
    //               the head as the holster is (pouchSet false: the holster mirrored).
    //   light...:   the flashlight's spot on the chest, from the neck.
    float seatedEyes = 0.0f;
    float armLength = 0.0f;
    bool pouchSet = false;
    float pouchRight = -0.20f;
    float pouchDown = 0.50f;
    float pouchForward = 0.0f;
    float lightLeft = 0.14f;
    float lightDown = 0.225f;
    float lightForward = 0.10f;
    //   slot...:    the right chest's item slot (the flat game's hotkey 1), from the
    //               neck as the flashlight's: to the right, below and in front. Higher
    //               than the flashlight's mirror, so that it stays clear of the
    //               holster's reach at the right hip (from 15 cm above the holster's
    //               spot); set on the Holsters page.
    float slotRight = 0.13f;
    float slotDown = 0.20f;
    float slotForward = 0.09f;

    // The start-up page, which measures your height so that the game uses the
    // same height in every run. In real meters.
    //   floorSaved, floorFromPlayArea: the floor a controller rested on, kept as
    //     its height above the play area's own floor, which stays put in the room
    //     from one run to the next (the play area's floor need not be the real
    //     floor). Not saved: the play area's floor is used as it is.
    //   floorGuessed: that floor was not touched but put below your
    //     seated eyes by how far a person of your height sits above the floor
    //     (seated, with the floor out of reach): a rested-on floor replaces it.
    //   bodyHeight: how tall you are, set on the page instead of standing up; 0 =
    //     measured standing (eyeHeight) or not known.
    //   startupDone: the page has been gone through once; from then on each
    //     start asks only "Same as last time?" for 3 seconds.
    bool floorSaved = false;
    float floorFromPlayArea = 0.0f;
    bool floorGuessed = false;
    float bodyHeight = 0.0f;
    bool startupDone = false;

    // The headset those floor numbers were measured with, as its runtime's name
    // and its own: "Oculus | Oculus Rift CV1". The numbers belong to one headset:
    // one runtime's floor offset added to another runtime's play area can put
    // the player inside the floor. Empty (a file that never stored it) counts as
    // the Oculus runtime. And one other headset's numbers, kept aside for when it
    // is used again (ChooseHeadsetFloor in vr.cpp swaps them).
    char floorHeadset[128] = "";
    char floorOtherHeadset[128] = "";
    bool floorOtherSaved = false;
    float floorOtherFromPlayArea = 0.0f;
    bool floorOtherGuessed = false;
    // seatedEyesGuessed: seatedEyes was not measured above a floor
    // but taken from your height (the start-up page with no floor rested on;
    // the floor was then put below it, floorGuessed). Right only with that
    // headset's floor: on a headset with no floor of its own the view keeps the
    // head's own height when centered (SavedHeightApplies in vr.cpp). Missing from
    // an older file: as floorGuessed (both were set together).
    bool seatedEyesGuessed = false;
};

Settings& GetSettings();
void LoadSettings();
void SaveSettings();

// For benchmark runs (the remote "set" command in vr.cpp): one of the drawing's speed switches, "name=0" or "name=1"
// (right_eye_shadows, right_eye_no_wait, right_eye_earlier_queries), changed for this run and not saved.
// False for any other name or a value that is not 0 or 1.
bool SetDrawingSwitchForRun(const char* words);

// Reads a whole file from the game's folder (the path is given from that
// folder, for example "fonts\special_elite_56.fnt"). Returns its bytes, with
// a zero after the last one, or nullptr; the caller frees them with delete[].
unsigned char* ReadGameFile(const char* relativePath, int& size);
