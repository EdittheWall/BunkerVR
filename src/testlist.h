#pragma once

// The test list: a developer tool that shows a list of things to check in the
// headset, one at a time, on the "Checks" page of the mod's menu, each answered
// Pass, Fail or Skip in the headset. BunkerVR\test_list.txt holds the list
// (read again each time the menu opens) and every answer is appended to
// BunkerVR\test_results.txt, with the time and, for a fail, the mark and the
// files saved with it. Answers are kept: a check answered in one run is still
// answered in the next.
//
// test_list.txt, one check a line (blank lines and lines starting # are skipped):
//     id | where to go (or nothing) | what to do and what to look for | reasons
// "Reasons" (optional): up to 5, separated by ";", shown as buttons after a
// Fail, since nothing can be typed in the headset. Without them, the general
// ones are offered (TestListWhyChoice). "I'll say it after" is always the last.
// "Where to go", any of these, separated by spaces:
//     map=trenches.hpm   the level
//     start=Start_Gate   the start spot in it (its .hpm_Area file names them)
//     give=Gun,Lighter   items to carry (config\Inventory.cfg names them)
//     spawn=wooden_door.ent,...   things made ahead of you when you get there
//     do=44              a button of the Testing or Beast page (its number in vrmenu.h)
//     opening            play the level's own opening (otherwise it is skipped)
//
// No OpenXR or OpenGL in here, so this can be unit-tested on its own.

enum TestCheckResult
{
    kCheckNotDone = 0,
    kCheckPassed = 1,
    kCheckFailed = 2,
    kCheckSkipped = 3,
};

struct TestCheck
{
    char id[16];
    char jump[240];
    char text[480];
    char reasons[200];
    int result;
};

// Where the two files are looked for: the folder of the game's exe, or (for
// the tests) the folder given. It must end in "\".
void TestListUseFolder(const char* folder);

// Reads the list and the answers so far. The check shown stays the same one if
// it is still there; otherwise (and the first time) it is the first one not
// answered.
void TestListLoad();

int TestListCount();
int TestListAt(); // which is shown, from 0
const TestCheck* TestListCurrent(); // null if the list is empty
void TestListMove(int step);
void TestListCounts(int& passed, int& failed, int& skipped, int& notDone);

// Answers the check shown: the line goes into test_results.txt, and the next
// one not answered comes up. A fail also gives its mark and the name its files
// start with; the others pass 0 and "". After a fail the same check stays up,
// asking why, until TestListGiveWhy or a move to another check.
void TestListAnswer(int result, int mark, const char* filesPrefix);

// Why a check failed: TestListAskingWhy says whether it is being asked.
// TestListWhyChoice gives the words of button `which` (0 to
// kTestListWhyButtons - 1) for the check shown, "" for a button with none.
// TestListGiveWhy (a button pressed) adds its line to test_results.txt and
// moves on.
static const int kTestListWhyButtons = 6;
bool TestListAskingWhy();
const char* TestListWhyChoice(int which);
void TestListGiveWhy(int which);

// A mark made with the controllers' sticks: its line in test_results.txt, naming the check shown.
void TestListNoteMark(int mark, const char* filesPrefix);

// Breaks `text` into lines no wider than `width`, as `measure` measures them.
// Returns how many lines it takes; at most `maxLines` are filled in.
static const int kTestListLineChars = 120;
int TestListWrap(const char* text, int width, int (*measure)(const char*), char (*lines)[kTestListLineChars], int maxLines);

// Where to go.
//   TestJumpStart: go where the check shown says. False if it says nothing, or
//                  a jump is under way.
//   TestJumpWanted: is a jump waiting to be started? True once for each.
//   TestJumpPart: its parts: 0 the map, 1 the start spot, 2 "again" for a jump
//                 made again after a death (TestJumpAgain), 10 on the items to
//                 give, 30 on the things to make; "" past the end.
//   TestJumpFinished: the scripts say it is over.
//   TestJumping: from TestJumpStart until then, or 4 minutes at most, so that a
//                jump that goes wrong can't block the next ones.
//   TestJumpedThisRun: has a jump been made? No autosave after one (the player
//                      is not where the game's own story has them): until
//                      TestJumpsOver, when a real game is started or loaded.
//   TestJumpStartWords: the same, to where these words say (for scripted test
//                       runs, which have no list).
//   TestJumpToPlace: the same, from the menu's Places page; `name` is for the log.
//   TestJumpAgain: a death after a jump (the game's own continue doesn't work
//                  then): the last jump is under way again, its level started
//                  afresh by the game-over script (cLux_StartMap) and the player
//                  put back at its start spot on arrival. Nothing is wanted (the
//                  script starts the level itself). False if no jump has been
//                  made this run.
bool TestJumpStart();
bool TestJumpAgain();
bool TestJumpStartWords(const char* words);
bool TestJumpToPlace(const char* name, const char* words);
bool TestJumpWanted();
const char* TestJumpPart(int part);
int TestJumpDoCommand();
void TestJumpFinished();
bool TestJumping();
bool TestJumpSkipsOpening(); // a jump is under way, and it didn't say "opening"
bool TestJumpedThisRun();
void TestJumpsOver();

// For the tests: what the 4 minutes are measured by (milliseconds).
void TestJumpUseClock(unsigned long long (*clock)());

// Reads a check's "where to go" into its parts (for TestJumpStart, and for
// checking the list outside the game).
struct TestJumpParts
{
    char map[64];
    char start[64];
    char give[8][32];
    int giveCount;
    char spawn[6][64];
    int spawnCount;
    int doCommand;
    bool keepOpening; // the word "opening": the level's own opening is played, to check it
};
bool TestJumpParse(const char* jump, TestJumpParts& parts); // false if it has a word it doesn't know
