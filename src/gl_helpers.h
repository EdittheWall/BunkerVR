#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>

// Small OpenGL helpers. Windows only ships declarations for very old OpenGL,
// so the newer functions we need are looked up by name at run time.
//
// "Framebuffer" below means a drawing target. Framebuffer 0 is the game's
// window; the others are off-screen pictures.

// Looks up the newer OpenGL functions. Needs the game's OpenGL context to be
// active. Returns false if any are missing.
bool GlLoadFunctions();

// Takes the OpenGL errors waiting to be read (glGetError), at most 32: returns
// how many there were, with the first one's code in `first` (0 for none).
// SteamVR's OpenXR runtime reads them after its own OpenGL calls and fails on
// any it finds, even one left over from earlier. Needs an OpenGL context.
int GlTakeErrors(unsigned& first);

// No pixel buffer bound for reading or writing pictures, so that pictures the
// headset software makes are made empty (a bound one would be read from). The
// game's are put back afterwards by the frame's GlStateGuard.
void GlUnbindPixelBuffers();

// Size in pixels of the window the game is drawing into.
bool GlGetWindowSize(int& width, int& height);

// The size of the picture `framebuffer` draws into: the window's, unless the
// game's own picture is smaller (its "Resolution scale" below 100%), in which
// case that. 0 = the window.
bool GlGetPictureSize(GLuint framebuffer, int& width, int& height);

// Timing the graphics card's work: marks put in its stream of work each frame
// (mark 0 starts a frame), read back some frames later without waiting.
// GlTimerTakeAverages gives, for each mark, the average milliseconds from it
// to the next mark the same frame had (or to the next frame's start), and in
// the last place the whole frame's; -1 where there were none. It starts the
// averages afresh. Returns false if nothing was measured.
static const int kGlTimerMarks = 8;
void GlTimerMark(int mark);
bool GlTimerTakeAverages(double averages[kGlTimerMarks + 1]);

// Creates an empty off-screen picture (texture) of the given size, and a
// framebuffer that draws into it. Leaves the game's own settings untouched.
bool GlCreateTarget(int width, int height, GLuint& texture, GLuint& framebuffer);
void GlDeleteTarget(GLuint& texture, GLuint& framebuffer);

// Creates an empty picture (texture), width x height pixels in `format` (a sized pixel format, such as
// kGlSrgb8Alpha8), at its full size alone, read smoothly: as SteamVR makes the pictures of its sets. 0 if it could not
// be made. Used for the mod's own sets of pictures on SteamVR (KnownSet in vr.cpp); deleted with glDeleteTextures.
// Leaves the game's own settings untouched.
GLuint GlCreatePicture(int width, int height, GLint format);

// Which framebuffer is currently being drawn into / read from.
GLuint GlGetDrawFramebuffer();
GLuint GlGetReadFramebuffer();
void GlSetDrawFramebuffer(GLuint framebuffer);
void GlSetReadFramebuffer(GLuint framebuffer);

// Wipes the framebuffer currently being drawn into to fully see-through.
// Leaves the game's own settings untouched.
void GlClearTransparent();

// Copies a rectangle from one framebuffer to a rectangle of another,
// stretching if they differ in size. Rectangles are left, bottom, right, top
// in pixels, counted from the bottom-left corner.
void GlCopy(GLuint sourceFramebuffer, int sourceLeft, int sourceBottom, int sourceRight, int sourceTop,
            GLuint targetFramebuffer, int targetLeft, int targetBottom, int targetRight, int targetTop);

// Copies a rectangle of a framebuffer into `texture`, stretched to fill the
// whole texture, which is `textureWidth` x `textureHeight` pixels.
void GlCopyToTexture(GLuint sourceFramebuffer, int sourceLeft, int sourceBottom, int sourceRight, int sourceTop,
                     GLuint texture, int textureWidth, int textureHeight);

// Remakes the smaller copies (mipmaps) of `texture` from its full-size
// picture (glGenerateMipmap; nothing if the driver lacks it). The texture
// must have room for them. Leaves the game's own settings untouched.
void GlMakeSmallerCopies(GLuint texture);

// Fills `texture` (width x height pixels) from memory: 4 bytes per pixel (red,
// green, blue, alpha), top row first. Leaves the game's own settings untouched.
void GlUploadPicture(GLuint texture, int width, int height, const unsigned char* rgbaTopFirst);

// Draws `texture` over the whole of `targetFramebuffer` (width x height
// pixels), letting what is already there show through its see-through parts.
// Returns false if it could not be drawn.
bool GlDrawOver(GLuint texture, GLuint targetFramebuffer, int width, int height);
// The same, drawn into one part of the target only (x and y from the bottom left).
bool GlDrawOverRect(GLuint texture, GLuint targetFramebuffer, int x, int y, int width, int height);
// The same, drawn as a flat panel standing in the view: `corners` are its
// bottom-left, bottom-right, top-left and top-right corners as seen from the
// eye, 4 numbers each (OpenGL "clip" positions: x, y, depth, distance), so
// the picture is drawn in perspective and cut off where it passes the eye.
bool GlDrawPanel(GLuint texture, GLuint targetFramebuffer, int width, int height, const float corners[16]);
// The same with only a part of the picture on the panel (used for the
// interface's pieces): `part` is its bottom-left and top-right corners in the
// picture, 0 to 1 (u, v, u, v; v from the bottom, as OpenGL has it).
bool GlDrawPanelPart(GLuint texture, GLuint targetFramebuffer, int width, int height, const float corners[16], const float part[4]);

// The panels drawn into the eyes' pictures (for SteamVR: PanelsIntoEyes in
// vr.cpp). `targetTexture` is an eye's picture (width x height pixels).
// Fills it with one color:
bool GlFillTexture(GLuint targetTexture, int width, int height, const float rgba[4]);
// Draws a panel into it as a headset's compositor would: `corners` and `part`
// as for GlDrawPanelPart; each pixel is the mean of four samples over the area
// it covers, mixed in linear light; with `blend` it is laid over with its
// colors already multiplied by their alpha, otherwise it is drawn solid.
bool GlDrawPanelIntoTexture(GLuint targetTexture, int width, int height, GLuint texture, const float corners[16],
                            const float part[4], bool blend);
// Draws into it `texture`, an eye's picture taken a moment ago, as the eye now
// sees it after turning: `turn` (9 numbers, row after row) takes a direction
// seen by the eye now to the same direction as the eye saw it then; the
// picture's edges and the eye's now are tangents: left, down, right, up.
// What the picture does not cover is black.
bool GlDrawEyeSeenAgain(GLuint targetTexture, int width, int height, GLuint texture, const float turn[9],
                        const float pictureEdges[4], const float viewEdges[4]);

// Draws over the whole of `targetFramebuffer` (width x height pixels) what a
// camera turned differently would see of `texture`, a picture taken by a
// camera looking along its -z (x right, y up) whose view reaches `pictureHalf`
// (tangents: across, up and down) to each side. The new camera's view reaches
// `viewHalf` to each side, and `turn` (9 numbers, row after row) takes a
// direction seen from the new camera to the same direction seen from the
// picture's camera. Whatever the picture does not cover is black. Used for the
// monitor's steady view of the left eye's picture (see ComposeMonitorPicture).
bool GlDrawTurnedView(GLuint texture, GLuint targetFramebuffer, int width, int height, const float turn[9],
                      const float pictureHalf[2], const float viewHalf[2]);

// Loads a picture file of the game's (.dds, kinds DXT1, DXT3 and DXT5) as a
// texture. Returns 0 if the file is missing or of another kind.
GLuint GlLoadDdsTexture(const char* path);

// Draws `texture` over the whole of `targetFramebuffer` (width x height
// pixels) as a screen effect such as blood on the face.
//   uvScale, uvOffset: which part of the texture lands where. For a point of
//               the target at (x, y), each running from -1 at the left/bottom
//               edge to +1 at the right/top edge, the texture is read at
//               (x * uvScale[0] + uvOffset[0], y * uvScale[1] + uvOffset[1]),
//               where (0, 0) is the texture's top-left corner and (1, 1) its
//               bottom-right. Outside that range its edge is repeated.
//   strength:   0 (nothing) to 1 (the texture's full strength)
//   multiply:   false = laid over what is there; true = what is there is
//               darkened by the texture (for dark edges)
//   tint:       red, green, blue (0 to 1) that the texture's color is
//               multiplied by; null = leave the color as it is
// Returns false if it could not be drawn.
bool GlDrawScreenEffect(GLuint texture, GLuint targetFramebuffer, int width, int height, const float uvScale[2],
                        const float uvOffset[2], float strength, bool multiply, const float* tint = nullptr);

// The same, for glass in front of the eyes: what is there is darkened by the
// texture as the game's own shader does it, its alpha counting too.
bool GlDrawScreenGlass(GLuint texture, GLuint targetFramebuffer, int width, int height, const float uvScale[2],
                       const float uvOffset[2], float strength);

// Lays one flat color over the whole of `targetFramebuffer`, `strength` solid
// (0 = nothing, 1 = only the color is left). Used for fades to black.
bool GlDrawSolid(GLuint targetFramebuffer, int width, int height, const float colour[3], float strength);

// Darkens the edges of `targetFramebuffer`, an eye's picture that is
// tanHalfWidth by tanHalfHeight (as tangents of the angles from straight
// ahead, its middle straight ahead): clear within `clearWithin` of the
// middle, black beyond `blackBeyond` (both as tangents, so the ring is round
// to the eye), a smooth step between; all of it as solid as `strength` (0 to
// 1). Used for the comfort ring (vignette). Returns false if it could not be drawn.
bool GlDrawRing(GLuint targetFramebuffer, int width, int height, float tanHalfWidth, float tanHalfHeight, float clearWithin,
                float blackBeyond, float strength);

// Draws the edge shown when the player is hit into `targetFramebuffer`: the view's edges reddened, as strong as
// `strength` (0 to 1) on the hit's side (sideX, sideY: a unit direction on the picture, x right, y up; 0, 0 for all
// round) and a quarter of that on the far side, with the whole view slightly darkened. The picture's half width and
// height are tangents, as for GlDrawRing.
bool GlDrawHurtEdge(GLuint targetFramebuffer, int width, int height, float tanHalfWidth, float tanHalfHeight, float sideX,
                    float sideY, float strength);

// Draws solid boxes into `targetFramebuffer` (width x height pixels), on top of
// whatever is there. Used for the simple hand shapes.
//   projection: 16 numbers, row after row: how 3D is flattened for this picture
//   boxes:      for each box, 16 numbers, row after row: where the box is as
//               seen from the eye, with its size included (a box spans -0.5 to
//               +0.5 along each of its own axes before this is applied)
//   colours:    for each box, red, green, blue (0 to 1)
// Returns false if it could not be drawn.
bool GlDrawBoxes(GLuint targetFramebuffer, int width, int height, const float* projection,
                 const float* boxes, const float* colours, int boxCount);

// Reads the picture the game has just finished drawing into `bgra`, which must
// hold width * height * 4 bytes. Rows come bottom to top; each pixel is blue,
// green, red, alpha. Returns false if it could not be read.
// `sourceFramebuffer`: where the picture is (0 = the window).
bool GlReadGameImage(unsigned char* bgra, int width, int height, GLuint sourceFramebuffer = 0);

// The game keeps track of which OpenGL settings it has switched on. If we or
// the VR software change one behind its back, its drawing goes wrong. This
// remembers the settings when created and puts them back with Restore().
struct GlStateGuard
{
    GLint readFramebuffer, drawFramebuffer;
    GLint activeTexture, texture2D;   // the selected texture slot and what is in it
    GLint slot0Texture2D, slot0Sampler; // what is in texture slot 0
    GLint program, vertexArray, arrayBuffer;
    GLint packBuffer, unpackBuffer;
    GLint viewport[4];
    GLboolean scissorTest, framebufferSrgb, depthTest, stencilTest, cullFace, blend;
    GLint blendSourceColour, blendTargetColour, blendSourceAlpha, blendTargetAlpha;
    GLint blendEquationColour, blendEquationAlpha;
    GLboolean colourMask[4];

    void Capture();
    // Puts everything back. Returns true if something had been changed.
    bool Restore();
};

// The pixel format codes we care about.
const GLint kGlSrgb8Alpha8 = 0x8C43; // sRGB color, the same encoding the game's window uses
const GLint kGlRgba8 = 0x8058;
