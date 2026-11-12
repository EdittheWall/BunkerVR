#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "settings.h"

static Settings g_settings;

Settings& GetSettings()
{
    return g_settings;
}

static void WorkOutWorldScale()
{
    g_settings.worldScale = g_settings.worldScaleCalibrated * (1.0f + 0.05f * (float)g_settings.sizeStep);
}

void SetWorldScaleCalibrated(float scale)
{
    g_settings.worldScaleCalibrated = scale;
    WorkOutWorldScale();
}

void SetSizeStep(int step)
{
    g_settings.sizeStep = step < -2 ? -2 : (step > 2 ? 2 : step);
    WorkOutWorldScale();
}

static bool SettingsPath(wchar_t* path)
{
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    while (length > 0 && path[length - 1] != L'\\') --length;
    path[length] = L'\0';
    if (length + 32 >= MAX_PATH) return false;
    lstrcatW(path, L"BunkerVR_settings.txt");
    return true;
}

// Finds "name=number" in the text and stores the number if it is sensible.
static void ReadNumber(const char* text, const char* name, float lowest, float highest, float& target);

// The same for an on/off setting, stored as 1 or 0.
static void ReadSwitch(const char* text, const char* name, bool& target)
{
    float value = target ? 1.0f : 0.0f;
    ReadNumber(text, name, 0.0f, 1.0f, value);
    target = value > 0.5f;
}

// "name=words": the text to the end of its line (used for the floor's headset name).
static void ReadText(const char* text, const char* name, char* target, size_t size)
{
    const char* line = strstr(text, name);
    while (line && line != text && line[-1] != '\n') line = strstr(line + 1, name);
    if (!line || size == 0) return;
    line += strlen(name);
    size_t length = strcspn(line, "\r\n");
    if (length >= size) length = size - 1;
    memcpy(target, line, length);
    target[length] = '\0';
}

static void ReadNumber(const char* text, const char* name, float lowest, float highest, float& target)
{
    // The name counts only at the start of a line: "seated=" is also the
    // end of "duck_seated=".
    const char* line = strstr(text, name);
    while (line && line != text && line[-1] != '\n') line = strstr(line + 1, name);
    if (!line) return;
    float value = 0;
    if (sscanf(line + strlen(name), "%f", &value) == 1 && value >= lowest && value <= highest)
        target = value;
}

// The settings file is there but could not be read at the start: nothing is
// saved over it for the rest of the run. Only a missing file means "no
// settings yet"; any other failure (another program having it open, for
// example) must not lead to the defaults being saved over the player's file.
static bool g_settingsUnreadable = false;

void LoadSettings()
{
    wchar_t path[MAX_PATH];
    if (!SettingsPath(path)) return;

    // (Others may read or write it meanwhile: a backup, a virus scan, a copy.
    // Tried again for a second before giving up.)
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD error = 0;
    for (int attempt = 0; attempt < 20 && file == INVALID_HANDLE_VALUE; ++attempt)
    {
        file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return; // no settings file yet: keep the defaults
        Sleep(50);
    }
    if (file == INVALID_HANDLE_VALUE)
    {
        g_settingsUnreadable = true;
        g_settings.graphicsRecommended = true; // (it could not be remembered: never done at all)
        Log("SETTINGS: BunkerVR_settings.txt is there but could not be opened (error %lu): this run uses the defaults and "
            "saves nothing over the file.", error);
        return;
    }

    char text[4096] = "";
    DWORD read = 0;
    LARGE_INTEGER size = {};
    bool sized = GetFileSizeEx(file, &size) != FALSE;
    bool readOk = ReadFile(file, text, sizeof(text) - 1, &read, nullptr) != FALSE;
    CloseHandle(file);
    if (read >= sizeof(text)) read = sizeof(text) - 1;
    text[read] = '\0';
    if (!readOk || (sized && size.QuadPart > 0 && read == 0))
    {
        g_settingsUnreadable = true;
        g_settings.graphicsRecommended = true; // (as above)
        Log("SETTINGS: BunkerVR_settings.txt could not be read: this run uses the defaults and saves nothing over the file.");
        return;
    }

    // (A file from before the player's adjustment has world_scale alone: that is the calibrated scale, unadjusted.)
    ReadNumber(text, "world_scale=", 0.25f, 4.0f, g_settings.worldScale);
    g_settings.worldScaleCalibrated = g_settings.worldScale;
    ReadNumber(text, "world_scale_calibrated=", 0.25f, 4.0f, g_settings.worldScaleCalibrated);
    float sizeStep = (float)g_settings.sizeStep;
    ReadNumber(text, "size_step=", -2.0f, 2.0f, sizeStep);
    SetSizeStep((int)floorf(sizeStep + 0.5f));
    ReadNumber(text, "hud_distance=", 0.3f, 10.0f, g_settings.hudDistance);
    ReadNumber(text, "hud_width=", 0.2f, 10.0f, g_settings.hudWidth);
    ReadNumber(text, "turn_speed=", 0.0f, 20000.0f, g_settings.turnSpeed);
    ReadNumber(text, "turn_rad_per_count=", 0.0f, 0.01f, g_settings.turnRadPerCount);
    ReadSwitch(text, "game_hands=", g_settings.gameHands);

    ReadSwitch(text, "seated=", g_settings.seated);
    ReadNumber(text, "height_offset=", -1.0f, 1.0f, g_settings.heightOffset);
    float turnMode = (float)g_settings.turnMode;
    ReadNumber(text, "turn_mode=", 0.0f, 1.0f, turnMode);
    g_settings.turnMode = turnMode > 0.5f ? 1 : 0;
    ReadNumber(text, "snap_angle=", 5.0f, 180.0f, g_settings.snapAngle);
    ReadSwitch(text, "big_body=", g_settings.bigBody);
    ReadNumber(text, "holster_right=", -1.0f, 1.0f, g_settings.holsterRight);
    ReadNumber(text, "holster_down=", -0.5f, 1.5f, g_settings.holsterDown);
    ReadNumber(text, "holster_forward=", -1.0f, 1.0f, g_settings.holsterForward);
    ReadNumber(text, "holster_radius=", 0.05f, 1.0f, g_settings.holsterRadius);
    ReadNumber(text, "hand_reach=", 0.05f, 3.0f, g_settings.handReach);
    ReadNumber(text, "support_left=", -0.3f, 0.3f, g_settings.supportLeft);
    ReadNumber(text, "support_down=", -0.3f, 0.3f, g_settings.supportDown);
    ReadNumber(text, "support_forward=", -0.3f, 0.3f, g_settings.supportForward);
    ReadNumber(text, "support_curl=", 0.0f, 1.0f, g_settings.supportCurl);
    ReadNumber(text, "throw_boost=", 0.2f, 5.0f, g_settings.throwBoost);
    ReadNumber(text, "elbow_move=", 0.0f, 2.0f, g_settings.elbowMove);
    float cutsceneMode = (float)g_settings.cutsceneMode;
    ReadNumber(text, "cutscene_mode=", 0.0f, 1.0f, cutsceneMode);
    g_settings.cutsceneMode = cutsceneMode > 0.5f ? 1 : 0;
    ReadSwitch(text, "face_effects=", g_settings.faceEffects);
    ReadSwitch(text, "pc_input=", g_settings.pcInput);
    ReadSwitch(text, "door_handle_turn=", g_settings.doorTurn);
    ReadSwitch(text, "logic_at_headset_rate=", g_settings.logicAtHeadsetRate);
    ReadSwitch(text, "logic_half_rate=", g_settings.logicHalfRate);
    ReadSwitch(text, "right_eye_no_wait=", g_settings.rightEyeNoWait);
    ReadSwitch(text, "right_eye_shadows=", g_settings.rightEyeShadows);
    ReadSwitch(text, "right_eye_earlier_queries=", g_settings.rightEyeEarlierQueries);
    ReadSwitch(text, "eye_position_drawing=", g_settings.eyePositionForDrawing);
    ReadNumber(text, "monitor_smooth=", 0.0f, 1.0f, g_settings.monitorSmooth);
    ReadNumber(text, "monitor_width=", 40.0f, 86.0f, g_settings.monitorWidth);
    ReadSwitch(text, "both_eyes=", g_settings.bothEyes);
    float moveDir = (float)g_settings.moveDir;
    ReadNumber(text, "move_dir=", 0.0f, 1.0f, moveDir);
    g_settings.moveDir = moveDir > 0.5f ? 1 : 0;
    ReadSwitch(text, "analog_walk=", g_settings.analogWalk);
    ReadSwitch(text, "duck_crouch=", g_settings.duckCrouch);
    ReadNumber(text, "duck_seated=", 0.03f, 0.6f, g_settings.duckSeated);
    ReadNumber(text, "duck_standing=", 0.05f, 1.0f, g_settings.duckStanding);
    ReadSwitch(text, "pair_frames=", g_settings.pairFrames);
    ReadSwitch(text, "stereo_frame=", g_settings.stereoFrame);
    ReadNumber(text, "buzz_scale=", 0.0f, 2.0f, g_settings.buzzScale);
    // (heal_mode= and bandage_turns= are not read: the arm is always wrapped three times.)
    ReadNumber(text, "bandage_radius=", 0.02f, 0.10f, g_settings.bandageRadius);
    ReadNumber(text, "bandage_shift_a=", -0.08f, 0.08f, g_settings.bandageShiftA);
    ReadNumber(text, "bandage_shift_b=", -0.08f, 0.08f, g_settings.bandageShiftB);
    ReadNumber(text, "revolver_open_start=", 0.0f, 2.0f, g_settings.revolverOpenStart);
    ReadNumber(text, "revolver_close_start=", 0.0f, 2.0f, g_settings.revolverCloseStart);
    ReadNumber(text, "gun_turn=", -45.0f, 45.0f, g_settings.gunTurn);
    ReadNumber(text, "gun_tip=", -45.0f, 45.0f, g_settings.gunTip);
    ReadNumber(text, "gun_lean=", -90.0f, 90.0f, g_settings.gunLean);
    ReadNumber(text, "pointer_smooth=", 0.0f, 0.95f, g_settings.pointerSmooth);
    ReadSwitch(text, "feel_weight=", g_settings.feelWeight);
    ReadNumber(text, "feel_hand_stiffness=", 200.0f, 10000.0f, g_settings.feelHandStiffness);
    ReadNumber(text, "feel_wrist_stiffness=", 5.0f, 400.0f, g_settings.feelWristStiffness);
    ReadNumber(text, "feel_move_quickest=", 10.0f, 60.0f, g_settings.feelMoveQuickest);
    ReadNumber(text, "feel_turn_quickest=", 10.0f, 60.0f, g_settings.feelTurnQuickest);
    ReadNumber(text, "feel_weight_shown=", 0.0f, 2.0f, g_settings.feelWeightShown);
    ReadNumber(text, "feel_wrist_strength=", 1.0f, 60.0f, g_settings.feelWristStrength);
    ReadNumber(text, "feel_steady_below=", 0.0f, 4.0f, g_settings.feelSteadyBelow);
    ReadSwitch(text, "feel_hand_on_thing=", g_settings.feelHandOnThing);
    ReadSwitch(text, "feel_even_turn=", g_settings.feelEvenTurn);
    ReadNumber(text, "feel_throw_speed=", 0.5f, 4.0f, g_settings.feelThrowSpeed);
    float pullMode = (float)g_settings.pullMode;
    ReadNumber(text, "pull_mode=", 0.0f, 2.0f, pullMode);
    g_settings.pullMode = (int)(pullMode + 0.5f);
    // (The view of knockdowns and kills is one of three, "knockdown_view": 0 no comfort, 1 the cutscene screen,
    // 2 the vignette. An older file has "rough_view" instead: 0 through the eyes, 1 the cutscene screen, 2 faded
    // to black, 3 gentle, taken as 0, 1, 1 (the calmest of the three) and 2.)
    float roughView = -1.0f;
    ReadNumber(text, "knockdown_view=", 0.0f, 2.0f, roughView);
    if (roughView < 0.0f)
    {
        float oldView = (float)g_settings.roughView;
        ReadNumber(text, "rough_view=", 0.0f, 3.0f, oldView);
        int old = (int)(oldView + 0.5f);
        roughView = old == 3 ? 2.0f : (old == 2 ? 1.0f : (float)old);
    }
    g_settings.roughView = (int)(roughView + 0.5f);
    float vignetteStrength = (float)g_settings.vignetteStrength;
    ReadNumber(text, "vignette_strength=", 0.0f, 3.0f, vignetteStrength);
    g_settings.vignetteStrength = (int)(vignetteStrength + 0.5f);
    // (The movement vignette. A file without its line belongs to an existing player: off, so their game doesn't
    // change; a new player, with no file, gets the default, weak.)
    if (strstr(text, "move_vignette=") == nullptr) g_settings.moveVignette = 0;
    else
    {
        float moveVignette = (float)g_settings.moveVignette;
        ReadNumber(text, "move_vignette=", 0.0f, 3.0f, moveVignette);
        g_settings.moveVignette = (int)(moveVignette + 0.5f);
    }
    ReadNumber(text, "pull_reach=", 0.5f, 6.0f, g_settings.pullReach);
    // (The default or off; a 2, the game's full motion, is taken as the default.)
    float viewSway = (float)g_settings.viewSway;
    ReadNumber(text, "view_sway=", 0.0f, 2.0f, viewSway);
    g_settings.viewSway = (int)(viewSway + 0.5f) == 1 ? 1 : 0;
    // (cutscene_view: 1, the cutscenes through the eyes, is not a choice; the line is kept in the
    // file, always 0, the cinema screen. See Settings::cutsceneView.)
    g_settings.cutsceneView = 0;
    ReadNumber(text, "eye_height=", 0.0f, 2.5f, g_settings.eyeHeight);
    ReadNumber(text, "seated_eyes=", 0.0f, 2.5f, g_settings.seatedEyes); // (the body measurements: see Settings::seatedEyes)
    ReadNumber(text, "arm_length=", 0.0f, 1.5f, g_settings.armLength);
    ReadSwitch(text, "pouch_set=", g_settings.pouchSet);
    ReadNumber(text, "pouch_right=", -1.0f, 1.0f, g_settings.pouchRight);
    ReadNumber(text, "pouch_down=", -0.5f, 1.5f, g_settings.pouchDown);
    ReadNumber(text, "pouch_forward=", -1.0f, 1.0f, g_settings.pouchForward);
    ReadNumber(text, "light_left=", -1.0f, 1.0f, g_settings.lightLeft);
    ReadNumber(text, "light_down=", -0.5f, 1.5f, g_settings.lightDown);
    ReadNumber(text, "light_forward=", -1.0f, 1.0f, g_settings.lightForward);
    ReadNumber(text, "slot_right=", -1.0f, 1.0f, g_settings.slotRight); // (the right chest's item slot)
    ReadNumber(text, "slot_down=", -0.5f, 1.5f, g_settings.slotDown);
    ReadNumber(text, "slot_forward=", -1.0f, 1.0f, g_settings.slotForward);
    ReadSwitch(text, "floor_saved=", g_settings.floorSaved); // (the start-up page)
    ReadNumber(text, "floor_from_play_area=", -1.5f, 1.5f, g_settings.floorFromPlayArea);
    ReadNumber(text, "body_height=", 0.0f, 2.5f, g_settings.bodyHeight);
    ReadSwitch(text, "startup_done=", g_settings.startupDone);
    ReadSwitch(text, "floor_guessed=", g_settings.floorGuessed);
    ReadText(text, "floor_headset=", g_settings.floorHeadset, sizeof(g_settings.floorHeadset));
    ReadText(text, "floor_other_headset=", g_settings.floorOtherHeadset, sizeof(g_settings.floorOtherHeadset));
    ReadSwitch(text, "floor_other_saved=", g_settings.floorOtherSaved);
    ReadNumber(text, "floor_other_from_play_area=", -1.5f, 1.5f, g_settings.floorOtherFromPlayArea);
    ReadSwitch(text, "floor_other_guessed=", g_settings.floorOtherGuessed);
    float seatedEyesGuessed = -1.0f;
    ReadNumber(text, "seated_eyes_guessed=", 0.0f, 1.0f, seatedEyesGuessed);
    g_settings.seatedEyesGuessed = seatedEyesGuessed < 0.0f ? g_settings.floorGuessed : seatedEyesGuessed > 0.5f;
    float interactionHints = (float)g_settings.interactionHints;
    ReadNumber(text, "interaction_hints=", 0.0f, 2.0f, interactionHints);
    g_settings.interactionHints = (int)(interactionHints + 0.5f);
    float hintSize = (float)g_settings.hintSize;
    ReadNumber(text, "hint_size=", 0.0f, 3.0f, hintSize);
    g_settings.hintSize = (int)(hintSize + 0.5f);
    // (A file from a version before the recommended graphics has no such line: the player's graphics
    // are their own already and are left as they are. Only a first start, with no file yet, gets them.)
    g_settings.graphicsRecommended = true;
    ReadSwitch(text, "graphics_recommended=", g_settings.graphicsRecommended);

    Log("Settings loaded: world scale %.3f (calibrated %.3f, adjusted %+d%%), interface panel %.2f m wide at %.2f m, turn speed %.0f",
        g_settings.worldScale, g_settings.worldScaleCalibrated, g_settings.sizeStep * 5, g_settings.hudWidth, g_settings.hudDistance,
        g_settings.turnSpeed);
}

bool SetDrawingSwitchForRun(const char* words)
{
    // (Only these, by name: a blanket reading of one line would set others too, as some settings
    // are worked out from others missing, e.g. seated_eyes_guessed.)
    struct DrawingSwitch
    {
        const char* name;
        bool* target;
    };
    const DrawingSwitch switches[] = {
        { "right_eye_shadows=", &g_settings.rightEyeShadows },
        { "right_eye_no_wait=", &g_settings.rightEyeNoWait },
        { "right_eye_earlier_queries=", &g_settings.rightEyeEarlierQueries },
    };
    if (words == nullptr) return false;
    for (const DrawingSwitch& entry : switches)
    {
        size_t length = strlen(entry.name);
        if (strncmp(words, entry.name, length) != 0) continue;
        const char* value = words + length;
        if ((value[0] != '0' && value[0] != '1') || (value[1] != '\0' && value[1] != ' ')) return false;
        *entry.target = value[0] == '1';
        return true;
    }
    return false;
}

void SaveSettings()
{
    if (g_settingsUnreadable)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            Log("SETTINGS: not saved: the settings file could not be read at the start, and is left as it was.");
        }
        return;
    }
    wchar_t path[MAX_PATH];
    if (!SettingsPath(path)) return;

    // Written whole into a file beside it, then put in its place, so a save
    // cut short never leaves the settings half written.
    wchar_t fresh[MAX_PATH + 8];
    lstrcpynW(fresh, path, MAX_PATH);
    lstrcatW(fresh, L".new");
    HANDLE file = CreateFileW(fresh, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    const Settings& s = g_settings;
    char text[4096];
    int length = snprintf(text, sizeof(text),
                          "world_scale=%.3f\r\nworld_scale_calibrated=%.3f\r\nsize_step=%d\r\nhud_distance=%.3f\r\nhud_width=%.3f\r\nturn_speed=%.0f\r\ngame_hands=%d\r\n"
                          "seated=%d\r\nheight_offset=%.3f\r\nturn_mode=%d\r\nsnap_angle=%.0f\r\nbig_body=%d\r\n"
                          "holster_right=%.3f\r\nholster_down=%.3f\r\nholster_forward=%.3f\r\nholster_radius=%.3f\r\n"
                          "hand_reach=%.3f\r\nsupport_left=%.3f\r\nsupport_down=%.3f\r\nsupport_forward=%.3f\r\nsupport_curl=%.3f\r\n"
                          "throw_boost=%.2f\r\ncutscene_mode=%d\r\nface_effects=%d\r\nboth_eyes=%d\r\nelbow_move=%.2f\r\n"
                          "move_dir=%d\r\nanalog_walk=%d\r\nduck_crouch=%d\r\nduck_seated=%.3f\r\nduck_standing=%.3f\r\npair_frames=%d\r\n"
                          "stereo_frame=%d\r\nrevolver_open_start=%.2f\r\nrevolver_close_start=%.2f\r\n"
                          "gun_turn=%.1f\r\ngun_tip=%.1f\r\ngun_lean=%.1f\r\npointer_smooth=%.2f\r\n"
                          "buzz_scale=%.2f\r\nbandage_radius=%.4f\r\nbandage_shift_a=%.4f\r\nbandage_shift_b=%.4f\r\npc_input=%d\r\ndoor_handle_turn=%d\r\n"
                          "logic_at_headset_rate=%d\r\nlogic_half_rate=%d\r\nright_eye_no_wait=%d\r\nright_eye_shadows=%d\r\nright_eye_earlier_queries=%d\r\n"
                          "monitor_smooth=%.2f\r\nmonitor_width=%.0f\r\neye_position_drawing=%d\r\nturn_rad_per_count=%.8f\r\n"
                          "feel_weight=%d\r\nfeel_hand_stiffness=%.1f\r\nfeel_wrist_stiffness=%.2f\r\nfeel_move_quickest=%.1f\r\nfeel_turn_quickest=%.1f\r\n"
                          "feel_weight_shown=%.2f\r\nfeel_wrist_strength=%.1f\r\nfeel_steady_below=%.2f\r\nfeel_hand_on_thing=%d\r\nfeel_even_turn=%d\r\nfeel_throw_speed=%.2f\r\npull_mode=%d\r\nknockdown_view=%d\r\nvignette_strength=%d\r\n"
                          "pull_reach=%.2f\r\nview_sway=%d\r\ncutscene_view=%d\r\neye_height=%.3f\r\n"
                          "seated_eyes=%.3f\r\narm_length=%.3f\r\npouch_set=%d\r\npouch_right=%.3f\r\npouch_down=%.3f\r\npouch_forward=%.3f\r\n"
                          "light_left=%.3f\r\nlight_down=%.3f\r\nlight_forward=%.3f\r\n"
                          "floor_saved=%d\r\nfloor_from_play_area=%.3f\r\nbody_height=%.3f\r\nstartup_done=%d\r\nfloor_guessed=%d\r\n"
                          "interaction_hints=%d\r\n"
                          "floor_headset=%s\r\nfloor_other_headset=%s\r\nfloor_other_saved=%d\r\nfloor_other_from_play_area=%.3f\r\nfloor_other_guessed=%d\r\n"
                          "seated_eyes_guessed=%d\r\n"
                          "slot_right=%.3f\r\nslot_down=%.3f\r\nslot_forward=%.3f\r\n"
                          "move_vignette=%d\r\nhint_size=%d\r\ngraphics_recommended=%d\r\n",
                          s.worldScale, s.worldScaleCalibrated, s.sizeStep, s.hudDistance, s.hudWidth, s.turnSpeed, s.gameHands ? 1 : 0,
                          s.seated ? 1 : 0, s.heightOffset, s.turnMode, s.snapAngle, s.bigBody ? 1 : 0,
                          s.holsterRight, s.holsterDown, s.holsterForward, s.holsterRadius,
                          s.handReach, s.supportLeft, s.supportDown, s.supportForward, s.supportCurl,
                          s.throwBoost, s.cutsceneMode, s.faceEffects ? 1 : 0, s.bothEyes ? 1 : 0, s.elbowMove,
                          s.moveDir, s.analogWalk ? 1 : 0, s.duckCrouch ? 1 : 0, s.duckSeated, s.duckStanding, s.pairFrames ? 1 : 0,
                          s.stereoFrame ? 1 : 0, s.revolverOpenStart, s.revolverCloseStart,
                          s.gunTurn, s.gunTip, s.gunLean, s.pointerSmooth,
                          s.buzzScale, s.bandageRadius, s.bandageShiftA, s.bandageShiftB, s.pcInput ? 1 : 0, s.doorTurn ? 1 : 0,
                          s.logicAtHeadsetRate ? 1 : 0, s.logicHalfRate ? 1 : 0, s.rightEyeNoWait ? 1 : 0, s.rightEyeShadows ? 1 : 0, s.rightEyeEarlierQueries ? 1 : 0, s.monitorSmooth, s.monitorWidth, s.eyePositionForDrawing ? 1 : 0,
                          s.turnRadPerCount,
                          s.feelWeight ? 1 : 0, s.feelHandStiffness, s.feelWristStiffness, s.feelMoveQuickest, s.feelTurnQuickest,
                          s.feelWeightShown, s.feelWristStrength, s.feelSteadyBelow, s.feelHandOnThing ? 1 : 0, s.feelEvenTurn ? 1 : 0, s.feelThrowSpeed, s.pullMode, s.roughView, s.vignetteStrength,
                          s.pullReach, s.viewSway, s.cutsceneView, s.eyeHeight,
                          s.seatedEyes, s.armLength, s.pouchSet ? 1 : 0, s.pouchRight, s.pouchDown, s.pouchForward,
                          s.lightLeft, s.lightDown, s.lightForward,
                          s.floorSaved ? 1 : 0, s.floorFromPlayArea, s.bodyHeight, s.startupDone ? 1 : 0, s.floorGuessed ? 1 : 0,
                          s.interactionHints,
                          s.floorHeadset, s.floorOtherHeadset, s.floorOtherSaved ? 1 : 0, s.floorOtherFromPlayArea, s.floorOtherGuessed ? 1 : 0,
                          s.seatedEyesGuessed ? 1 : 0,
                          s.slotRight, s.slotDown, s.slotForward,
                          s.moveVignette, s.hintSize, s.graphicsRecommended ? 1 : 0);
    if (length >= (int)sizeof(text)) length = (int)sizeof(text) - 1;
    DWORD written = 0;
    bool wrote = length > 0 && WriteFile(file, text, (DWORD)length, &written, nullptr) && written == (DWORD)length;
    CloseHandle(file);
    bool moved = false;
    DWORD error = 0;
    for (int attempt = 0; wrote && attempt < 10 && !moved; ++attempt)
    {
        moved = MoveFileExW(fresh, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        if (!moved)
        {
            error = GetLastError();
            Sleep(20);
        }
    }
    if (!moved)
    {
        DeleteFileW(fresh);
        static int logsLeft = 5;
        if (logsLeft > 0)
        {
            --logsLeft;
            Log("SETTINGS: could not be saved this time (error %lu); the file is as it was.", wrote ? error : 0ul);
        }
    }
}

unsigned char* ReadGameFile(const char* relativePath, int& size)
{
    size = 0;
    char path[MAX_PATH * 2];
    DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    while (length > 0 && path[length - 1] != '\\') --length;
    path[length] = '\0';
    if (length + strlen(relativePath) + 1 >= sizeof(path)) return nullptr;
    strcat(path, relativePath);

    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    DWORD fileSize = GetFileSize(file, nullptr);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0 || fileSize > 64 * 1024 * 1024)
    {
        CloseHandle(file);
        return nullptr;
    }
    unsigned char* data = new unsigned char[fileSize + 1];
    DWORD got = 0;
    bool ok = ReadFile(file, data, fileSize, &got, nullptr) && got == fileSize;
    CloseHandle(file);
    if (!ok)
    {
        delete[] data;
        return nullptr;
    }
    data[fileSize] = 0; // so that a text file can be read as a string
    size = (int)fileSize;
    return data;
}
