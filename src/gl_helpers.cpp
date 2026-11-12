#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "gl_helpers.h"
#include "log.h"

// --- OpenGL constants newer than the ones in Windows' gl.h -----------------
#define GLX_READ_FRAMEBUFFER 0x8CA8
#define GLX_DRAW_FRAMEBUFFER 0x8CA9
#define GLX_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GLX_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GLX_COLOR_ATTACHMENT0 0x8CE0
#define GLX_FRAMEBUFFER_COMPLETE 0x8CD5
#define GLX_FRAMEBUFFER_SRGB 0x8DB9
#define GLX_ACTIVE_TEXTURE 0x84E0
#define GLX_TEXTURE0 0x84C0
#define GLX_CLAMP_TO_EDGE 0x812F
#define GLX_TEXTURE_MAX_LEVEL 0x813D
#define GLX_SAMPLER_BINDING 0x8919
#define GLX_CURRENT_PROGRAM 0x8B8D
#define GLX_VERTEX_ARRAY_BINDING 0x85B5
#define GLX_ARRAY_BUFFER 0x8892
#define GLX_ARRAY_BUFFER_BINDING 0x8894
#define GLX_PIXEL_PACK_BUFFER 0x88EB
#define GLX_PIXEL_UNPACK_BUFFER 0x88EC
#define GLX_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GLX_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#define GLX_BLEND_DST_RGB 0x80C8
#define GLX_BLEND_SRC_RGB 0x80C9
#define GLX_BLEND_DST_ALPHA 0x80CA
#define GLX_BLEND_SRC_ALPHA 0x80CB
#define GLX_BLEND_EQUATION_RGB 0x8009
#define GLX_BLEND_EQUATION_ALPHA 0x883D
#define GLX_FUNC_ADD 0x8006
#define GLX_FRAGMENT_SHADER 0x8B30
#define GLX_VERTEX_SHADER 0x8B31
#define GLX_COMPILE_STATUS 0x8B81
#define GLX_LINK_STATUS 0x8B82
#define GLX_BGRA 0x80E1

// --- Newer OpenGL functions, looked up at run time --------------------------
typedef void (APIENTRY* GenFramebuffersFn)(GLsizei count, GLuint* ids);
typedef void (APIENTRY* DeleteFramebuffersFn)(GLsizei count, const GLuint* ids);
typedef void (APIENTRY* BindFramebufferFn)(GLenum target, GLuint id);
typedef GLenum (APIENTRY* CheckFramebufferStatusFn)(GLenum target);
typedef void (APIENTRY* FramebufferTexture2DFn)(GLenum target, GLenum attachment, GLenum textureTarget, GLuint texture, GLint level);
typedef void (APIENTRY* BlitFramebufferFn)(GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                                           GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                                           GLbitfield mask, GLenum filter);
typedef void (APIENTRY* ActiveTextureFn)(GLenum unit);
typedef void (APIENTRY* UseProgramFn)(GLuint program);
typedef void (APIENTRY* BindVertexArrayFn)(GLuint array);
typedef void (APIENTRY* GenVertexArraysFn)(GLsizei count, GLuint* arrays);
typedef void (APIENTRY* BindBufferFn)(GLenum target, GLuint buffer);
typedef void (APIENTRY* BindSamplerFn)(GLuint unit, GLuint sampler);
typedef void (APIENTRY* GenSamplersFn)(GLsizei count, GLuint* samplers);
typedef void (APIENTRY* SamplerParameteriFn)(GLuint sampler, GLenum name, GLint value);
typedef void (APIENTRY* BlendFuncSeparateFn)(GLenum srcRgb, GLenum dstRgb, GLenum srcAlpha, GLenum dstAlpha);
typedef void (APIENTRY* BlendEquationSeparateFn)(GLenum modeRgb, GLenum modeAlpha);
typedef GLuint (APIENTRY* CreateShaderFn)(GLenum type);
typedef void (APIENTRY* ShaderSourceFn)(GLuint shader, GLsizei count, const char* const* strings, const GLint* lengths);
typedef void (APIENTRY* CompileShaderFn)(GLuint shader);
typedef void (APIENTRY* GetShaderivFn)(GLuint shader, GLenum name, GLint* value);
typedef void (APIENTRY* GetShaderInfoLogFn)(GLuint shader, GLsizei size, GLsizei* length, char* text);
typedef GLuint (APIENTRY* CreateProgramFn)();
typedef void (APIENTRY* AttachShaderFn)(GLuint program, GLuint shader);
typedef void (APIENTRY* LinkProgramFn)(GLuint program);
typedef void (APIENTRY* GetProgramivFn)(GLuint program, GLenum name, GLint* value);
typedef GLint (APIENTRY* GetUniformLocationFn)(GLuint program, const char* name);
typedef void (APIENTRY* Uniform1iFn)(GLint location, GLint value);
typedef void (APIENTRY* Uniform3fFn)(GLint location, GLfloat x, GLfloat y, GLfloat z);
typedef void (APIENTRY* Uniform1fFn)(GLint location, GLfloat value);
typedef void (APIENTRY* Uniform4fFn)(GLint location, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
typedef void (APIENTRY* Uniform4fvFn)(GLint location, GLsizei count, const GLfloat* values);
typedef void (APIENTRY* CompressedTexImage2DFn)(GLenum target, GLint level, GLenum format, GLsizei width, GLsizei height,
                                                GLint border, GLsizei size, const void* data);
typedef void (APIENTRY* GenerateMipmapFn)(GLenum target);
typedef void (APIENTRY* UniformMatrix4fvFn)(GLint location, GLsizei count, GLboolean transpose, const GLfloat* values);

static GenFramebuffersFn glxGenFramebuffers = nullptr;
static DeleteFramebuffersFn glxDeleteFramebuffers = nullptr;
static BindFramebufferFn glxBindFramebuffer = nullptr;
static CheckFramebufferStatusFn glxCheckFramebufferStatus = nullptr;
static FramebufferTexture2DFn glxFramebufferTexture2D = nullptr;
static BlitFramebufferFn glxBlitFramebuffer = nullptr;
static ActiveTextureFn glxActiveTexture = nullptr;
static UseProgramFn glxUseProgram = nullptr;
static BindVertexArrayFn glxBindVertexArray = nullptr;
static GenVertexArraysFn glxGenVertexArrays = nullptr;
static BindBufferFn glxBindBuffer = nullptr;
static BindSamplerFn glxBindSampler = nullptr;
static BlendFuncSeparateFn glxBlendFuncSeparate = nullptr;
static BlendEquationSeparateFn glxBlendEquationSeparate = nullptr;
static CreateShaderFn glxCreateShader = nullptr;
static ShaderSourceFn glxShaderSource = nullptr;
static CompileShaderFn glxCompileShader = nullptr;
static GetShaderivFn glxGetShaderiv = nullptr;
static GetShaderInfoLogFn glxGetShaderInfoLog = nullptr;
static CreateProgramFn glxCreateProgram = nullptr;
static AttachShaderFn glxAttachShader = nullptr;
static LinkProgramFn glxLinkProgram = nullptr;
static GetProgramivFn glxGetProgramiv = nullptr;
static GetUniformLocationFn glxGetUniformLocation = nullptr;
static Uniform1iFn glxUniform1i = nullptr;
static Uniform3fFn glxUniform3f = nullptr;
static UniformMatrix4fvFn glxUniformMatrix4fv = nullptr;
// Only the screen effects need these three, so the rest works without them.
static Uniform1fFn glxUniform1f = nullptr;
static Uniform4fFn glxUniform4f = nullptr;
static Uniform4fvFn glxUniform4fv = nullptr;
static CompressedTexImage2DFn glxCompressedTexImage2D = nullptr;
// Only the interface's pieces need this (their smaller copies).
static GenerateMipmapFn glxGenerateMipmap = nullptr;
// Only drawing the panels into the eyes' pictures needs these (SteamVR).
static GenSamplersFn glxGenSamplers = nullptr;
static SamplerParameteriFn glxSamplerParameteri = nullptr;

static bool g_triedLoading = false;
static bool g_loaded = false;
static GLuint g_copyFramebuffer = 0;

template <typename T>
static bool Load(T& function, const char* name)
{
    function = (T)wglGetProcAddress(name);
    return function != nullptr;
}

bool GlLoadFunctions()
{
    if (g_triedLoading) return g_loaded;
    if (wglGetCurrentContext() == nullptr) return false; // try again later
    g_triedLoading = true;

    bool ok = true;
    ok &= Load(glxGenFramebuffers, "glGenFramebuffers");
    ok &= Load(glxDeleteFramebuffers, "glDeleteFramebuffers");
    ok &= Load(glxBindFramebuffer, "glBindFramebuffer");
    ok &= Load(glxCheckFramebufferStatus, "glCheckFramebufferStatus");
    ok &= Load(glxFramebufferTexture2D, "glFramebufferTexture2D");
    ok &= Load(glxBlitFramebuffer, "glBlitFramebuffer");
    ok &= Load(glxActiveTexture, "glActiveTexture");
    ok &= Load(glxUseProgram, "glUseProgram");
    ok &= Load(glxBindVertexArray, "glBindVertexArray");
    ok &= Load(glxGenVertexArrays, "glGenVertexArrays");
    ok &= Load(glxBindBuffer, "glBindBuffer");
    ok &= Load(glxBindSampler, "glBindSampler");
    ok &= Load(glxBlendFuncSeparate, "glBlendFuncSeparate");
    ok &= Load(glxBlendEquationSeparate, "glBlendEquationSeparate");
    ok &= Load(glxCreateShader, "glCreateShader");
    ok &= Load(glxShaderSource, "glShaderSource");
    ok &= Load(glxCompileShader, "glCompileShader");
    ok &= Load(glxGetShaderiv, "glGetShaderiv");
    ok &= Load(glxGetShaderInfoLog, "glGetShaderInfoLog");
    ok &= Load(glxCreateProgram, "glCreateProgram");
    ok &= Load(glxAttachShader, "glAttachShader");
    ok &= Load(glxLinkProgram, "glLinkProgram");
    ok &= Load(glxGetProgramiv, "glGetProgramiv");
    ok &= Load(glxGetUniformLocation, "glGetUniformLocation");
    ok &= Load(glxUniform1i, "glUniform1i");
    ok &= Load(glxUniform3f, "glUniform3f");
    ok &= Load(glxUniformMatrix4fv, "glUniformMatrix4fv");

    Load(glxUniform1f, "glUniform1f");
    Load(glxUniform4f, "glUniform4f");
    Load(glxUniform4fv, "glUniform4fv");
    Load(glxCompressedTexImage2D, "glCompressedTexImage2D");
    Load(glxGenerateMipmap, "glGenerateMipmap");
    Load(glxGenSamplers, "glGenSamplers");
    Load(glxSamplerParameteri, "glSamplerParameteri");

    if (!ok) Log("GL ERROR: some newer OpenGL functions are missing; VR pictures cannot be made.");
    g_loaded = ok;
    return g_loaded;
}

int GlTakeErrors(unsigned& first)
{
    first = 0;
    if (wglGetCurrentContext() == nullptr) return 0;
    int count = 0;
    for (; count < 32; ++count)
    {
        GLenum error = glGetError();
        if (error == GL_NO_ERROR) break;
        if (count == 0) first = (unsigned)error;
    }
    return count;
}

void GlUnbindPixelBuffers()
{
    if (!GlLoadFunctions()) return;
    glxBindBuffer(GLX_PIXEL_UNPACK_BUFFER, 0);
    glxBindBuffer(GLX_PIXEL_PACK_BUFFER, 0);
}

bool GlGetWindowSize(int& width, int& height)
{
    HDC dc = wglGetCurrentDC();
    if (!dc) return false;
    HWND window = WindowFromDC(dc);
    if (!window) return false;

    RECT area;
    if (!GetClientRect(window, &area)) return false;
    width = area.right - area.left;
    height = area.bottom - area.top;
    return width > 0 && height > 0;
}

// ---------------------------------------------------------------------------
// The size of the game's picture, and timing the graphics card
// ---------------------------------------------------------------------------
// (OpenGL 4.5 functions that need nothing bound, so the game's own state is
// never touched. Optional: without them the picture is taken to be the
// window's size and nothing is timed.)
#define GLX_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GLX_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#define GLX_RENDERBUFFER 0x8D41
#define GLX_RENDERBUFFER_WIDTH 0x8D42
#define GLX_RENDERBUFFER_HEIGHT 0x8D43
#define GLX_TIMESTAMP 0x8E28
#define GLX_QUERY_RESULT 0x8866
#define GLX_QUERY_RESULT_AVAILABLE 0x8867
typedef unsigned long long GLxuint64;
typedef void (APIENTRY* GetNamedFramebufferAttachmentParameterivFn)(GLuint framebuffer, GLenum attachment, GLenum name, GLint* value);
typedef void (APIENTRY* GetTextureLevelParameterivFn)(GLuint texture, GLint level, GLenum name, GLint* value);
typedef void (APIENTRY* GetNamedRenderbufferParameterivFn)(GLuint renderbuffer, GLenum name, GLint* value);
typedef void (APIENTRY* GenQueriesFn)(GLsizei count, GLuint* ids);
typedef void (APIENTRY* QueryCounterFn)(GLuint id, GLenum target);
typedef void (APIENTRY* GetQueryObjectivFn)(GLuint id, GLenum name, GLint* value);
typedef void (APIENTRY* GetQueryObjectui64vFn)(GLuint id, GLenum name, GLxuint64* value);
static GetNamedFramebufferAttachmentParameterivFn glxGetNamedFramebufferAttachmentParameteriv = nullptr;
static GetTextureLevelParameterivFn glxGetTextureLevelParameteriv = nullptr;
static GetNamedRenderbufferParameterivFn glxGetNamedRenderbufferParameteriv = nullptr;
static GenQueriesFn glxGenQueries = nullptr;
static QueryCounterFn glxQueryCounter = nullptr;
static GetQueryObjectivFn glxGetQueryObjectiv = nullptr;
static GetQueryObjectui64vFn glxGetQueryObjectui64v = nullptr;

static void LoadExtras()
{
    static bool tried = false;
    if (tried) return;
    tried = true;
    Load(glxGetNamedFramebufferAttachmentParameteriv, "glGetNamedFramebufferAttachmentParameteriv");
    Load(glxGetTextureLevelParameteriv, "glGetTextureLevelParameteriv");
    Load(glxGetNamedRenderbufferParameteriv, "glGetNamedRenderbufferParameteriv");
    Load(glxGenQueries, "glGenQueries");
    Load(glxQueryCounter, "glQueryCounter");
    Load(glxGetQueryObjectiv, "glGetQueryObjectiv");
    Load(glxGetQueryObjectui64v, "glGetQueryObjectui64v");
}

// How big the picture is that `framebuffer` draws into (its first color
// attachment). Asked again only every 90 calls or for another framebuffer.
static bool FramebufferPictureSize(GLuint framebuffer, int& width, int& height)
{
    static GLuint knownFor = 0;
    static int knownWidth = 0, knownHeight = 0;
    static unsigned callsLeft = 0;
    if (framebuffer == knownFor && callsLeft > 0 && knownWidth > 0)
    {
        --callsLeft;
        width = knownWidth;
        height = knownHeight;
        return true;
    }
    LoadExtras();
    if (!glxGetNamedFramebufferAttachmentParameteriv || !glxGetTextureLevelParameteriv || !glxGetNamedRenderbufferParameteriv)
        return false;
    GLint type = 0, name = 0, w = 0, h = 0;
    glxGetNamedFramebufferAttachmentParameteriv(framebuffer, GLX_COLOR_ATTACHMENT0, GLX_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    glxGetNamedFramebufferAttachmentParameteriv(framebuffer, GLX_COLOR_ATTACHMENT0, GLX_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
    if (type == GL_TEXTURE && name != 0)
    {
        glxGetTextureLevelParameteriv((GLuint)name, 0, GL_TEXTURE_WIDTH, &w);
        glxGetTextureLevelParameteriv((GLuint)name, 0, GL_TEXTURE_HEIGHT, &h);
    }
    else if (type == GLX_RENDERBUFFER && name != 0)
    {
        glxGetNamedRenderbufferParameteriv((GLuint)name, GLX_RENDERBUFFER_WIDTH, &w);
        glxGetNamedRenderbufferParameteriv((GLuint)name, GLX_RENDERBUFFER_HEIGHT, &h);
    }
    if (w <= 0 || h <= 0) return false;
    knownFor = framebuffer;
    knownWidth = w;
    knownHeight = h;
    callsLeft = 90;
    width = w;
    height = h;
    return true;
}

bool GlGetPictureSize(GLuint framebuffer, int& width, int& height)
{
    if (!GlGetWindowSize(width, height)) return false;
    if (framebuffer == 0) return true;

    // The game draws into a picture of its own; with the game's "Resolution
    // scale" below 100% that picture can be smaller than the window. Its own
    // size is used only then: smaller both ways, the same shape (within 2%).
    // Otherwise (the same size, or anything unexpected) the window's size is
    // used.
    int pictureWidth = 0, pictureHeight = 0;
    if (!FramebufferPictureSize(framebuffer, pictureWidth, pictureHeight)) return true;
    static int loggedWidth = 0, loggedHeight = 0;
    if (pictureWidth != loggedWidth || pictureHeight != loggedHeight)
    {
        loggedWidth = pictureWidth;
        loggedHeight = pictureHeight;
        Log("GL: the game's picture (drawing target %u) is %d x %d; the window is %d x %d.",
            framebuffer, pictureWidth, pictureHeight, width, height);
    }
    float windowShape = (float)width / (float)height, pictureShape = (float)pictureWidth / (float)pictureHeight;
    if (pictureWidth < width && pictureHeight < height && fabsf(pictureShape - windowShape) < windowShape * 0.02f)
    {
        width = pictureWidth;
        height = pictureHeight;
    }
    return true;
}

// Timing marks: each is a timestamp the graphics card writes when it has
// done everything handed to it before the mark. They are read back several
// frames later, never waited for, so they cost nothing.
static const int kTimerFrames = 8;
static GLuint g_timerQueries[kTimerFrames][kGlTimerMarks];
static bool g_timerUsed[kTimerFrames][kGlTimerMarks];
static bool g_timerReady = false;
static int g_timerSlot = -1;
static double g_timerSum[kGlTimerMarks + 1];   // per mark: ms to the next mark of the frame; the last: the whole frame
static unsigned g_timerCount[kGlTimerMarks + 1];

static bool TimerResult(GLuint query, GLxuint64& value)
{
    GLint available = 0;
    glxGetQueryObjectiv(query, GLX_QUERY_RESULT_AVAILABLE, &available);
    if (!available) return false;
    glxGetQueryObjectui64v(query, GLX_QUERY_RESULT, &value);
    return true;
}

// Adds up a frame's times (it ends where the next frame's first mark is).
static void ReadTimerFrame(int slot)
{
    int next = (slot + 1) % kTimerFrames;
    GLxuint64 times[kGlTimerMarks];
    bool have[kGlTimerMarks];
    for (int m = 0; m < kGlTimerMarks; ++m)
        have[m] = g_timerUsed[slot][m] && TimerResult(g_timerQueries[slot][m], times[m]);
    if (!have[0]) return;
    int last = 0;
    for (int m = 1; m < kGlTimerMarks; ++m)
    {
        if (!have[m] || times[m] < times[last]) continue;
        g_timerSum[last] += (double)(times[m] - times[last]) / 1000000.0;
        ++g_timerCount[last];
        last = m;
    }
    GLxuint64 end = 0;
    if (g_timerUsed[next][0] && TimerResult(g_timerQueries[next][0], end) && end >= times[last])
    {
        g_timerSum[last] += (double)(end - times[last]) / 1000000.0;
        ++g_timerCount[last];
        g_timerSum[kGlTimerMarks] += (double)(end - times[0]) / 1000000.0;
        ++g_timerCount[kGlTimerMarks];
    }
}

void GlTimerMark(int mark)
{
    if (mark < 0 || mark >= kGlTimerMarks || !GlLoadFunctions()) return;
    LoadExtras();
    if (!glxGenQueries || !glxQueryCounter || !glxGetQueryObjectiv || !glxGetQueryObjectui64v) return;
    if (!g_timerReady)
    {
        glxGenQueries(kTimerFrames * kGlTimerMarks, &g_timerQueries[0][0]);
        memset(g_timerUsed, 0, sizeof(g_timerUsed));
        g_timerReady = true;
    }
    if (mark == 0)
    {
        // A new frame: the oldest one is read before its marks are used again.
        g_timerSlot = (g_timerSlot + 1) % kTimerFrames;
        ReadTimerFrame(g_timerSlot);
        for (int m = 0; m < kGlTimerMarks; ++m) g_timerUsed[g_timerSlot][m] = false;
    }
    if (g_timerSlot < 0) return;
    glxQueryCounter(g_timerQueries[g_timerSlot][mark], GLX_TIMESTAMP);
    g_timerUsed[g_timerSlot][mark] = true;
}

bool GlTimerTakeAverages(double averages[kGlTimerMarks + 1])
{
    bool any = g_timerCount[kGlTimerMarks] > 0;
    for (int m = 0; m <= kGlTimerMarks; ++m)
    {
        averages[m] = g_timerCount[m] > 0 ? g_timerSum[m] / (double)g_timerCount[m] : -1.0;
        g_timerSum[m] = 0.0;
        g_timerCount[m] = 0;
    }
    return any;
}

// ---------------------------------------------------------------------------
// Remembering and restoring the game's settings
// ---------------------------------------------------------------------------

void GlStateGuard::Capture()
{
    glGetIntegerv(GLX_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
    glGetIntegerv(GLX_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
    glGetIntegerv(GLX_ACTIVE_TEXTURE, &activeTexture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture2D);

    glxActiveTexture(GLX_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &slot0Texture2D);
    glGetIntegerv(GLX_SAMPLER_BINDING, &slot0Sampler);
    glxActiveTexture((GLenum)activeTexture);

    glGetIntegerv(GLX_CURRENT_PROGRAM, &program);
    glGetIntegerv(GLX_VERTEX_ARRAY_BINDING, &vertexArray);
    glGetIntegerv(GLX_ARRAY_BUFFER_BINDING, &arrayBuffer);
    glGetIntegerv(GLX_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
    glGetIntegerv(GLX_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
    glGetIntegerv(GL_VIEWPORT, viewport);

    scissorTest = glIsEnabled(GL_SCISSOR_TEST);
    framebufferSrgb = glIsEnabled(GLX_FRAMEBUFFER_SRGB);
    depthTest = glIsEnabled(GL_DEPTH_TEST);
    stencilTest = glIsEnabled(GL_STENCIL_TEST);
    cullFace = glIsEnabled(GL_CULL_FACE);
    blend = glIsEnabled(GL_BLEND);

    glGetIntegerv(GLX_BLEND_SRC_RGB, &blendSourceColour);
    glGetIntegerv(GLX_BLEND_DST_RGB, &blendTargetColour);
    glGetIntegerv(GLX_BLEND_SRC_ALPHA, &blendSourceAlpha);
    glGetIntegerv(GLX_BLEND_DST_ALPHA, &blendTargetAlpha);
    glGetIntegerv(GLX_BLEND_EQUATION_RGB, &blendEquationColour);
    glGetIntegerv(GLX_BLEND_EQUATION_ALPHA, &blendEquationAlpha);
    glGetBooleanv(GL_COLOR_WRITEMASK, colourMask);
}

static bool RestoreSwitch(GLenum what, GLboolean wanted)
{
    if (glIsEnabled(what) == wanted) return false;
    if (wanted) glEnable(what); else glDisable(what);
    return true;
}

bool GlStateGuard::Restore()
{
    GlStateGuard now;
    now.Capture();

    bool changed = false;

    if (now.readFramebuffer != readFramebuffer) { glxBindFramebuffer(GLX_READ_FRAMEBUFFER, (GLuint)readFramebuffer); changed = true; }
    if (now.drawFramebuffer != drawFramebuffer) { glxBindFramebuffer(GLX_DRAW_FRAMEBUFFER, (GLuint)drawFramebuffer); changed = true; }

    if (now.slot0Texture2D != slot0Texture2D || now.slot0Sampler != slot0Sampler ||
        now.activeTexture != activeTexture || now.texture2D != texture2D)
    {
        glxActiveTexture(GLX_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)slot0Texture2D);
        glxBindSampler(0, (GLuint)slot0Sampler);
        glxActiveTexture((GLenum)activeTexture);
        glBindTexture(GL_TEXTURE_2D, (GLuint)texture2D);
        changed = true;
    }

    if (now.program != program) { glxUseProgram((GLuint)program); changed = true; }
    if (now.vertexArray != vertexArray) { glxBindVertexArray((GLuint)vertexArray); changed = true; }
    if (now.arrayBuffer != arrayBuffer) { glxBindBuffer(GLX_ARRAY_BUFFER, (GLuint)arrayBuffer); changed = true; }
    if (now.packBuffer != packBuffer) { glxBindBuffer(GLX_PIXEL_PACK_BUFFER, (GLuint)packBuffer); changed = true; }
    if (now.unpackBuffer != unpackBuffer) { glxBindBuffer(GLX_PIXEL_UNPACK_BUFFER, (GLuint)unpackBuffer); changed = true; }

    if (now.viewport[0] != viewport[0] || now.viewport[1] != viewport[1] ||
        now.viewport[2] != viewport[2] || now.viewport[3] != viewport[3])
    {
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        changed = true;
    }

    changed |= RestoreSwitch(GL_SCISSOR_TEST, scissorTest);
    changed |= RestoreSwitch(GLX_FRAMEBUFFER_SRGB, framebufferSrgb);
    changed |= RestoreSwitch(GL_DEPTH_TEST, depthTest);
    changed |= RestoreSwitch(GL_STENCIL_TEST, stencilTest);
    changed |= RestoreSwitch(GL_CULL_FACE, cullFace);
    changed |= RestoreSwitch(GL_BLEND, blend);

    if (now.blendSourceColour != blendSourceColour || now.blendTargetColour != blendTargetColour ||
        now.blendSourceAlpha != blendSourceAlpha || now.blendTargetAlpha != blendTargetAlpha)
    {
        glxBlendFuncSeparate((GLenum)blendSourceColour, (GLenum)blendTargetColour,
                             (GLenum)blendSourceAlpha, (GLenum)blendTargetAlpha);
        changed = true;
    }
    if (now.blendEquationColour != blendEquationColour || now.blendEquationAlpha != blendEquationAlpha)
    {
        glxBlendEquationSeparate((GLenum)blendEquationColour, (GLenum)blendEquationAlpha);
        changed = true;
    }
    if (now.colourMask[0] != colourMask[0] || now.colourMask[1] != colourMask[1] ||
        now.colourMask[2] != colourMask[2] || now.colourMask[3] != colourMask[3])
    {
        glColorMask(colourMask[0], colourMask[1], colourMask[2], colourMask[3]);
        changed = true;
    }
    return changed;
}

// ---------------------------------------------------------------------------
// Off-screen pictures
// ---------------------------------------------------------------------------

bool GlCreateTarget(int width, int height, GLuint& texture, GLuint& framebuffer)
{
    texture = 0;
    framebuffer = 0;
    if (!g_loaded) return false;

    // Remember the few settings this touches, and put them back afterwards.
    GLint oldTexture = 0, oldUnpackBuffer = 0, oldDraw = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    glGetIntegerv(GLX_PIXEL_UNPACK_BUFFER_BINDING, &oldUnpackBuffer);
    glGetIntegerv(GLX_DRAW_FRAMEBUFFER_BINDING, &oldDraw);

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glxBindBuffer(GLX_PIXEL_UNPACK_BUFFER, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, kGlRgba8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GLX_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GLX_CLAMP_TO_EDGE);

    glxGenFramebuffers(1, &framebuffer);
    glxBindFramebuffer(GLX_DRAW_FRAMEBUFFER, framebuffer);
    glxFramebufferTexture2D(GLX_DRAW_FRAMEBUFFER, GLX_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    bool complete = glxCheckFramebufferStatus(GLX_DRAW_FRAMEBUFFER) == GLX_FRAMEBUFFER_COMPLETE;

    glxBindFramebuffer(GLX_DRAW_FRAMEBUFFER, (GLuint)oldDraw);
    glxBindBuffer(GLX_PIXEL_UNPACK_BUFFER, (GLuint)oldUnpackBuffer);
    glBindTexture(GL_TEXTURE_2D, (GLuint)oldTexture);

    if (!complete)
    {
        Log("GL ERROR: could not create a %d x %d off-screen picture.", width, height);
        GlDeleteTarget(texture, framebuffer);
        return false;
    }
    return true;
}

void GlUploadPicture(GLuint texture, int width, int height, const unsigned char* rgbaTopFirst)
{
    if (!g_loaded || texture == 0 || !rgbaTopFirst || width <= 0 || height <= 0) return;

    // OpenGL counts rows from the bottom, so the picture is turned upside down first.
    static unsigned char* flipped = nullptr;
    static int flippedSize = 0;
    int size = width * height * 4;
    if (flippedSize < size)
    {
        delete[] flipped;
        flipped = new unsigned char[size];
        flippedSize = size;
    }
    for (int row = 0; row < height; ++row)
        memcpy(flipped + (size_t)row * width * 4, rgbaTopFirst + (size_t)(height - 1 - row) * width * 4, (size_t)width * 4);

    // Remember the few settings this touches, and put them back afterwards.
    GLint oldTexture = 0, oldUnpackBuffer = 0, oldAlignment = 4, oldRowLength = 0, oldSkipRows = 0, oldSkipPixels = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    glGetIntegerv(GLX_PIXEL_UNPACK_BUFFER_BINDING, &oldUnpackBuffer);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldAlignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &oldRowLength);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &oldSkipRows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &oldSkipPixels);

    glBindTexture(GL_TEXTURE_2D, texture);
    glxBindBuffer(GLX_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped);

    glPixelStorei(GL_UNPACK_ALIGNMENT, oldAlignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, oldRowLength);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, oldSkipRows);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, oldSkipPixels);
    glxBindBuffer(GLX_PIXEL_UNPACK_BUFFER, (GLuint)oldUnpackBuffer);
    glBindTexture(GL_TEXTURE_2D, (GLuint)oldTexture);
}

void GlDeleteTarget(GLuint& texture, GLuint& framebuffer)
{
    if (framebuffer != 0 && g_loaded) glxDeleteFramebuffers(1, &framebuffer);
    if (texture != 0) glDeleteTextures(1, &texture);
    framebuffer = 0;
    texture = 0;
}

GLuint GlCreatePicture(int width, int height, GLint format)
{
    if (!g_loaded || width <= 0 || height <= 0) return 0;

    GLint oldTexture = 0, oldUnpackBuffer = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    glGetIntegerv(GLX_PIXEL_UNPACK_BUFFER_BINDING, &oldUnpackBuffer);

    // Made like SteamVR's own swapchain images: one mip level, linear filtering.
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glxBindBuffer(GLX_PIXEL_UNPACK_BUFFER, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GLX_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GLX_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GLX_CLAMP_TO_EDGE);

    glxBindBuffer(GLX_PIXEL_UNPACK_BUFFER, (GLuint)oldUnpackBuffer);
    glBindTexture(GL_TEXTURE_2D, (GLuint)oldTexture);
    return texture;
}

GLuint GlGetDrawFramebuffer()
{
    GLint value = 0;
    glGetIntegerv(GLX_DRAW_FRAMEBUFFER_BINDING, &value);
    return (GLuint)value;
}

GLuint GlGetReadFramebuffer()
{
    GLint value = 0;
    glGetIntegerv(GLX_READ_FRAMEBUFFER_BINDING, &value);
    return (GLuint)value;
}

void GlSetDrawFramebuffer(GLuint framebuffer)
{
    if (g_loaded) glxBindFramebuffer(GLX_DRAW_FRAMEBUFFER, framebuffer);
}

void GlSetReadFramebuffer(GLuint framebuffer)
{
    if (g_loaded) glxBindFramebuffer(GLX_READ_FRAMEBUFFER, framebuffer);
}

void GlClearTransparent()
{
    GLfloat oldColour[4] = { 0, 0, 0, 0 };
    GLboolean oldMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    glGetFloatv(GL_COLOR_CLEAR_VALUE, oldColour);
    glGetBooleanv(GL_COLOR_WRITEMASK, oldMask);
    GLboolean oldScissor = glIsEnabled(GL_SCISSOR_TEST);

    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    glClearColor(oldColour[0], oldColour[1], oldColour[2], oldColour[3]);
    glColorMask(oldMask[0], oldMask[1], oldMask[2], oldMask[3]);
    if (oldScissor) glEnable(GL_SCISSOR_TEST);
}

// ---------------------------------------------------------------------------
// Copying
// ---------------------------------------------------------------------------
// The functions below change framebuffer bindings and a few switches without
// putting them back. They are only called from places protected by a
// GlStateGuard, which restores everything afterwards.

void GlCopy(GLuint sourceFramebuffer, int sourceLeft, int sourceBottom, int sourceRight, int sourceTop,
            GLuint targetFramebuffer, int targetLeft, int targetBottom, int targetRight, int targetTop)
{
    if (!g_loaded) return;

    // These two settings would interfere with a straight copy.
    glDisable(GL_SCISSOR_TEST);
    glDisable(GLX_FRAMEBUFFER_SRGB);

    glxBindFramebuffer(GLX_READ_FRAMEBUFFER, sourceFramebuffer);
    glxBindFramebuffer(GLX_DRAW_FRAMEBUFFER, targetFramebuffer);

    // When reading from the game's window, read the picture that is about to be shown.
    GLint oldReadBuffer = GL_BACK;
    if (sourceFramebuffer == 0)
    {
        glGetIntegerv(GL_READ_BUFFER, &oldReadBuffer);
        glReadBuffer(GL_BACK);
    }

    glxBlitFramebuffer(sourceLeft, sourceBottom, sourceRight, sourceTop,
                       targetLeft, targetBottom, targetRight, targetTop, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    if (sourceFramebuffer == 0) glReadBuffer((GLenum)oldReadBuffer);
}

void GlCopyToTexture(GLuint sourceFramebuffer, int sourceLeft, int sourceBottom, int sourceRight, int sourceTop,
                     GLuint texture, int textureWidth, int textureHeight)
{
    if (!g_loaded) return;
    if (g_copyFramebuffer == 0) glxGenFramebuffers(1, &g_copyFramebuffer);

    glxBindFramebuffer(GLX_DRAW_FRAMEBUFFER, g_copyFramebuffer);
    glxFramebufferTexture2D(GLX_DRAW_FRAMEBUFFER, GLX_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

    GlCopy(sourceFramebuffer, sourceLeft, sourceBottom, sourceRight, sourceTop,
           g_copyFramebuffer, 0, 0, textureWidth, textureHeight);

    glxFramebufferTexture2D(GLX_DRAW_FRAMEBUFFER, GLX_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
}

void GlMakeSmallerCopies(GLuint texture)
{
    if (!g_loaded || glxGenerateMipmap == nullptr || texture == 0) return;

    GLint oldTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glxGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, (GLuint)oldTexture);
}

bool GlReadGameImage(unsigned char* bgra, int width, int height, GLuint sourceFramebuffer)
{
    if (!g_loaded || !bgra) return false;

    // Read from the window's back buffer (or the given drawing target), straight into our memory.
    glxBindFramebuffer(GLX_READ_FRAMEBUFFER, sourceFramebuffer);
    glxBindBuffer(GLX_PIXEL_PACK_BUFFER, 0);

    GLint oldReadBuffer = GL_BACK;
    glGetIntegerv(GL_READ_BUFFER, &oldReadBuffer);
    glReadBuffer(sourceFramebuffer == 0 ? GL_BACK : GLX_COLOR_ATTACHMENT0);

    GLint oldAlignment = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &oldAlignment);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    while (glGetError() != GL_NO_ERROR) {} // clear old errors so we can check ours
    glReadPixels(0, 0, width, height, GLX_BGRA, GL_UNSIGNED_BYTE, bgra);
    bool ok = glGetError() == GL_NO_ERROR;

    glPixelStorei(GL_PACK_ALIGNMENT, oldAlignment);
    glReadBuffer((GLenum)oldReadBuffer);
    return ok;
}

// ---------------------------------------------------------------------------
// Drawing one picture over another
// ---------------------------------------------------------------------------
// Copying cannot blend, so laying the interface over the 3D picture needs a
// tiny "shader": a program that runs on the graphics card. This one draws a
// picture across the whole target.

static GLuint g_overlayProgram = 0;
static GLuint g_overlayVertexArray = 0;
static bool g_overlayFailed = false;

static GLuint CompileShader(GLenum type, const char* source)
{
    GLuint shader = glxCreateShader(type);
    glxShaderSource(shader, 1, &source, nullptr);
    glxCompileShader(shader);

    GLint ok = 0;
    glxGetShaderiv(shader, GLX_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char message[512] = "";
        glxGetShaderInfoLog(shader, sizeof(message), nullptr, message);
        Log("GL ERROR: overlay shader did not compile: %s", message);
        return 0;
    }
    return shader;
}

// Builds a complete program from its two halves. Returns 0 on failure.
static GLuint BuildProgram(const char* vertexSource, const char* fragmentSource, const char* name)
{
    GLuint vertexShader = CompileShader(GLX_VERTEX_SHADER, vertexSource);
    GLuint fragmentShader = CompileShader(GLX_FRAGMENT_SHADER, fragmentSource);
    if (vertexShader == 0 || fragmentShader == 0)
    {
        Log("GL ERROR: the %s shader could not be built.", name);
        return 0;
    }

    GLuint program = glxCreateProgram();
    glxAttachShader(program, vertexShader);
    glxAttachShader(program, fragmentShader);
    glxLinkProgram(program);

    GLint ok = 0;
    glxGetProgramiv(program, GLX_LINK_STATUS, &ok);
    if (!ok)
    {
        Log("GL ERROR: the %s shader did not link.", name);
        return 0;
    }
    return program;
}

static bool EnsureOverlayProgram()
{
    if (g_overlayProgram != 0) return true;
    if (g_overlayFailed) return false;

    // Three corner points that together cover the whole target.
    const char* vertexSource =
        "#version 150\n"
        "out vec2 uv;\n"
        "void main() {\n"
        "    vec2 corner = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);\n"
        "    uv = corner * 0.5 + 0.5;\n"
        "    gl_Position = vec4(corner, 0.0, 1.0);\n"
        "}\n";
    const char* fragmentSource =
        "#version 150\n"
        "uniform sampler2D picture;\n"
        "in vec2 uv;\n"
        "out vec4 colour;\n"
        "void main() { colour = texture(picture, uv); }\n";

    GLuint program = BuildProgram(vertexSource, fragmentSource, "overlay");
    if (program == 0)
    {
        g_overlayFailed = true;
        return false;
    }

    glxUseProgram(program);
    glxUniform1i(glxGetUniformLocation(program, "picture"), 0); // read from texture slot 0
    glxGenVertexArrays(1, &g_overlayVertexArray);
    g_overlayProgram = program;
    return true;
}

bool GlDrawOver(GLuint texture, GLuint targetFramebuffer, int width, int height)
{
    if (!g_loaded || !EnsureOverlayProgram()) return false;

    glxBindFramebuffer(GLX_DRAW_FRAMEBUFFER, targetFramebuffer);
    glViewport(0, 0, width, height);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GLX_FRAMEBUFFER_SRGB);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // The interface picture already has its colors multiplied by its alpha
    // (premultiplied), so add it on top and dim what is underneath by the same amount.
    glEnable(GL_BLEND);
    glxBlendEquationSeparate(GLX_FUNC_ADD, GLX_FUNC_ADD);
    glxBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glxUseProgram(g_overlayProgram);
    glxBindVertexArray(g_overlayVertexArray);
    glxActiveTexture(GLX_TEXTURE0);
    glxBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, texture);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    return true; // the caller's GlStateGuard puts every setting back
}

// ---------------------------------------------------------------------------
// The game's image correction (gamma, contrast, brightness)
// ---------------------------------------------------------------------------
// The engine corrects its finished frame on the way to the window, with the
// same steps as its base_image_correction_frag shader. A copy of the frame
// taken before that gets the correction here: copied into the bottom-left of
// a picture of the mod's own (one for every copy, as large as the largest
// so far), then drawn from there into the target, pixel for pixel.

static GLuint g_correctProgram = 0;
static GLuint g_correctVertexArray = 0;
static GLint g_correctStepsLocation = -1, g_correctOriginLocation = -1;
static bool g_correctFailed = false;
static GLuint g_correctTexture = 0, g_correctFramebuffer = 0;
static int g_correctWidth = 0, g_correctHeight = 0;

static bool EnsureCorrectProgram()
{
    if (g_correctProgram != 0) return true;
    if (g_correctFailed || glxUniform4f == nullptr) return false;

    const char* vertexSource =
        "#version 150\n"
        "void main() {\n"
        "    vec2 corner = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);\n"
        "    gl_Position = vec4(corner, 0.0, 1.0);\n"
        "}\n";
    // steps: the power (1 / gamma), the contrast, the brightness, and 1 for a
    // picture with its colours multiplied by how solid they are (the
    // interface's), which is corrected as the colours themselves, then
    // multiplied again; light beyond what a pixel covers (an added glow:
    // color above its solidness) is kept as it is. origin: the target's pixel
    // that reads the picture's first.
    const char* fragmentSource =
        "#version 150\n"
        "uniform sampler2D picture;\n"
        "uniform vec4 steps;\n"
        "uniform vec3 origin;\n"
        "out vec4 colour;\n"
        "void main() {\n"
        "    vec4 t = texelFetch(picture, ivec2(gl_FragCoord.xy - origin.xy), 0);\n"
        "    bool multiplied = steps.w > 0.5;\n"
        "    float solid = multiplied ? max(t.a, 1.0 / 255.0) : 1.0;\n"
        "    vec3 c = pow(max(t.rgb / solid, vec3(0.0)), vec3(steps.x));\n"
        "    c = clamp(((c - 0.5) * steps.y + 0.5) * steps.z, 0.0, 1.0);\n"
        "    vec3 added = multiplied ? max(t.rgb - vec3(t.a), vec3(0.0)) : vec3(0.0);\n"
        "    colour = vec4(multiplied ? c * t.a + added : c, t.a);\n"
        "}\n";

    GLuint program = BuildProgram(vertexSource, fragmentSource, "image correction");
    if (program == 0)
    {
        g_correctFailed = true;
        return false;
    }

    glxUseProgram(program);
    glxUniform1i(glxGetUniformLocation(program, "picture"), 0); // read from texture slot 0
    g_correctStepsLocation = glxGetUniformLocation(program, "steps");
    g_correctOriginLocation = glxGetUniformLocation(program, "origin");
    glxGenVertexArrays(1, &g_correctVertexArray);
    g_correctProgram = program;
    return true;
}

bool GlCopyCorrected(GLuint sourceFramebuffer, int sourceLeft, int sourceBottom, int sourceRight, int sourceTop,
                     GLuint targetFramebuffer, int targetLeft, int targetBottom, int targetRight, int targetTop,
                     float gamma, float brightness, float contrast, bool premultiplied)
{
    int width = targetRight - targetLeft, height = targetTop - targetBottom;
    if (!g_loaded || g_correctFailed || width <= 0 || height <= 0 || !(gamma > 0.0f) || !EnsureCorrectProgram()) return false;
    if (width > g_correctWidth || height > g_correctHeight)
    {
        int newWidth = width > g_correctWidth ? width : g_correctWidth;
        int newHeight = height > g_correctHeight ? height : g_correctHeight;
        GlDeleteTarget(g_correctTexture, g_correctFramebuffer);
        g_correctWidth = g_correctHeight = 0;
        if (!GlCreateTarget(newWidth, newHeight, g_correctTexture, g_correctFramebuffer))
        {
            g_correctFailed = true; // (not tried again every frame: the pictures are copied uncorrected from now on)
            return false;
        }
        g_correctWidth = newWidth;
        g_correctHeight = newHeight;
    }
    GlCopy(sourceFramebuffer, sourceLeft, sourceBottom, sourceRight, sourceTop, g_correctFramebuffer, 0, 0, width, height);

    glxBindFramebuffer(GLX_DRAW_FRAMEBUFFER, targetFramebuffer);
    glViewport(targetLeft, targetBottom, width, height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GLX_FRAMEBUFFER_SRGB);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glxUseProgram(g_correctProgram);
    glxUniform4f(g_correctStepsLocation, 1.0f / gamma, contrast, brightness, premultiplied ? 1.0f : 0.0f);
    glxUniform3f(g_correctOriginLocation, (float)targetLeft, (float)targetBottom, 0.0f);
    glxBindVertexArray(g_correctVertexArray);
    glxActiveTexture(GLX_TEXTURE0);
    glxBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, g_correctTexture);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    return true; // the caller's GlStateGuard puts every setting back
}

bool GlCopyToTextureCorrected(GLuint sourceFramebuffer, int sourceLeft, int sourceBottom, int sourceRight, int sourceTop,
                              GLuint texture, int textureWidth, int textureHeight, float gamma, float brightness, float contrast,
                              bool premultiplied)
{
    if (!g_loaded) return false;
    if (g_copyFramebuffer == 0) glxGenFramebuffers(1, &g_copyFramebuffer);
    glxBindFramebuffer(GLX_DRAW_FRAMEBUFFER, g_copyFramebuffer);
    glxFramebufferTexture2D(GLX_DRAW_FRAMEBUFFER, GLX_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    bool drawn = GlCopyCorrected(sourceFramebuffer, sourceLeft, sourceBottom, sourceRight, sourceTop, g_copyFramebuffer, 0, 0,
                                 textureWidth, textureHeight, gamma, brightness, contrast, premultiplied);
    glxBindFramebuffer(GLX_DRAW_FRAMEBUFFER, g_copyFramebuffer);
    glxFramebufferTexture2D(GLX_DRAW_FRAMEBUFFER, GLX_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    return drawn;
}

// ---------------------------------------------------------------------------
// Screen effects (blood and dark edges across the face)
// ---------------------------------------------------------------------------

GLuint GlLoadDdsTexture(const char* path)
{
    if (!g_loaded || glxCompressedTexImage2D == nullptr || path == nullptr) return 0;

    FILE* file = fopen(path, "rb");
    if (file == nullptr) return 0;

    // A .dds file starts with "DDS " and a 124-byte description; the picture
    // itself follows, largest size first.
    unsigned char header[128];
    GLuint texture = 0;
    unsigned char* data = nullptr;
    do
    {
        if (fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "DDS ", 4) != 0) break;

        unsigned int height = 0, width = 0;
        memcpy(&height, header + 12, 4);
        memcpy(&width, header + 16, 4);
        if (width == 0 || height == 0 || width > 8192 || height > 8192) break;

        // How the picture is packed: three kinds are understood.
        GLenum format = 0;
        unsigned int blockBytes = 16;
        if (memcmp(header + 84, "DXT1", 4) == 0) { format = 0x83F1; blockBytes = 8; }
        else if (memcmp(header + 84, "DXT3", 4) == 0) format = 0x83F2;
        else if (memcmp(header + 84, "DXT5", 4) == 0) format = 0x83F3;
        else break;

        unsigned int size = ((width + 3) / 4) * ((height + 3) / 4) * blockBytes;
        data = (unsigned char*)malloc(size);
        if (data == nullptr || fread(data, 1, size, file) != size) break;

        GLint oldTexture = 0, oldUnpackBuffer = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
        glGetIntegerv(GLX_PIXEL_UNPACK_BUFFER_BINDING, &oldUnpackBuffer);
        glxBindBuffer(GLX_PIXEL_UNPACK_BUFFER, 0);

        while (glGetError() != GL_NO_ERROR) {} // forget older complaints

        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GLX_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GLX_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, 0x813D /* highest size level in use */, 0);
        glxCompressedTexImage2D(GL_TEXTURE_2D, 0, format, (GLsizei)width, (GLsizei)height, 0, (GLsizei)size, data);
        if (glGetError() != GL_NO_ERROR)
        {
            glDeleteTextures(1, &texture);
            texture = 0;
        }

        glxBindBuffer(GLX_PIXEL_UNPACK_BUFFER, (GLuint)oldUnpackBuffer);
        glBindTexture(GL_TEXTURE_2D, (GLuint)oldTexture);
    } while (false);

    free(data);
    fclose(file);
    return texture;
}

static GLuint g_effectProgram = 0;
static GLuint g_effectVertexArray = 0;
static GLint g_effectMapLocation = -1;
static GLint g_effectStrengthLocation = -1;
static GLint g_effectMultiplyLocation = -1;
static GLint g_effectTintLocation = -1;
static bool g_effectFailed = false;

static bool EnsureEffectProgram()
{
    if (g_effectProgram != 0) return true;
    if (g_effectFailed || glxUniform1f == nullptr || glxUniform4f == nullptr) return false;

    // Three corner points that together cover the whole target.
    const char* vertexSource =
        "#version 150\n"
        "uniform vec4 map;\n" // xy = scale, zw = offset
        "out vec2 uv;\n"
        "void main() {\n"
        "    vec2 corner = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);\n"
        "    uv = corner * map.xy + map.zw;\n"
        "    gl_Position = vec4(corner, 0.0, 1.0);\n"
        "}\n";
    // Laid over: the texture's color, as solid as its alpha times strength.
    // Multiply: the target is darkened towards the texture's color.
    // Glass: as the game's own shader multiplies (posteffect_screen_mat_frag):
    // the texture's alpha counts too. The game does it before its final
    // brightness curve and this is after it, hence the power.
    const char* fragmentSource =
        "#version 150\n"
        "uniform sampler2D picture;\n"
        "uniform float strength;\n"
        "uniform float multiply;\n" // 0 = laid over, 1 = multiply, 2 = one flat color, 3 = glass
        "uniform vec4 tint;\n"
        "in vec2 uv;\n"
        "out vec4 colour;\n"
        "void main() {\n"
        "    if (multiply > 1.5 && multiply < 2.5) { colour = vec4(tint.rgb, strength); return; }\n"
        "    vec4 t = texture(picture, uv);\n"
        "    if (multiply > 2.5) { colour = vec4(pow(mix(vec3(1.0), t.rgb * tint.rgb, strength * t.a), vec3(1.0 / 2.2)), 1.0); return; }\n"
        "    t.rgb *= tint.rgb;\n"
        "    if (multiply > 0.5) colour = vec4(mix(vec3(1.0), t.rgb, strength), 1.0);\n"
        "    else colour = vec4(t.rgb, t.a * strength);\n"
        "}\n";

    GLuint program = BuildProgram(vertexSource, fragmentSource, "screen effect");
    if (program == 0)
    {
        g_effectFailed = true;
        return false;
    }

    glxUseProgram(program);
    glxUniform1i(glxGetUniformLocation(program, "picture"), 0); // read from texture slot 0
    g_effectMapLocation = glxGetUniformLocation(program, "map");
    g_effectStrengthLocation = glxGetUniformLocation(program, "strength");
    g_effectMultiplyLocation = glxGetUniformLocation(program, "multiply");
    g_effectTintLocation = glxGetUniformLocation(program, "tint");
    glxGenVertexArrays(1, &g_effectVertexArray);
    g_effectProgram = program;
    return true;
}

// The part the kinds of screen effect share. `mode`: 0 = laid over,
// 1 = multiply, 2 = one flat color (no texture), 3 = glass.
static bool DrawEffect(GLuint texture, GLuint targetFramebuffer, int width, int height, const float uvScale[2],
                       const float uvOffset[2], float strength, int mode, const float* tint)
{
    if (!g_loaded || (texture == 0 && mode != 2) || !EnsureEffectProgram()) return false;
    bool multiply = mode == 1 || mode == 3;

    glxBindFramebuffer(GLX_DRAW_FRAMEBUFFER, targetFramebuffer);
    glViewport(0, 0, width, height);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GLX_FRAMEBUFFER_SRGB);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE); // the picture stays fully solid

    glEnable(GL_BLEND);
    glxBlendEquationSeparate(GLX_FUNC_ADD, GLX_FUNC_ADD);
    if (multiply) glxBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ZERO, GL_ONE);
    else glxBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

    glxUseProgram(g_effectProgram);
    glxUniform4f(g_effectMapLocation, uvScale[0], uvScale[1], uvOffset[0], uvOffset[1]);
    glxUniform1f(g_effectStrengthLocation, strength);
    glxUniform1f(g_effectMultiplyLocation, (float)mode);
    if (tint != nullptr) glxUniform4f(g_effectTintLocation, tint[0], tint[1], tint[2], 1.0f);
    else glxUniform4f(g_effectTintLocation, 1.0f, 1.0f, 1.0f, 1.0f);
    glxBindVertexArray(g_effectVertexArray);
    glxActiveTexture(GLX_TEXTURE0);
    glxBindSampler(0, 0);
    if (texture != 0) glBindTexture(GL_TEXTURE_2D, texture);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    return true; // the caller's GlStateGuard puts every setting back
}

bool GlDrawScreenEffect(GLuint texture, GLuint targetFramebuffer, int width, int height, const float uvScale[2],
                        const float uvOffset[2], float strength, bool multiply, const float* tint)
{
    return DrawEffect(texture, targetFramebuffer, width, height, uvScale, uvOffset, strength, multiply ? 1 : 0, tint);
}

bool GlDrawScreenGlass(GLuint texture, GLuint targetFramebuffer, int width, int height, const float uvScale[2],
                       const float uvOffset[2], float strength)
{
    return DrawEffect(texture, targetFramebuffer, width, height, uvScale, uvOffset, strength, 3, nullptr);
}

bool GlDrawSolid(GLuint targetFramebuffer, int width, int height, const float colour[3], float strength)
{
    const float one[2] = { 1.0f, 1.0f }, zero[2] = { 0.0f, 0.0f };
    return DrawEffect(0, targetFramebuffer, width, height, one, zero, strength, 2, colour);
}

// ---------------------------------------------------------------------------
// The comfort ring (vignette): the edges of the view darkened
// ---------------------------------------------------------------------------
// A program of its own, so that if it ever failed to build, only the ring
// would be missing, never the screen effects and fades above.

static GLuint g_ringProgram = 0;
static GLuint g_ringVertexArray = 0;
static GLint g_ringViewLocation = -1;
static GLint g_ringEdgesLocation = -1;
static bool g_ringFailed = false;

static bool EnsureRingProgram()
{
    if (g_ringProgram != 0) return true;
    if (g_ringFailed || glxUniform4f == nullptr) return false;

    // Three corner points that together cover the whole target; each point
    // of it as the tangents of its angles from straight ahead.
    const char* vertexSource =
        "#version 150\n"
        "uniform vec4 view;\n" // xy = the picture's half width and half height as tangents
        "out vec2 slope;\n"
        "void main() {\n"
        "    vec2 corner = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);\n"
        "    slope = corner * view.xy;\n"
        "    gl_Position = vec4(corner, 0.0, 1.0);\n"
        "}\n";
    // Clear inside edges.x, black from edges.y out, a smooth step between;
    // all of it as solid as edges.z.
    const char* fragmentSource =
        "#version 150\n"
        "uniform vec4 edges;\n" // x = clear within, y = black beyond (tangents), z = strength
        "in vec2 slope;\n"
        "out vec4 colour;\n"
        "void main() {\n"
        "    float dark = smoothstep(edges.x, edges.y, length(slope)) * edges.z;\n"
        "    colour = vec4(0.0, 0.0, 0.0, dark);\n"
        "}\n";

    GLuint program = BuildProgram(vertexSource, fragmentSource, "comfort ring");
    if (program == 0)
    {
        g_ringFailed = true;
        return false;
    }

    g_ringViewLocation = glxGetUniformLocation(program, "view");
    g_ringEdgesLocation = glxGetUniformLocation(program, "edges");
    glxGenVertexArrays(1, &g_ringVertexArray);
    g_ringProgram = program;
    return true;
}

bool GlDrawRing(GLuint targetFramebuffer, int width, int height, float tanHalfWidth, float tanHalfHeight, float clearWithin,
                float blackBeyond, float strength)
{
    if (!g_loaded || !EnsureRingProgram()) return false;

    glxBindFramebuffer(GLX_DRAW_FRAMEBUFFER, targetFramebuffer);
    glViewport(0, 0, width, height);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GLX_FRAMEBUFFER_SRGB);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE); // the picture stays fully solid

    glEnable(GL_BLEND);
    glxBlendEquationSeparate(GLX_FUNC_ADD, GLX_FUNC_ADD);
    glxBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

    glxUseProgram(g_ringProgram);
    glxUniform4f(g_ringViewLocation, tanHalfWidth, tanHalfHeight, 0.0f, 0.0f);
    glxUniform4f(g_ringEdgesLocation, clearWithin, blackBeyond, strength, 0.0f);
    glxBindVertexArray(g_ringVertexArray);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    return true; // the caller's GlStateGuard puts every setting back
}

// ---------------------------------------------------------------------------
// The hit's edge: the view's edges reddened as the player is hurt, most on
// the side the hit came from
// ---------------------------------------------------------------------------
// A program of its own, as the ring's: if it ever failed to build, only the
// hit's edge would be missing.

static GLuint g_hurtProgram = 0;
static GLuint g_hurtVertexArray = 0;
static GLint g_hurtViewLocation = -1;
static GLint g_hurtHitLocation = -1;
static bool g_hurtFailed = false;

static bool EnsureHurtProgram()
{
    if (g_hurtProgram != 0) return true;
    if (g_hurtFailed || glxUniform4f == nullptr) return false;

    // The same corner points as the ring's: each point of the target as the tangents of its angles from straight ahead.
    const char* vertexSource =
        "#version 150\n"
        "uniform vec4 view;\n" // xy = the picture's half width and half height as tangents
        "out vec2 slope;\n"
        "void main() {\n"
        "    vec2 corner = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);\n"
        "    slope = corner * view.xy;\n"
        "    gl_Position = vec4(corner, 0.0, 1.0);\n"
        "}\n";
    // A deep red rising from about 14 degrees out to the edges; on the hit's side (hit.xy, a direction on the
    // picture, or none for all round) as strong as hit.z, on the far side a quarter of that; and the whole view
    // slightly darkened with it (the shock).
    const char* fragmentSource =
        "#version 150\n"
        "uniform vec4 hit;\n" // xy = the side the hit came from (a unit direction, or 0 0 for all round), z = strength
        "in vec2 slope;\n"
        "out vec4 colour;\n"
        "void main() {\n"
        "    float out_ = length(slope);\n"
        "    float edge = smoothstep(0.25, 1.1, out_);\n"
        "    float side = 1.0;\n"
        "    if (dot(hit.xy, hit.xy) > 0.01) {\n"
        "        vec2 towards = out_ > 0.0001 ? slope / out_ : vec2(0.0);\n"
        "        side = mix(0.25, 1.0, smoothstep(-0.3, 0.8, dot(towards, hit.xy)));\n"
        "    }\n"
        "    float a = clamp(max(edge * side, 0.08) * hit.z, 0.0, 1.0);\n"
        "    colour = vec4(0.38, 0.0, 0.0, a);\n"
        "}\n";

    GLuint program = BuildProgram(vertexSource, fragmentSource, "hit's edge");
    if (program == 0)
    {
        g_hurtFailed = true;
        return false;
    }

    g_hurtViewLocation = glxGetUniformLocation(program, "view");
    g_hurtHitLocation = glxGetUniformLocation(program, "hit");
    glxGenVertexArrays(1, &g_hurtVertexArray);
    g_hurtProgram = program;
    return true;
}

bool GlDrawHurtEdge(GLuint targetFramebuffer, int width, int height, float tanHalfWidth, float tanHalfHeight, float sideX,
                    float sideY, float strength)
{
    if (!g_loaded || !EnsureHurtProgram()) return false;

    glxBindFramebuffer(GLX_DRAW_FRAMEBUFFER, targetFramebuffer);
    glViewport(0, 0, width, height);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GLX_FRAMEBUFFER_SRGB);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE); // the picture stays fully solid

    glEnable(GL_BLEND);
    glxBlendEquationSeparate(GLX_FUNC_ADD, GLX_FUNC_ADD);
    glxBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

    glxUseProgram(g_hurtProgram);
    glxUniform4f(g_hurtViewLocation, tanHalfWidth, tanHalfHeight, 0.0f, 0.0f);
    glxUniform4f(g_hurtHitLocation, sideX, sideY, strength, 0.0f);
    glxBindVertexArray(g_hurtVertexArray);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    return true; // the caller's GlStateGuard puts every setting back
}

// ---------------------------------------------------------------------------
// Drawing solid boxes (the hand shapes)
// ---------------------------------------------------------------------------
// Each box is drawn from 36 corner points (6 faces x 2 triangles x 3 corners).
// The points are worked out inside the shader from their number alone, so no
// model data has to be sent to the graphics card.
//
// A box is drawn without a depth buffer: instead, each face is left out when
// it points away from the eye. For a single box that always gives the right
// picture.

static GLuint g_boxProgram = 0;
static GLuint g_boxVertexArray = 0;
static GLint g_boxModelViewLocation = -1;
static GLint g_boxProjectionLocation = -1;
static GLint g_boxColourLocation = -1;
static bool g_boxFailed = false;

static bool EnsureBoxProgram()
{
    if (g_boxProgram != 0) return true;
    if (g_boxFailed) return false;

    const char* vertexSource =
        "#version 150\n"
        "uniform mat4 modelView;\n"
        "uniform mat4 projection;\n"
        "flat out float shade;\n"
        "flat out float facesEye;\n"
        "void main() {\n"
        "    vec3 normals[6] = vec3[6](vec3(1,0,0), vec3(-1,0,0), vec3(0,1,0), vec3(0,-1,0), vec3(0,0,1), vec3(0,0,-1));\n"
        "    vec2 corners[6] = vec2[6](vec2(-1,-1), vec2(1,-1), vec2(1,1), vec2(-1,-1), vec2(1,1), vec2(-1,1));\n"
        "    int face = gl_VertexID / 6;\n"
        "    int corner = gl_VertexID - face * 6;\n"
        "    vec3 n = normals[face];\n"
        "    vec3 u = vec3(n.y, n.z, n.x);\n"
        "    vec3 v = vec3(n.z, n.x, n.y);\n"
        "    vec3 point = (n + corners[corner].x * u + corners[corner].y * v) * 0.5;\n"
        "    vec4 seen = modelView * vec4(point, 1.0);\n"
        "    vec3 seenNormal = normalize(mat3(modelView) * n);\n"
        "    vec3 towardsEye = normalize(-seen.xyz);\n"
        "    float facing = dot(seenNormal, towardsEye);\n"
        "    facesEye = facing > 0.0 ? 1.0 : 0.0;\n"
        "    shade = 0.45 + 0.55 * max(facing, 0.0);\n"
        "    gl_Position = projection * seen;\n"
        "}\n";
    const char* fragmentSource =
        "#version 150\n"
        "uniform vec3 boxColour;\n"
        "flat in float shade;\n"
        "flat in float facesEye;\n"
        "out vec4 colour;\n"
        "void main() {\n"
        "    if (facesEye < 0.5) discard;\n"
        "    colour = vec4(boxColour * shade, 1.0);\n"
        "}\n";

    GLuint program = BuildProgram(vertexSource, fragmentSource, "hand");
    if (program == 0)
    {
        g_boxFailed = true;
        return false;
    }

    g_boxModelViewLocation = glxGetUniformLocation(program, "modelView");
    g_boxProjectionLocation = glxGetUniformLocation(program, "projection");
    g_boxColourLocation = glxGetUniformLocation(program, "boxColour");
    glxGenVertexArrays(1, &g_boxVertexArray);
    g_boxProgram = program;
    return true;
}

bool GlDrawBoxes(GLuint targetFramebuffer, int width, int height, const float* projection,
                 const float* boxes, const float* colours, int boxCount)
{
    if (!g_loaded || boxCount <= 0 || !EnsureBoxProgram()) return false;

    glxBindFramebuffer(GLX_DRAW_FRAMEBUFFER, targetFramebuffer);
    glViewport(0, 0, width, height);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GLX_FRAMEBUFFER_SRGB);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glxUseProgram(g_boxProgram);
    glxBindVertexArray(g_boxVertexArray);

    // GL_TRUE: our numbers are stored row after row.
    glxUniformMatrix4fv(g_boxProjectionLocation, 1, GL_TRUE, projection);
    for (int i = 0; i < boxCount; ++i)
    {
        glxUniformMatrix4fv(g_boxModelViewLocation, 1, GL_TRUE, boxes + i * 16);
        glxUniform3f(g_boxColourLocation, colours[i * 3 + 0], colours[i * 3 + 1], colours[i * 3 + 2]);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }
    return true; // the caller's GlStateGuard puts every setting back
}

// ---------------------------------------------------------------------------
// The panels drawn into an eye's picture (SteamVR)
// ---------------------------------------------------------------------------
// What a headset's own compositor does with its quad layers, done here: see
// PanelsIntoEyes in vr.cpp for why. The target is a picture of the headset
// software's (an eye's), attached to a framebuffer of our own while drawn into.

static GLuint g_intoFramebuffer = 0;
static GLuint g_intoSampler = 0;
static GLuint g_intoProgram = 0, g_intoVertexArray = 0;
static GLint g_intoCornersLocation = -1, g_intoPartLocation = -1, g_intoSolidLocation = -1;
static GLuint g_seenProgram = 0, g_seenVertexArray = 0;
static GLint g_seenRowsLocation = -1, g_seenViewLocation = -1, g_seenPictureLocation = -1;
static bool g_intoFailed = false, g_seenFailed = false;

// Aims drawing at `targetTexture` (width x height pixels).
static bool AimAtTexture(GLuint targetTexture, int width, int height)
{
    if (targetTexture == 0 || width <= 0 || height <= 0) return false;
    if (g_intoFramebuffer == 0) glxGenFramebuffers(1, &g_intoFramebuffer);
    if (g_intoFramebuffer == 0) return false;
    glxBindFramebuffer(GLX_DRAW_FRAMEBUFFER, g_intoFramebuffer);
    glxFramebufferTexture2D(GLX_DRAW_FRAMEBUFFER, GLX_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targetTexture, 0);
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    return true;
}

// Lets go of the target again (our framebuffer keeps nothing of the headset's).
static void LetGoOfTexture()
{
    glxBindFramebuffer(GLX_DRAW_FRAMEBUFFER, g_intoFramebuffer);
    glxFramebufferTexture2D(GLX_DRAW_FRAMEBUFFER, GLX_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
}

bool GlFillTexture(GLuint targetTexture, int width, int height, const float rgba[4])
{
    if (!g_loaded || !AimAtTexture(targetTexture, width, height)) return false;
    GLfloat oldColour[4] = { 0, 0, 0, 0 };
    glGetFloatv(GL_COLOR_CLEAR_VALUE, oldColour);
    glDisable(GLX_FRAMEBUFFER_SRGB);
    glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(oldColour[0], oldColour[1], oldColour[2], oldColour[3]); // the guard doesn't keep the clear color
    LetGoOfTexture();
    return true; // the caller's GlStateGuard puts every other setting back
}

static bool EnsureIntoProgram()
{
    if (g_intoProgram != 0) return true;
    if (g_intoFailed) return false;
    g_intoFailed = true; // (until made)
    if (glxGenSamplers == nullptr || glxSamplerParameteri == nullptr || glxUniform1f == nullptr ||
        glxUniform4f == nullptr || glxUniform4fv == nullptr)
    {
        Log("GL ERROR: the OpenGL functions for drawing the panels into the eyes' pictures are missing.");
        return false;
    }

    // As the panel's (GlDrawPanelPart): two triangles between corners already
    // seen from the eye. Each pixel is the mean of four samples spread over the
    // part of the picture it covers, so text drawn smaller than its picture
    // stays smooth (the headset's pictures have no mipmaps).
    const char* vertexSource =
        "#version 150\n"
        "uniform vec4 corners[4];\n" // bottom left, bottom right, top left, top right
        "uniform vec4 part;\n"
        "out vec2 uv;\n"
        "void main() {\n"
        "    int order[6] = int[6](0, 1, 3, 0, 3, 2);\n"
        "    int c = order[gl_VertexID];\n"
        "    uv = mix(part.xy, part.zw, vec2((c == 1 || c == 3) ? 1.0 : 0.0, c >= 2 ? 1.0 : 0.0));\n"
        "    gl_Position = corners[c];\n"
        "}\n";
    const char* fragmentSource =
        "#version 150\n"
        "uniform sampler2D picture;\n"
        "uniform float solid;\n" // 1: its see-through parts ignored
        "in vec2 uv;\n"
        "out vec4 colour;\n"
        "void main() {\n"
        "    vec2 across = dFdx(uv) * 0.25, up = dFdy(uv) * 0.25;\n"
        "    vec4 sum = texture(picture, uv - across - up) + texture(picture, uv + across - up)\n"
        "             + texture(picture, uv - across + up) + texture(picture, uv + across + up);\n"
        "    colour = sum * 0.25;\n"
        "    if (solid > 0.5) colour.a = 1.0;\n"
        "}\n";
    GLuint program = BuildProgram(vertexSource, fragmentSource, "panel into an eye");
    if (program == 0) return false;

    glxUseProgram(program);
    glxUniform1i(glxGetUniformLocation(program, "picture"), 0);
    g_intoCornersLocation = glxGetUniformLocation(program, "corners");
    g_intoPartLocation = glxGetUniformLocation(program, "part");
    g_intoSolidLocation = glxGetUniformLocation(program, "solid");
    glxGenVertexArrays(1, &g_intoVertexArray);

    // The headset's pictures are read through this sampler: linear, clamped to
    // the edges (their own settings ask for mipmaps they don't have).
    glxGenSamplers(1, &g_intoSampler);
    glxSamplerParameteri(g_intoSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glxSamplerParameteri(g_intoSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glxSamplerParameteri(g_intoSampler, GL_TEXTURE_WRAP_S, GLX_CLAMP_TO_EDGE);
    glxSamplerParameteri(g_intoSampler, GL_TEXTURE_WRAP_T, GLX_CLAMP_TO_EDGE);

    g_intoProgram = program;
    g_intoFailed = false;
    return true;
}

bool GlDrawPanelIntoTexture(GLuint targetTexture, int width, int height, GLuint texture, const float corners[16],
                            const float part[4], bool blend)
{
    if (!g_loaded || texture == 0 || !EnsureIntoProgram() || !AimAtTexture(targetTexture, width, height)) return false;

    // Mixed in linear light: a target in sRGB encoding is read as linear
    // light and the result written back in sRGB (other targets as they are).
    glEnable(GLX_FRAMEBUFFER_SRGB);
    if (blend)
    {
        // Colors already multiplied by their alpha (OpenXR's default for layers).
        glEnable(GL_BLEND);
        glxBlendEquationSeparate(GLX_FUNC_ADD, GLX_FUNC_ADD);
        glxBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    else
    {
        glDisable(GL_BLEND);
    }

    glxUseProgram(g_intoProgram);
    glxUniform4fv(g_intoCornersLocation, 4, corners);
    glxUniform4f(g_intoPartLocation, part[0], part[1], part[2], part[3]);
    glxUniform1f(g_intoSolidLocation, blend ? 0.0f : 1.0f);
    glxBindVertexArray(g_intoVertexArray);
    glxActiveTexture(GLX_TEXTURE0);
    glxBindSampler(0, g_intoSampler);
    glBindTexture(GL_TEXTURE_2D, texture);

    glDrawArrays(GL_TRIANGLES, 0, 6);
    LetGoOfTexture();
    return true; // the caller's GlStateGuard puts every setting back (the sampler too)
}

static bool EnsureSeenProgram()
{
    if (g_seenProgram != 0) return true;
    if (g_seenFailed) return false;
    g_seenFailed = true; // (until made)
    if (glxUniform4fv == nullptr) return false;

    // For each pixel: the direction it looks in from the eye now, turned into
    // the eye as it was when the picture was taken, and where that lands in
    // the picture (its edges and the eye's as tangents: left, down, right, up).
    const char* vertexSource =
        "#version 150\n"
        "out vec2 uv;\n"
        "void main() {\n"
        "    vec2 corner = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);\n"
        "    uv = corner * 0.5 + 0.5;\n"
        "    gl_Position = vec4(corner, 0.0, 1.0);\n"
        "}\n";
    const char* fragmentSource =
        "#version 150\n"
        "uniform sampler2D picture;\n"
        "uniform vec4 turnRows[3];\n"
        "uniform vec4 view;\n"
        "uniform vec4 seenBy;\n"
        "in vec2 uv;\n"
        "out vec4 colour;\n"
        "void main() {\n"
        "    vec3 ray = vec3(mix(view.x, view.z, uv.x), mix(view.y, view.w, uv.y), -1.0);\n"
        "    vec3 seen = vec3(dot(turnRows[0].xyz, ray), dot(turnRows[1].xyz, ray), dot(turnRows[2].xyz, ray));\n"
        "    if (seen.z > -0.0001) { colour = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
        "    vec2 spot = (seen.xy / -seen.z - seenBy.xy) / (seenBy.zw - seenBy.xy);\n"
        "    if (spot.x < 0.0 || spot.x > 1.0 || spot.y < 0.0 || spot.y > 1.0) { colour = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
        "    colour = vec4(texture(picture, spot).rgb, 1.0);\n"
        "}\n";
    GLuint program = BuildProgram(vertexSource, fragmentSource, "eye seen again");
    if (program == 0) return false;

    glxUseProgram(program);
    glxUniform1i(glxGetUniformLocation(program, "picture"), 0);
    g_seenRowsLocation = glxGetUniformLocation(program, "turnRows");
    g_seenViewLocation = glxGetUniformLocation(program, "view");
    g_seenPictureLocation = glxGetUniformLocation(program, "seenBy");
    glxGenVertexArrays(1, &g_seenVertexArray);
    g_seenProgram = program;
    g_seenFailed = false;
    return true;
}

bool GlDrawEyeSeenAgain(GLuint targetTexture, int width, int height, GLuint texture, const float turn[9],
                        const float pictureEdges[4], const float viewEdges[4])
{
    if (!g_loaded || texture == 0 || !EnsureSeenProgram() || !AimAtTexture(targetTexture, width, height)) return false;

    glDisable(GLX_FRAMEBUFFER_SRGB); // copied as it is, like the eyes' pictures always are
    glDisable(GL_BLEND);             // every pixel is written, solid

    const float rows[12] = { turn[0], turn[1], turn[2], 0.0f,
                             turn[3], turn[4], turn[5], 0.0f,
                             turn[6], turn[7], turn[8], 0.0f };
    glxUseProgram(g_seenProgram);
    glxUniform4fv(g_seenRowsLocation, 3, rows);
    glxUniform4fv(g_seenViewLocation, 1, viewEdges);
    glxUniform4fv(g_seenPictureLocation, 1, pictureEdges);
    glxBindVertexArray(g_seenVertexArray);
    glxActiveTexture(GLX_TEXTURE0);
    glxBindSampler(0, 0); // our own picture: its own settings are linear, clamped to the edges
    glBindTexture(GL_TEXTURE_2D, texture);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    LetGoOfTexture();
    return true; // the caller's GlStateGuard puts every setting back
}

// ---------------------------------------------------------------------------
// A picture seen from a camera turned differently (the steady monitor view)
// ---------------------------------------------------------------------------
// For each pixel of the target: the direction it looks in from the new
// camera, turned into the picture's camera, and where that lands in the
// picture. Exact for a camera that turns without moving, which is the case
// here (the same eye, looking slightly another way).

static GLuint g_turnedProgram = 0;
static GLuint g_turnedVertexArray = 0;
static GLint g_turnedRowsLocation = -1, g_turnedHalvesLocation = -1;
static bool g_turnedFailed = false;

static bool EnsureTurnedViewProgram()
{
    if (g_turnedProgram != 0) return true;
    if (g_turnedFailed) return false;

    // Three corner points that together cover the whole target (as for the overlay).
    const char* vertexSource =
        "#version 150\n"
        "out vec2 uv;\n"
        "void main() {\n"
        "    vec2 corner = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);\n"
        "    uv = corner * 0.5 + 0.5;\n"
        "    gl_Position = vec4(corner, 0.0, 1.0);\n"
        "}\n";
    const char* fragmentSource =
        "#version 150\n"
        "uniform sampler2D picture;\n"
        "uniform vec4 turnRows[3];\n" // the turn, row after row (x, y, z used)
        "uniform vec4 halves;\n"      // the picture's half view (across, up), then the new camera's
        "in vec2 uv;\n"
        "out vec4 colour;\n"
        "void main() {\n"
        "    vec3 ray = vec3((uv.x * 2.0 - 1.0) * halves.z, (uv.y * 2.0 - 1.0) * halves.w, -1.0);\n"
        "    vec3 seen = vec3(dot(turnRows[0].xyz, ray), dot(turnRows[1].xyz, ray), dot(turnRows[2].xyz, ray));\n"
        "    if (seen.z > -0.0001) { colour = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
        "    vec2 spot = (seen.xy / -seen.z) / halves.xy * 0.5 + 0.5;\n"
        "    if (spot.x < 0.0 || spot.x > 1.0 || spot.y < 0.0 || spot.y > 1.0) { colour = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
        "    colour = vec4(texture(picture, spot).rgb, 1.0);\n"
        "}\n";

    GLuint program = BuildProgram(vertexSource, fragmentSource, "steady view");
    if (program == 0)
    {
        g_turnedFailed = true;
        return false;
    }

    glxUseProgram(program);
    glxUniform1i(glxGetUniformLocation(program, "picture"), 0);
    g_turnedRowsLocation = glxGetUniformLocation(program, "turnRows");
    g_turnedHalvesLocation = glxGetUniformLocation(program, "halves");
    glxGenVertexArrays(1, &g_turnedVertexArray);
    g_turnedProgram = program;
    return true;
}

bool GlDrawTurnedView(GLuint texture, GLuint targetFramebuffer, int width, int height, const float turn[9],
                      const float pictureHalf[2], const float viewHalf[2])
{
    if (!g_loaded || glxUniform4fv == nullptr || !EnsureTurnedViewProgram()) return false;

    glxBindFramebuffer(GLX_DRAW_FRAMEBUFFER, targetFramebuffer);
    glViewport(0, 0, width, height);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GLX_FRAMEBUFFER_SRGB);
    glDisable(GL_BLEND); // every pixel is written, solid
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    const float rows[12] = { turn[0], turn[1], turn[2], 0.0f,
                             turn[3], turn[4], turn[5], 0.0f,
                             turn[6], turn[7], turn[8], 0.0f };
    const float halves[4] = { pictureHalf[0], pictureHalf[1], viewHalf[0], viewHalf[1] };

    glxUseProgram(g_turnedProgram);
    glxUniform4fv(g_turnedRowsLocation, 3, rows);
    glxUniform4fv(g_turnedHalvesLocation, 1, halves);
    glxBindVertexArray(g_turnedVertexArray);
    glxActiveTexture(GLX_TEXTURE0);
    glxBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, texture);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    return true; // the caller's GlStateGuard puts every setting back
}

// ---------------------------------------------------------------------------
// A flat picture standing somewhere in the view (a panel seen from the eye)
// ---------------------------------------------------------------------------

static GLuint g_panelProgram = 0;
static GLuint g_panelVertexArray = 0;
static GLint g_panelCornersLocation = -1;
static GLint g_panelPartLocation = -1;
static bool g_panelFailed = false;

static bool EnsurePanelProgram()
{
    if (g_panelProgram != 0) return true;
    if (g_panelFailed) return false;

    // Two triangles between the four corners, which arrive already seen from
    // the eye (with their distance in w, so the picture is drawn in perspective).
    const char* vertexSource =
        "#version 150\n"
        "uniform vec4 corners[4];\n" // bottom left, bottom right, top left, top right
        "uniform vec4 part;\n" // the part of the picture shown: its bottom left (xy) and top right (zw), 0 to 1
        "out vec2 uv;\n"
        "void main() {\n"
        "    int order[6] = int[6](0, 1, 3, 0, 3, 2);\n"
        "    int c = order[gl_VertexID];\n"
        "    uv = mix(part.xy, part.zw, vec2((c == 1 || c == 3) ? 1.0 : 0.0, c >= 2 ? 1.0 : 0.0));\n"
        "    gl_Position = corners[c];\n"
        "}\n";
    const char* fragmentSource =
        "#version 150\n"
        "uniform sampler2D picture;\n"
        "in vec2 uv;\n"
        "out vec4 colour;\n"
        "void main() { colour = texture(picture, uv); }\n";

    GLuint program = BuildProgram(vertexSource, fragmentSource, "panel");
    if (program == 0)
    {
        g_panelFailed = true;
        return false;
    }

    glxUseProgram(program);
    glxUniform1i(glxGetUniformLocation(program, "picture"), 0);
    g_panelCornersLocation = glxGetUniformLocation(program, "corners");
    g_panelPartLocation = glxGetUniformLocation(program, "part");
    glxGenVertexArrays(1, &g_panelVertexArray);
    g_panelProgram = program;
    return true;
}

bool GlDrawPanel(GLuint texture, GLuint targetFramebuffer, int width, int height, const float corners[16])
{
    const float whole[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    return GlDrawPanelPart(texture, targetFramebuffer, width, height, corners, whole);
}

bool GlDrawPanelPart(GLuint texture, GLuint targetFramebuffer, int width, int height, const float corners[16], const float part[4])
{
    if (!g_loaded || glxUniform4fv == nullptr || glxUniform4f == nullptr || !EnsurePanelProgram()) return false;

    glxBindFramebuffer(GLX_DRAW_FRAMEBUFFER, targetFramebuffer);
    glViewport(0, 0, width, height);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GLX_FRAMEBUFFER_SRGB);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // Colors already multiplied by their alpha, as in GlDrawOver.
    glEnable(GL_BLEND);
    glxBlendEquationSeparate(GLX_FUNC_ADD, GLX_FUNC_ADD);
    glxBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glxUseProgram(g_panelProgram);
    glxUniform4fv(g_panelCornersLocation, 4, corners);
    glxUniform4f(g_panelPartLocation, part[0], part[1], part[2], part[3]);
    glxBindVertexArray(g_panelVertexArray);
    glxActiveTexture(GLX_TEXTURE0);
    glxBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, texture);

    glDrawArrays(GL_TRIANGLES, 0, 6);
    return true; // the caller's GlStateGuard puts every setting back
}

bool GlDrawOverRect(GLuint texture, GLuint targetFramebuffer, int x, int y, int width, int height)
{
    if (!g_loaded || !EnsureOverlayProgram() || width <= 0 || height <= 0) return false;

    glxBindFramebuffer(GLX_DRAW_FRAMEBUFFER, targetFramebuffer);
    // The same drawing as GlDrawOver, squeezed into the given part of the target.
    glViewport(x, y, width, height);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GLX_FRAMEBUFFER_SRGB);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glEnable(GL_BLEND);
    glxBlendEquationSeparate(GLX_FUNC_ADD, GLX_FUNC_ADD);
    glxBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glxUseProgram(g_overlayProgram);
    glxBindVertexArray(g_overlayVertexArray);
    glxActiveTexture(GLX_TEXTURE0);
    glxBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, texture);

    glDrawArrays(GL_TRIANGLES, 0, 3);
    return true; // the caller's GlStateGuard puts every setting back
}
