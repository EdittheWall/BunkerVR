#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "testlist.h"

static const int kMostChecks = 200;
static TestCheck g_checks[kMostChecks];
static int g_checkCount = 0;
static int g_at = 0;
static bool g_loadedOnce = false;
static bool g_askingWhy = false;
static char g_folder[MAX_PATH] = "";

void TestListUseFolder(const char* folder)
{
    snprintf(g_folder, sizeof(g_folder), "%s", folder);
}

static void FilePath(char* out, int outSize, const char* name)
{
    if (g_folder[0] == '\0')
    {
        // The folder of the game's exe.
        char exe[MAX_PATH];
        DWORD length = GetModuleFileNameA(nullptr, exe, MAX_PATH);
        while (length > 0 && exe[length - 1] != '\\') --length;
        exe[length] = '\0';
        snprintf(out, (size_t)outSize, "%sBunkerVR\\%s", exe, name);
    }
    else
    {
        snprintf(out, (size_t)outSize, "%s%s", g_folder, name);
    }
}

// Takes the spaces off both ends, in place.
static char* Trim(char* s)
{
    while (*s == ' ' || *s == '\t') ++s;
    size_t length = strlen(s);
    while (length > 0 && (s[length - 1] == ' ' || s[length - 1] == '\t' || s[length - 1] == '\r' || s[length - 1] == '\n')) s[--length] = '\0';
    return s;
}

// The whole of a file, ended with a zero; null if it cannot be read.
static char* ReadWholeFile(const char* path)
{
    FILE* file = fopen(path, "rb");
    if (!file) return nullptr;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char* text = size >= 0 ? (char*)malloc((size_t)size + 1) : nullptr;
    if (text)
    {
        size_t got = fread(text, 1, (size_t)size, file);
        text[got] = '\0';
    }
    fclose(file);
    return text;
}

// The next line of `text`, ended with a zero in place; `text` moves on past it.
// Null at the end.
static char* NextLine(char*& text)
{
    if (*text == '\0') return nullptr;
    char* line = text;
    char* end = strchr(text, '\n');
    if (end)
    {
        *end = '\0';
        text = end + 1;
    }
    else
    {
        text += strlen(text);
    }
    return line;
}

static void ReadList()
{
    g_checkCount = 0;
    char path[MAX_PATH];
    FilePath(path, sizeof(path), "test_list.txt");
    char* whole = ReadWholeFile(path);
    if (!whole) return;
    char* text = whole;
    while (char* line = NextLine(text))
    {
        line = Trim(line);
        if (*line == '\0' || *line == '#') continue;
        char* firstBar = strchr(line, '|');
        char* secondBar = firstBar ? strchr(firstBar + 1, '|') : nullptr;
        if (!secondBar)
        {
            Log("TEST LIST: a line without its two bars was left out: %.60s", line);
            continue;
        }
        if (g_checkCount >= kMostChecks)
        {
            Log("TEST LIST: only the first %d checks were read.", kMostChecks);
            break;
        }
        char* thirdBar = strchr(secondBar + 1, '|');
        *firstBar = '\0';
        *secondBar = '\0';
        if (thirdBar) *thirdBar = '\0';
        TestCheck& check = g_checks[g_checkCount++];
        snprintf(check.id, sizeof(check.id), "%s", Trim(line));
        snprintf(check.jump, sizeof(check.jump), "%s", Trim(firstBar + 1));
        snprintf(check.text, sizeof(check.text), "%s", Trim(secondBar + 1));
        snprintf(check.reasons, sizeof(check.reasons), "%s", thirdBar ? Trim(thirdBar + 1) : "");
        check.result = kCheckNotDone;
    }
    free(whole);
}

// The answers so far; the last one for a check counts. A line is
// "date time | id | PASS ...".
static void ReadResults()
{
    char path[MAX_PATH];
    FilePath(path, sizeof(path), "test_results.txt");
    char* whole = ReadWholeFile(path);
    if (!whole) return;
    char* text = whole;
    while (char* line = NextLine(text))
    {
        char* firstBar = strchr(line, '|');
        char* secondBar = firstBar ? strchr(firstBar + 1, '|') : nullptr;
        if (!secondBar) continue;
        *secondBar = '\0';
        char* thirdBar = strchr(secondBar + 1, '|');
        if (thirdBar) *thirdBar = '\0';
        char* id = Trim(firstBar + 1);
        char* answer = Trim(secondBar + 1);
        int result = strcmp(answer, "PASS") == 0 ? kCheckPassed : (strcmp(answer, "FAIL") == 0 ? kCheckFailed : (strcmp(answer, "SKIP") == 0 ? kCheckSkipped : kCheckNotDone));
        if (result == kCheckNotDone) continue;
        for (int i = 0; i < g_checkCount; ++i)
        {
            if (strcmp(g_checks[i].id, id) == 0) g_checks[i].result = result;
        }
    }
    free(whole);
}

static void GoToFirstNotDone(int from)
{
    for (int i = 0; i < g_checkCount; ++i)
    {
        int at = (from + i) % g_checkCount;
        if (g_checks[at].result == kCheckNotDone)
        {
            g_at = at;
            return;
        }
    }
    g_at = from < g_checkCount ? from : 0; // all answered: stay
}

void TestListLoad()
{
    char shownId[16] = "";
    if (g_loadedOnce && g_at < g_checkCount) snprintf(shownId, sizeof(shownId), "%s", g_checks[g_at].id);
    ReadList();
    ReadResults();
    if (g_checkCount == 0)
    {
        g_at = 0;
        g_loadedOnce = true;
        return;
    }
    if (shownId[0] != '\0')
    {
        for (int i = 0; i < g_checkCount; ++i)
        {
            if (strcmp(g_checks[i].id, shownId) == 0)
            {
                g_at = i;
                return;
            }
        }
    }
    GoToFirstNotDone(0);
    if (!g_loadedOnce) Log("TEST LIST: %d checks read.", g_checkCount);
    g_loadedOnce = true;
}

int TestListCount()
{
    return g_checkCount;
}

int TestListAt()
{
    return g_at;
}

const TestCheck* TestListCurrent()
{
    return g_at < g_checkCount ? &g_checks[g_at] : nullptr;
}

void TestListMove(int step)
{
    if (g_checkCount == 0) return;
    g_at = (g_at + step + g_checkCount) % g_checkCount;
    g_askingWhy = false;
}

void TestListCounts(int& passed, int& failed, int& skipped, int& notDone)
{
    passed = failed = skipped = notDone = 0;
    for (int i = 0; i < g_checkCount; ++i)
    {
        switch (g_checks[i].result)
        {
        case kCheckPassed: ++passed; break;
        case kCheckFailed: ++failed; break;
        case kCheckSkipped: ++skipped; break;
        default: ++notDone; break;
        }
    }
}

static void AddResultLine(const char* id, const char* what)
{
    char path[MAX_PATH];
    FilePath(path, sizeof(path), "test_results.txt");
    FILE* file = fopen(path, "ab");
    if (!file)
    {
        Log("TEST LIST ERROR: could not add to %s", path);
        return;
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    fprintf(file, "%04d-%02d-%02d %02d:%02d:%02d | %s | %s\n", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, id, what);
    fclose(file);
}

void TestListAnswer(int result, int mark, const char* filesPrefix)
{
    if (g_at >= g_checkCount) return;
    TestCheck& check = g_checks[g_at];
    check.result = result;
    char what[192];
    if (result == kCheckFailed) snprintf(what, sizeof(what), "FAIL | mark %d | %s", mark, filesPrefix);
    else snprintf(what, sizeof(what), "%s", result == kCheckPassed ? "PASS" : "SKIP");
    AddResultLine(check.id, what);
    Log("TEST LIST: check %s: %s", check.id, what);
    g_askingWhy = result == kCheckFailed;
    if (!g_askingWhy) GoToFirstNotDone(g_at);
}

void TestListNoteMark(int mark, const char* filesPrefix)
{
    char what[192];
    snprintf(what, sizeof(what), "MARK | mark %d | %s", mark, filesPrefix);
    AddResultLine(g_at < g_checkCount ? g_checks[g_at].id : "-", what);
}

bool TestListAskingWhy()
{
    return g_askingWhy && g_at < g_checkCount;
}

const char* TestListWhyChoice(int which)
{
    if (which < 0 || which >= kTestListWhyButtons || g_at >= g_checkCount) return "";
    if (which == kTestListWhyButtons - 1) return "I'll say it after";

    static const char* const kGeneral[kTestListWhyButtons - 1] = { "Nothing happened", "Could not find or reach it", "Looks wrong", "Feels wrong", "Too slow" };
    const char* list = g_checks[g_at].reasons;
    if (list[0] == '\0') return kGeneral[which];

    // The check's own reasons: number `which`, between its ";" separators.
    for (int i = 0; i < which && list; ++i)
    {
        list = strchr(list, ';');
        if (list) ++list;
    }
    if (!list) return "";
    while (*list == ' ') ++list;
    static char word[64];
    const char* end = strchr(list, ';');
    size_t length = end ? (size_t)(end - list) : strlen(list);
    while (length > 0 && list[length - 1] == ' ') --length;
    if (length >= sizeof(word)) length = sizeof(word) - 1;
    memcpy(word, list, length);
    word[length] = '\0';
    return word;
}

void TestListGiveWhy(int which)
{
    if (!TestListAskingWhy()) return;
    const char* words = TestListWhyChoice(which);
    if (words[0] == '\0') return;
    char what[96];
    snprintf(what, sizeof(what), "WHY | %s", words);
    AddResultLine(g_checks[g_at].id, what);
    Log("TEST LIST: check %s: %s", g_checks[g_at].id, what);
    g_askingWhy = false;
    GoToFirstNotDone(g_at);
}

// ---------------------------------------------------------------------------

int TestListWrap(const char* text, int width, int (*measure)(const char*), char (*lines)[kTestListLineChars], int maxLines)
{
    int count = 0;
    char line[kTestListLineChars] = "";
    const char* s = text;
    while (*s)
    {
        while (*s == ' ') ++s;
        if (*s == '\0') break;
        const char* wordEnd = s;
        while (*wordEnd && *wordEnd != ' ') ++wordEnd;

        char attempt[kTestListLineChars];
        int lineLength = (int)strlen(line);
        int wordLength = (int)(wordEnd - s);
        bool fits = false;
        if (lineLength + 1 + wordLength < kTestListLineChars)
        {
            memcpy(attempt, line, (size_t)lineLength);
            int at = lineLength;
            if (lineLength > 0) attempt[at++] = ' ';
            memcpy(attempt + at, s, (size_t)wordLength);
            attempt[at + wordLength] = '\0';
            fits = measure(attempt) <= width;
        }
        if (fits)
        {
            memcpy(line, attempt, sizeof(attempt));
            s = wordEnd;
            continue;
        }
        if (lineLength > 0)
        {
            // This line is full: the word starts the next.
            if (count < maxLines) memcpy(lines[count], line, sizeof(line));
            ++count;
            line[0] = '\0';
            continue;
        }
        // One word wider than a whole line: as much of it as fits.
        int take = 0;
        while (take < wordLength && take < kTestListLineChars - 1)
        {
            attempt[take] = s[take];
            attempt[take + 1] = '\0';
            if (measure(attempt) > width) break;
            ++take;
        }
        if (take == 0) take = 1;
        if (count < maxLines)
        {
            memcpy(lines[count], s, (size_t)take);
            lines[count][take] = '\0';
        }
        ++count;
        s += take;
    }
    if (line[0] != '\0')
    {
        if (count < maxLines) memcpy(lines[count], line, sizeof(line));
        ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Where to go
// ---------------------------------------------------------------------------

static TestJumpParts g_jump = {};
static bool g_jumpWanted = false;
static bool g_jumping = false;
static bool g_jumpedThisRun = false;
static bool g_jumpAgain = false; // the jump under way is the last one again, after a death
static unsigned long long g_jumpStartedMs = 0;
static unsigned long long (*g_jumpClock)() = nullptr;
static const unsigned long long kJumpMostMs = 240000; // the scripts give up at 180 s; this is for when they can't

static unsigned long long JumpClock()
{
    return g_jumpClock ? g_jumpClock() : GetTickCount64();
}

void TestJumpUseClock(unsigned long long (*clock)())
{
    g_jumpClock = clock;
}

// Splits "a,b,c" into up to `most` names of `size` characters.
static int SplitNames(const char* list, char* names, int size, int most)
{
    int count = 0;
    while (*list && count < most)
    {
        const char* end = strchr(list, ',');
        size_t length = end ? (size_t)(end - list) : strlen(list);
        if (length > 0 && length < (size_t)size)
        {
            memcpy(names + count * size, list, length);
            names[count * size + length] = '\0';
            ++count;
        }
        if (!end) break;
        list = end + 1;
    }
    return count;
}

bool TestJumpParse(const char* jump, TestJumpParts& parts)
{
    parts = {};
    bool good = true;
    char copy[sizeof(TestCheck::jump)];
    snprintf(copy, sizeof(copy), "%s", jump);
    char* context = nullptr;
    for (char* word = strtok_s(copy, " \t", &context); word; word = strtok_s(nullptr, " \t", &context))
    {
        if (strncmp(word, "map=", 4) == 0) snprintf(parts.map, sizeof(parts.map), "%s", word + 4);
        else if (strncmp(word, "start=", 6) == 0) snprintf(parts.start, sizeof(parts.start), "%s", word + 6);
        else if (strncmp(word, "give=", 5) == 0) parts.giveCount = SplitNames(word + 5, &parts.give[0][0], (int)sizeof(parts.give[0]), 8);
        else if (strncmp(word, "spawn=", 6) == 0) parts.spawnCount = SplitNames(word + 6, &parts.spawn[0][0], (int)sizeof(parts.spawn[0]), 6);
        else if (strncmp(word, "do=", 3) == 0) parts.doCommand = atoi(word + 3);
        else if (strcmp(word, "opening") == 0) parts.keepOpening = true;
        else good = false;
    }
    return good;
}

static bool JumpTo(const char* who, const char* words)
{
    if (TestJumping())
    {
        Log("TEST LIST: a jump is already under way.");
        return false;
    }
    if (!TestJumpParse(words, g_jump)) Log("TEST LIST: %s: a word in where to go was not understood: %s", who, words);
    g_jumpWanted = true;
    g_jumping = true;
    g_jumpAgain = false;
    g_jumpStartedMs = JumpClock();
    g_jumpedThisRun = true;
    Log("TEST LIST: %s: going to: %s", who, words);
    return true;
}

bool TestJumpStart()
{
    const TestCheck* check = TestListCurrent();
    if (!check || check->jump[0] == '\0')
    {
        return false;
    }
    char who[64];
    snprintf(who, sizeof(who), "check %s", check->id);
    return JumpTo(who, check->jump);
}

bool TestJumpStartWords(const char* words)
{
    return JumpTo("the pretend headset", words);
}

bool TestJumpToPlace(const char* name, const char* words)
{
    char who[96];
    snprintf(who, sizeof(who), "the Places page, %s", name);
    return JumpTo(who, words);
}

bool TestJumpAgain()
{
    if (!g_jumpedThisRun) return false;
    g_jumpWanted = false;
    g_jumping = true;
    g_jumpAgain = true;
    g_jumpStartedMs = JumpClock();
    Log("TEST LIST: a death after a jump: %s started again, back at %s (no save loaded).",
        g_jump.map[0] ? g_jump.map : "the same level", g_jump.start[0] ? g_jump.start : "its own start");
    return true;
}

bool TestJumpWanted()
{
    bool wanted = g_jumpWanted;
    g_jumpWanted = false;
    return wanted;
}

const char* TestJumpPart(int part)
{
    if (part == 0) return g_jump.map;
    if (part == 1) return g_jump.start;
    if (part == 2) return g_jumpAgain ? "again" : "";
    if (part >= 10 && part < 10 + g_jump.giveCount) return g_jump.give[part - 10];
    if (part >= 30 && part < 30 + g_jump.spawnCount) return g_jump.spawn[part - 30];
    return "";
}

int TestJumpDoCommand()
{
    return g_jump.doCommand;
}

void TestJumpFinished()
{
    g_jumping = false;
    g_jumpAgain = false;
}

bool TestJumping()
{
    if (g_jumping && JumpClock() - g_jumpStartedMs > kJumpMostMs)
    {
        Log("TEST LIST: the jump has not arrived in 4 minutes: it no longer counts as under way.");
        g_jumping = false;
    }
    return g_jumping;
}

bool TestJumpSkipsOpening()
{
    return TestJumping() && !g_jump.keepOpening;
}

bool TestJumpedThisRun()
{
    return g_jumpedThisRun;
}

void TestJumpsOver()
{
    g_jumpedThisRun = false;
    g_jumpAgain = false;
}
