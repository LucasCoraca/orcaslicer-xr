// Between the XR side (xr_glue.cpp: OpenXR, the XR core, the preview; plain OpenGL ES) and OrcaSlicer (orca_xr.cpp: Qt,
// wxWidgets, Orca's 3D view through its own GL loader). Neither includes the other's headers.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "xmath.h"

namespace oxr {

/** What the XR side asks of OrcaSlicer. Everything is called on Qt's GUI thread (Orca's), with Orca's GL context. */
struct AppHost {
    std::function<bool()> makeCurrent;                        // a GL context of Orca's current (the one its canvases share with)
    std::function<bool(V3* min, V3* max)> bounds;             // what stands on the table: the current plate (mm)
    std::function<float(V3 o, V3 d)> hit;                     // how far (mm) a ray in the scene goes to what it hits; < 0: nothing
    /** The 3D scene for one eye (0 left, 1 right), drawn in the GL context of the canvas shown (Orca's 3D view or its
     *  preview: each has its own, sharing objects with ours) into an image of Orca's own: a w x h RGBA texture (rows from
     *  the bottom, transparent where nothing is), multisampled with `samples` and resolved. `ready`: a GLsync Orca's
     *  context waits for before drawing (our last use of that eye's image), or null. View (rigid, mm) and projection
     *  column-major. Returns the texture (0: nothing) and in *sync a GLsync of the drawing; leaves that canvas' context
     *  current. The eyes' images never cross contexts while being drawn into: only finished textures are shared. */
    std::function<unsigned(const float view[16], const float proj[16], int w, int h, float nearMm, float farMm,
                           int eye, int samples, void* ready, void** sync)> drawScene;
    /** The same, for the preview: drawn into an image of its own and read back (RGBA, rows from the bottom), for a
     *  context that shares nothing with Orca's (the Android emulator's GL does not cope with sharing). */
    std::function<bool(const float view[16], const float proj[16], int w, int h, float nearMm, float farMm, std::vector<unsigned char>& rgba)> sceneImage;
    /** The ray in the room as the 3D view's mouse: 0 press 1 drag 2 release 3 hover 4 left the scene 5 right click 6 cancel. */
    std::function<void(int action, V3 o, V3 d)> pointer;
    /** The ray on the window (pixels of the virtual display): 0 press 1 drag 2 release 3 hover 4 leave 5 wheel (amount)
     *  6 right click. */
    std::function<void(int action, float x, float y, float amount)> panelMouse;
    /** The on-screen keyboard: shown while a text field has focus (its image, RGBA w x h, changes with shift); a key at
     *  (u, v) of it pressed. */
    std::function<bool(const unsigned char** rgba, int* w, int* h, unsigned* version)> keyboard;
    std::function<void(float u, float v)> keyboardPress;
};

/** Called by OrcaSlicer once its main window and 3D view are up (GUI thread, GL current): starts the headset, or the
 *  preview without one. */
void begin(const AppHost& host);
/** One frame of the headset or the preview; returns how long to wait before the next one (ms). */
int frame();

/** OrcaSlicer's side: wait for the app, then call begin() and keep calling frame() (orca_xr.cpp). */
void startApp();
/** Run a function on Qt's GUI thread (orca_xr.cpp). */
void postToGui(std::function<void()> f);

}  // namespace oxr
