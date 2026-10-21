// The XR side of OrcaSlicer XR on Android. It runs on Qt's GUI thread (OrcaSlicer's), in the GL context of Orca's 3D
// view, so Orca can draw its scene straight into the eyes' images: the headset loop (OpenXR, xr_session), or without an
// OpenXR runtime (phone, emulator) the same scene drawn onto a SurfaceView of the activity as a touch preview. Orca's
// window arrives as a SurfaceTexture (its virtual display, see XrActivity.java) shown in front of the user.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "gl.h"
#include "interaction.h"
#include "renderer.h"
#include "xr_host.h"
#include "xr_session.h"
#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65   // GL_OES_EGL_image_external (SurfaceTexture images)
#endif
#ifndef GL_FRAMEBUFFER_SRGB_EXT
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9   // GL_EXT_sRGB_write_control
#endif

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "orcaxr", __VA_ARGS__)

namespace oxr {
namespace {
JavaVM* gVm = nullptr; jobject gActivity = nullptr;
int gPanelW = 3200, gPanelH = 2000; float gPxPerDp = 2.f;
AppHost gHost; Interaction gIa; Renderer gRenderer; Headset gHs;
bool gBegun = false, gXr = false, gPlaceModel = true, gPlacePanel = true, gSrgbControl = false;
EGLDisplay gDpy = EGL_NO_DISPLAY; EGLContext gCtx = EGL_NO_CONTEXT; EGLConfig gCfg = nullptr;   // ours (shares with Orca's)
EGLSurface gPbuffer = EGL_NO_SURFACE;
float gHoverX = 0, gHoverY = 0;                 // where the ray is on the window (for the wheel)
unsigned gKeysVersion = ~0u;

JNIEnv* env() {
    JNIEnv* e = nullptr;
    if (gVm->GetEnv((void**)&e, JNI_VERSION_1_6) != JNI_OK) gVm->AttachCurrentThread(&e, nullptr);
    return e;
}
jmethodID method(JNIEnv* e, const char* name, const char* sig) {
    jclass c = e->GetObjectClass(gActivity); jmethodID m = e->GetMethodID(c, name, sig); e->DeleteLocalRef(c); return m;
}
void report(bool ok, const std::string& msg) {
    JNIEnv* e = env(); jstring s = e->NewStringUTF(msg.c_str());
    e->CallVoidMethod(gActivity, method(e, "onXrResult", "(ZLjava/lang/String;)V"), (jboolean)ok, s); e->DeleteLocalRef(s);
}

/** Orca's window, about 1.2 m wide, straight ahead a little over a metre away, tilted back a little; the keyboard (when
 *  a text field has focus) under it, closer, tilted up like a real one. */
void arrangePanel() {
    const float width = 1.2f, ppm = gPanelW / width, D = 1.15f;
    Sheet s; s.x = 0; s.y = 0; s.w = (float)gPanelW; s.h = (float)gPanelH; s.pxPerM = ppm;
    s.local = s.home = Pose{V3{0, 0.04f, -D}, axisAngle({1, 0, 0}, -0.08f), 1};
    gIa.panel.sheets = {s};
    gIa.panel.seeThrough = false; gIa.panel.badgeSheet = 0;
    // like a keyboard on the desk, between you and the plate
    gIa.keyboard.local = Pose{V3{0, -0.36f, -0.5f}, axisAngle({1, 0, 0}, -0.65f), 1};
}

/** The window's texture: Java attaches Orca's virtual display to it. */
void createPanel() {
    GLuint tex = 0; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    gIa.panel.tex = tex; gIa.panel.external = true; gIa.panel.pxW = gPanelW; gIa.panel.pxH = gPanelH;
    gIa.panel.width = 1.2f; gIa.panel.height = 1.2f * gPanelH / gPanelW;
    gIa.panel.visible = true;
    arrangePanel();
    JNIEnv* e = env(); e->CallVoidMethod(gActivity, method(e, "attachPanel", "(I)V"), (jint)tex);
}

void writePpm(const std::string& path, const std::vector<uint8_t>& px, int w, int h) {
    FILE* f = fopen(path.c_str(), "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; --y) for (int x = 0; x < w; ++x) fwrite(&px[((size_t)y * w + x) * 4], 1, 3, f);
    fclose(f);
}
bool gDumpPreview = false;

/** Development: `adb shell run-as org.orcaport.slicer.xr touch files/dump_panel` writes the window's image (half size)
 *  to files/panel.ppm (the virtual display is private: screencap cannot see it). */
void dumpPanelIfAsked() {
    const char* home = getenv("HOME"); if (!home) return;
    std::string flag = std::string(home) + "/dump_panel";
    if (access(flag.c_str(), F_OK) != 0) return;
    unlink(flag.c_str());
    int w = gPanelW / 2, h = gPanelH / 2; std::vector<uint8_t> px;
    gRenderer.readImage(gIa.panel.tex, true, w, h, px);
    if (px.size() != (size_t)w * h * 4) return;
    FILE* f = fopen((std::string(home) + "/panel.ppm").c_str(), "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; --y) for (int x = 0; x < w; ++x) fwrite(&px[((size_t)y * w + x) * 4], 1, 3, f);
    fclose(f); LOG("panel image written"); gDumpPreview = true;
    if (gIa.panel.sharp) {
        gRenderer.readImage(gIa.panel.sharp, false, w, h, px);
        FILE* g = fopen((std::string(home) + "/sharp.ppm").c_str(), "wb"); if (!g) return;
        fprintf(g, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; --y) for (int x = 0; x < w; ++x) fwrite(&px[((size_t)y * w + x) * 4], 1, 3, g);
        fclose(g);
    }
}

/** Development: `adb shell run-as org.orcaport.slicer.xr sh -c 'echo X Y > files/click'` clicks the window at pixel (X, Y)
 *  (`echo X Y r` right-clicks, `echo U V k` presses the keyboard's key at (U, V) of its image), for scripted tests. */
void clickIfAsked() {
    static int release = 0; static float rx = 0, ry = 0;
    if (release && --release == 0) { if (gHost.panelMouse) gHost.panelMouse(2, rx, ry, 0); return; }
    const char* home = getenv("HOME"); if (!home) return;
    std::string path = std::string(home) + "/click";
    FILE* f = fopen(path.c_str(), "r"); if (!f) return;
    float x = 0, y = 0; char r[4] = {0};
    int n = fscanf(f, "%f %f %3s", &x, &y, r); fclose(f); unlink(path.c_str());
    if (n < 2 || !gHost.panelMouse) return;
    if (r[0] == 'k') { if (gHost.keyboardPress) gHost.keyboardPress(x, y); return; }
    gHost.panelMouse(3, x, y, 0);
    if (r[0] == 'r') { gHost.panelMouse(6, x, y, 0); return; }
    gHost.panelMouse(0, x, y, 0); rx = x; ry = y; release = 2;
}

void updatePanel() {
    clickIfAsked();
    JNIEnv* e = env();
    if (e->CallBooleanMethod(gActivity, method(e, "updatePanel", "()Z"))) gIa.panelChanged(gRenderer);
    dumpPanelIfAsked();
    // the keyboard follows the text focus
    const unsigned char* px = nullptr; int w = 0, h = 0; unsigned ver = 0;
    bool show = gHost.keyboard && gHost.keyboard(&px, &w, &h, &ver);
    if (show && px && ver != gKeysVersion) {
        gIa.keyboard.tex = gRenderer.makeTexture(w, h, px, gIa.keyboard.tex);
        gIa.keyboard.w = 0.42f; gIa.keyboard.h = 0.42f * h / std::max(w, 1); gKeysVersion = ver;
    }
    gIa.keyboard.visible = show && gIa.keyboard.tex;
}

/** The controller models (WebXR input profiles, MIT) from the APK's assets. */
void loadModels() {
    JNIEnv* e = env();
    jobject am = e->CallObjectMethod(gActivity, method(e, "getAssets", "()Landroid/content/res/AssetManager;"));
    AAssetManager* mgr = am ? AAssetManager_fromJava(e, am) : nullptr;
    for (const char* n : {"touch-plus-left", "touch-plus-right"}) {
        if (!mgr) break;
        AAsset* a = AAssetManager_open(mgr, (std::string("models/") + n + ".glb").c_str(), AASSET_MODE_BUFFER);
        if (!a) continue;
        const uint8_t* b = (const uint8_t*)AAsset_getBuffer(a);
        std::vector<uint8_t> glb(b, b + AAsset_getLength(a)); AAsset_close(a);
        gIa.setModel(n, glb);
    }
    if (am) e->DeleteLocalRef(am);
}

/** Orca draws its scene for the eye in its canvas' own GL context, and we draw that into the eye's image; then ours is
 *  current again, with the eye's framebuffer, its depth cleared (it is the scene's, which our drawing need not test against) and GL state as we use it. */
GLuint gSceneTex = 0; std::vector<unsigned char> gScenePx;
void drawHostScene(const M4& view, const M4& proj, const EyeView& eye, float nearMm, float farMm) {
    if (!gXr) {
        // the preview: Orca's image of the scene, behind everything else
        if (!gHost.sceneImage) return;
        EGLSurface draw = eglGetCurrentSurface(EGL_DRAW), read = eglGetCurrentSurface(EGL_READ);
        bool ok = gHost.sceneImage(view.m, proj.m, eye.w, eye.h, nearMm, farMm, gScenePx);
        eglMakeCurrent(gDpy, draw, read, gCtx);
        if (!ok) return;
        gSceneTex = gRenderer.makeTexture(eye.w, eye.h, gScenePx.data(), gSceneTex);
        glBindFramebuffer(GL_FRAMEBUFFER, eye.fbo); glViewport(0, 0, eye.w, eye.h); glDisable(GL_DEPTH_TEST);
        M4 ortho{}; ortho.m[0] = 2; ortho.m[5] = -2; ortho.m[10] = 1; ortho.m[15] = 1;    // (makeTexture: row 0 at the top)
        gRenderer.drawPanel(gSceneTex, false, Pose{V3{}, Q{}, 1}, 1, 1, 0, ortho, 1, nullptr, true);
        glEnable(GL_DEPTH_TEST); glClear(GL_DEPTH_BUFFER_BIT);
        return;
    }
    // the headset: Orca draws the eye into an image of its own (after our last use of it), then we draw that image into
    // the eye's, over what is there so far (the floor)
    static GLsync used[2] = {nullptr, nullptr};
    int e = eye.index & 1;
    EGLSurface draw = eglGetCurrentSurface(EGL_DRAW), read = eglGetCurrentSurface(EGL_READ);
    glFlush();
    void* sync = nullptr; unsigned tex = 0;
    if (gHost.drawScene) tex = gHost.drawScene(view.m, proj.m, eye.w, eye.h, nearMm, farMm, e, eye.samples, used[e], &sync);
    else if (used[e]) glDeleteSync(used[e]);
    used[e] = nullptr;
    eglMakeCurrent(gDpy, draw, read, gCtx);
    if (sync) { glWaitSync((GLsync)sync, 0, GL_TIMEOUT_IGNORED); glDeleteSync((GLsync)sync); }
    glBindFramebuffer(GL_FRAMEBUFFER, eye.fbo); glViewport(0, 0, eye.w, eye.h);
    glBindVertexArray(0); glUseProgram(0); glActiveTexture(GL_TEXTURE0);
    glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST); glDisable(GL_POLYGON_OFFSET_FILL); glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (tex) {
        glDisable(GL_DEPTH_TEST);
        std::vector<float> holes; holes.swap(gRenderer.holes);
        M4 ortho{}; ortho.m[0] = 2; ortho.m[5] = -2; ortho.m[10] = 1; ortho.m[15] = 1;     // (rows from the bottom, as the preview's)
        gRenderer.drawPanel(tex, false, Pose{V3{}, Q{}, 1}, 1, 1, 0, ortho, 1, nullptr, true);
        holes.swap(gRenderer.holes);
        if (!gRenderer.regenerateCopies) { used[e] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0); glFlush(); }
    }
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_TRUE); glDisable(GL_BLEND);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void hookCallbacks() {
    gIa.host.on = true;
    gIa.host.bounds = [](V3* a, V3* b) { return gHost.bounds && gHost.bounds(a, b); };
    gIa.host.hit = [](V3 o, V3 d) { return gHost.hit ? gHost.hit(o, d) : -1.f; };
    gIa.host.draw = drawHostScene;
    gIa.plate = false;                                         // Orca draws its own plate
    gIa.hostSelection = true;
    gIa.onPanelPointer = [](int action, float x, float y) {
        if (!gHost.panelMouse) return;
        if (action == PanelScroll) { gHost.panelMouse(5, gHoverX, gHoverY, y); return; }
        gHoverX = x; gHoverY = y;
        gHost.panelMouse(action, x, y, 0);
    };
    gIa.onMenu = [] { gPlaceModel = true; gPlacePanel = true; };     // A / X: the window and the plate back in front
    // the room is Orca's 3D view: the trigger there is its mouse (pick, select, drag, the gizmos), B / Y its right click
    gIa.onViewport = [](int action, V3 o, V3 d, float) { if (gHost.pointer) gHost.pointer(action == 5 ? 6 : action, o, d); };
    gIa.onContextMenu = [](V3 o, V3 d) { if (gHost.pointer) gHost.pointer(5, o, d); };
    gIa.onKeyboard = [](float u, float v) { if (gHost.keyboardPress) gHost.keyboardPress(u, v); };
}

// ---- headset-free preview: you stand at the origin; one finger is the controller with its trigger pressed (on the
// window and in the room alike), two fingers look around
ANativeWindow* gPreviewWin = nullptr; int gPvW = 1, gPvH = 1; std::vector<uint8_t> gPvPixels;
std::mutex gPvLock;
struct Preview {
    float yaw = 0, pitch = -0.16f, fov = 0.62f;
    int mode = 0;                 // 0 idle, 1 trigger held, 2 looking around
    float lastX = 0, lastY = 0, tx = 0, ty = 0;
    bool pressSeen = false, releasePending = false;   // a quick tap is held for at least one frame
    bool buttons[7] = {};                             // B/Y, A/X, stick up / down / left / right, grip (held)
    int pressedFrames[7] = {};                        // a quick key press is held for a few frames
} gPv;
const Pose kHead{V3{0, 1.6f, 0}, Q{}, 1};
Pose previewEye() { return {kHead.pos, qnorm(axisAngle({0, 1, 0}, gPv.yaw) * axisAngle({1, 0, 0}, gPv.pitch)), 1}; }
Pose previewRay(float sx, float sy) {
    Pose eye = previewEye(); float aspect = (float)gPvW / std::max(gPvH, 1), fy = gPv.fov, fx = std::atan(std::tan(fy) * aspect);
    float nx = sx / gPvW * 2 - 1, ny = 1 - sy / gPvH * 2;
    V3 d = norm(rotate(eye.rot, V3{nx * std::tan(fx), ny * std::tan(fy), -1}));
    return {eye.pos, lookRot(d), 1};
}

GLuint gPvFbo = 0, gPvColor = 0, gPvDepth = 0; int gPvTexW = 0, gPvTexH = 0;
void previewTarget() {
    if (gPvFbo && gPvTexW == gPvW && gPvTexH == gPvH) return;
    if (!gPvFbo) { glGenFramebuffers(1, &gPvFbo); glGenTextures(1, &gPvColor); glGenRenderbuffers(1, &gPvDepth); }
    glBindTexture(GL_TEXTURE_2D, gPvColor);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, gPvW, gPvH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, gPvDepth); glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, gPvW, gPvH);
    glBindFramebuffer(GL_FRAMEBUFFER, gPvFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gPvColor, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, gPvDepth);
    gPvTexW = gPvW; gPvTexH = gPvH;
}

void previewFrame() {
    previewTarget();
    if (gPlacePanel) { gIa.placePanel(kHead); gPlacePanel = false; }
    if (gPlaceModel && gIa.hasScene()) { gIa.placeModel(kHead); gPlaceModel = false; }
    HandInput hands[2];
    hands[1].active = true; hands[1].aim = previewRay(gPv.tx, gPv.ty); hands[1].grip = hands[1].aim;
    bool press = gPv.mode == 1 || gPv.releasePending;
    if (press && gPv.pressSeen) gPv.releasePending = false, press = gPv.mode == 1;
    if (press) gPv.pressSeen = true;
    hands[1].select = press ? 1.f : 0.f;
    auto held = [](int k) { if (gPv.pressedFrames[k] > 0) { --gPv.pressedFrames[k]; return true; } return gPv.buttons[k]; };
    hands[1].context = held(0); hands[1].menu = held(1);
    hands[1].stickY = held(2) ? 1.f : held(3) ? -1.f : 0.f; hands[1].stickX = held(5) ? 1.f : held(4) ? -1.f : 0.f;
    hands[1].squeeze = held(6) ? 1.f : 0.f;
    gIa.showControllers = false;
    gIa.headPose = previewEye();
    gIa.update(hands, 1.f / 30.f);
    Pose eye = previewEye(); float aspect = (float)gPvW / std::max(gPvH, 1), fy = gPv.fov, fx = std::atan(std::tan(fy) * aspect);
    M4 vp = mul(projectionFov(-fx, fx, fy, -fy, 0.02f, 50.f), viewFrom(eye));
    gRenderer.gamma = 1.f; gRenderer.beginView(gPvFbo, 0, 0, gPvW, gPvH, true);
    gIa.eye = EyeView{eye, -fx, fx, fy, -fy, gPvW, gPvH, 0.02f, 50.f, gPvColor, gPvDepth, gPvFbo, 1};
    gIa.render(gRenderer, vp, eye.pos);
    if (gDumpPreview) {
        gDumpPreview = false; std::vector<uint8_t> px((size_t)gPvW * gPvH * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, gPvFbo); glReadPixels(0, 0, gPvW, gPvH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        if (const char* home = getenv("HOME")) writePpm(std::string(home) + "/preview.ppm", px, gPvW, gPvH);
    }
}
}  // namespace

/** The context the app had: put back after each frame, as it was. */
struct SavedContext {
    EGLDisplay dpy = eglGetCurrentDisplay(); EGLContext ctx = eglGetCurrentContext();
    EGLSurface draw = eglGetCurrentSurface(EGL_DRAW), read = eglGetCurrentSurface(EGL_READ);
    ~SavedContext() { if (dpy != EGL_NO_DISPLAY) eglMakeCurrent(dpy, draw, read, ctx); }
};

void begin(const AppHost& host) {
    if (gBegun) return;
    gHost = host;
    SavedContext saved;
    // our own context. For the headset it shares objects with Orca's (its 3D view's): Orca draws straight into the eyes'
    // images. Without one (the preview), it shares nothing and Orca's scene comes as an image (the Android emulator's GL
    // breaks with contexts shared between apps' threads of drawing).
    if (!gHost.makeCurrent || !gHost.makeCurrent()) { LOG("no GL context"); return; }
    gDpy = eglGetCurrentDisplay(); EGLContext share = eglGetCurrentContext();
    const EGLint want[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                           EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLint n = 0; eglChooseConfig(gDpy, want, &gCfg, 1, &n);
    const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    auto makeContext = [&](EGLContext with) {
        gCtx = n ? eglCreateContext(gDpy, gCfg, with, ctxAttrs) : EGL_NO_CONTEXT;
        if (gPbuffer == EGL_NO_SURFACE && gCtx) gPbuffer = eglCreatePbufferSurface(gDpy, gCfg, pb);
        return gCtx && eglMakeCurrent(gDpy, gPbuffer, gPbuffer, gCtx);
    };
    if (!makeContext(share)) { LOG("no shared GL context (0x%x)", eglGetError()); return; }
    PlatformInit p; p.vm = gVm; p.activity = gActivity; p.display = gDpy; p.config = gCfg; p.context = gCtx;
    std::string err;
    gXr = gHs.create(p, "OrcaSlicer XR", &err);
    if (!gXr) {
        eglMakeCurrent(gDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); eglDestroyContext(gDpy, gCtx);
        if (!makeContext(EGL_NO_CONTEXT)) { LOG("no GL context for the preview (0x%x)", eglGetError()); return; }
    }
    gBegun = true;
    const char* ext = (const char*)glGetString(GL_EXTENSIONS);
    gSrgbControl = ext && std::strstr(ext, "GL_EXT_sRGB_write_control");
    LOG("GL: %s, %s; sRGB write control %s", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION), gSrgbControl ? "yes" : "no");
    gRenderer.contextLost(); gRenderer.init(); hookCallbacks(); createPanel(); loadModels();
    gRenderer.regenerateCopies = std::strstr((const char*)glGetString(GL_RENDERER), "Emulator") != nullptr;
    if (gXr) {
        gHs.onPlaced = [](const Pose& head) { if (gPlacePanel) { gIa.placePanel(head); gPlacePanel = false; } };
        report(true, gHs.systemName);
    } else report(false, err);
}

int frame() {
    if (!gBegun) return 100;
    SavedContext saved;
    int wait = 0;
    if (gXr) {
        eglMakeCurrent(gDpy, gPbuffer, gPbuffer, gCtx);
        updatePanel();
        bool quit = false;
        gHs.pollEvents(&quit);
        if (quit) { JNIEnv* e = env(); e->CallVoidMethod(gActivity, method(e, "onXrExit", "()V")); gXr = false; return 100; }
        if (gPlaceModel) { gHs.needsPlacement = true; gPlaceModel = false; }
        if (gPlacePanel) gHs.needsPlacement = true;
        if (!gHs.running()) wait = 10;
        else { gIa.showControllers = true; gHs.frame(gIa, gRenderer); }
    } else {
        // the preview: drawn offscreen in our context, copied to the screen by the CPU (the emulator's GL does not cope
        // with a window surface used between other contexts' drawing)
        std::lock_guard<std::mutex> l(gPvLock);
        if (!gPreviewWin) wait = 50;
        else {
            eglMakeCurrent(gDpy, gPbuffer, gPbuffer, gCtx);
            updatePanel();
            previewFrame();
            std::vector<uint8_t>& px = gPvPixels; px.resize((size_t)gPvW * gPvH * 4);
            glBindFramebuffer(GL_FRAMEBUFFER, gPvFbo); glReadPixels(0, 0, gPvW, gPvH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            ANativeWindow_Buffer b;
            if (ANativeWindow_lock(gPreviewWin, &b, nullptr) == 0) {
                for (int y = 0; y < gPvH && y < b.height; ++y)
                    std::memcpy((uint8_t*)b.bits + (size_t)y * b.stride * 4, &px[(size_t)(gPvH - 1 - y) * gPvW * 4], (size_t)std::min(gPvW, b.width) * 4);
                ANativeWindow_unlockAndPost(gPreviewWin);
            }
            wait = 80;     // (a test tool: about 12 frames a second, easy on the emulator's software GPU)
        }
    }
    return wait;
}

}  // namespace oxr

using namespace oxr;

extern "C" {

JNIEXPORT void JNICALL Java_org_orcaport_xr_XrActivity_nativeCreate(JNIEnv* e, jclass, jobject activity, jint w, jint h, jfloat pxPerDp) {
    e->GetJavaVM(&gVm);
    if (gActivity) e->DeleteGlobalRef(gActivity);
    gActivity = e->NewGlobalRef(activity);
    gPanelW = w; gPanelH = h; gPxPerDp = pxPerDp;
    startApp();
}

JNIEXPORT void JNICALL Java_org_orcaport_xr_XrActivity_nativeDestroy(JNIEnv*, jclass) {
    postToGui([] { if (gXr) { gHs.destroy(); gXr = false; } });
}

/** The preview's surface (UI thread): made an EGL window surface of Orca's context on the GUI thread; gone (null) only
 *  once the GUI thread has let go of it. */
JNIEXPORT void JNICALL Java_org_orcaport_xr_XrActivity_nativePreviewSurface(JNIEnv* e, jclass, jobject surface, jint w, jint h) {
    ANativeWindow* win = surface ? ANativeWindow_fromSurface(e, surface) : nullptr;
    std::lock_guard<std::mutex> l(gPvLock);
    if (gPreviewWin) ANativeWindow_release(gPreviewWin);
    gPreviewWin = win;
    if (win) {
        // drawn at half the screen's resolution and copied in by the CPU (see frame())
        gPvW = std::max(1, (int)w / 2); gPvH = std::max(1, (int)h / 2);
        ANativeWindow_setBuffersGeometry(win, gPvW, gPvH, WINDOW_FORMAT_RGBA_8888);
    }
}

/** A controller button in the preview (a gamepad key): 0 B / Y, 1 A / X, 2-5 the thumbstick up / down / left / right,
 *  6 the grip. */
JNIEXPORT void JNICALL Java_org_orcaport_xr_XrActivity_nativePreviewButton(JNIEnv*, jclass, jint button, jboolean down) {
    postToGui([button, down] {
        if (button < 0 || button >= 7) return;
        gPv.buttons[button] = down;
        if (down) gPv.pressedFrames[button] = button >= 2 && button <= 5 ? 8 : 2;
    });
}

/** Touch on the preview (pixels): 0 down, 1 move, 2 up of one finger = the trigger at that point; 3 two fingers moving
 *  (their centre) = look around, 4 the second finger landed (the press so far is cancelled: a release at once). */
JNIEXPORT void JNICALL Java_org_orcaport_xr_XrActivity_nativePreviewTouch(JNIEnv*, jclass, jint action, jfloat sx, jfloat sy) {
    float x = sx / 2, y = sy / 2;                  // (the preview is drawn at half the screen's resolution)
    postToGui([action, x, y] {
        if (action == 0) { gPv.tx = x; gPv.ty = y; gPv.mode = 1; gPv.pressSeen = false; gPv.releasePending = false; }
        else if (action == 1) { if (gPv.mode == 1) { gPv.tx = x; gPv.ty = y; } }
        else if (action == 2) { if (gPv.mode == 1 && !gPv.pressSeen) gPv.releasePending = true; gPv.mode = 0; }
        else if (action == 4) { gPv.mode = 2; gPv.releasePending = false; gPv.lastX = x; gPv.lastY = y; }
        else if (action == 3 && gPv.mode == 2) {
            gPv.yaw += (x - gPv.lastX) * 0.004f; gPv.pitch = std::max(-1.2f, std::min(1.2f, gPv.pitch + (y - gPv.lastY) * 0.004f));
            gPv.lastX = x; gPv.lastY = y;
        }
    });
}

}  // extern "C"
