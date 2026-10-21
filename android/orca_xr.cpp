// OrcaSlicer's side of OrcaSlicer XR: waits for Orca to be up, then drives the XR frames (xr_glue.cpp) from a timer on
// Qt's GUI thread, and gives the XR side what it needs from Orca (xr_host.h):
//   - the scene for each eye, drawn by Orca's own 3D view (GLCanvas3D::xr_render_eye) in that canvas' GL context;
//   - the controller ray in the room as the 3D view's mouse (GLCanvas3D::xr_pointer): picking, selection, gizmos, drags;
//   - the ray on the window as a desktop mouse: Qt window-system events (hover, click, drag, wheel, right click), so
//     Orca's interface works as on a desktop, menus and dialogs included;
//   - an on-screen keyboard (painted here with QPainter), shown while a text field has focus, typing into it.
#include <glad/gl.h>
#include <EGL/egl.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QJniObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <qpa/qwindowsysteminterface.h>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>
#include <android/log.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <map>
#include <thread>
#include <wx/glcanvas.h>
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "xr_host.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "orcaxr", __VA_ARGS__)

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace oxr {

void postToGui(std::function<void()> f) {
    if (QCoreApplication* a = QCoreApplication::instance()) QMetaObject::invokeMethod(a, std::move(f), Qt::QueuedConnection);
}

bool makeWidgetContextCurrent(QWidget* widget);   // (qt_gl.cpp)
bool makeShareContextCurrent();

namespace {

Plater* plater() { return wxTheApp ? wxGetApp().plater() : nullptr; }
GLCanvas3D* shownCanvas() { Plater* p = plater(); return p ? p->get_current_canvas3D() : nullptr; }
/** The context the XR side lives in: the 3D view's (it always exists; the preview's canvas has one of its own). */
/** Makes the canvas' context current for real (see releaseQtContext). */
bool makeCanvasCurrent(GLCanvas3D* c) {
    return c != nullptr && c->get_wxglcanvas() != nullptr && makeWidgetContextCurrent((QWidget*)c->get_wxglcanvas()->GetHandle());
}
GLCanvas3D* homeCanvas() { Plater* p = plater(); return p ? p->get_view3D_canvas3D() : nullptr; }

Vec3d vec(V3 v) { return Vec3d(v.x, v.y, v.z); }

// ---- the eyes: per canvas context, a framebuffer on the eye's colour image with a depth-stencil buffer of its own (the
// XR side's depth stays its own: only textures cross between the contexts)
typedef void (*FbTex2DMsFn)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
typedef void (*RbStorageMsFn)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
FbTex2DMsFn fbTex2DMs = nullptr; RbStorageMsFn rbStorageMs = nullptr;
struct EyeTarget { GLuint fbo = 0, depth = 0; int w = 0, h = 0, samples = 0; };
std::map<std::pair<GLCanvas3D*, unsigned>, EyeTarget> gEyes;

void* drawScene(const float view[16], const float proj[16], int w, int h, float nearMm, float farMm, unsigned color, unsigned /*depth*/,
                int samples, bool srgbRaw) {
    GLCanvas3D* c = shownCanvas();
    if (!makeCanvasCurrent(c)) return nullptr;
    if (samples > 1 && !fbTex2DMs) {
        fbTex2DMs = (FbTex2DMsFn)eglGetProcAddress("glFramebufferTexture2DMultisampleEXT");
        rbStorageMs = (RbStorageMsFn)eglGetProcAddress("glRenderbufferStorageMultisampleEXT");
    }
    bool ms = samples > 1 && fbTex2DMs && rbStorageMs;
    EyeTarget& t = gEyes[{c, color}];
    if (t.fbo == 0) { glsafe(::glGenFramebuffers(1, &t.fbo)); glsafe(::glGenRenderbuffers(1, &t.depth)); }
    if (t.w != w || t.h != h || t.samples != samples) {
        glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, t.depth));
        if (ms) rbStorageMs(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, w, h);
        else glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h));
        glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, 0));
        t.w = w; t.h = h; t.samples = samples;
    }
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, t.fbo));
    if (ms) fbTex2DMs(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0, samples);
    else glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0));
    glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.depth));
    glsafe(::glViewport(0, 0, w, h));
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT));       // (the colour is what the XR side drew so far)
    if (srgbRaw) glDisable(0x8DB9 /* GL_FRAMEBUFFER_SRGB_EXT */);
    Transform3d v; Matrix4d p;
    for (int col = 0; col < 4; ++col) for (int row = 0; row < 4; ++row) { v.matrix()(row, col) = view[col * 4 + row]; p(row, col) = proj[col * 4 + row]; }
    c->xr_render_eye(v, p, w, h, nearMm, farMm);
    if (srgbRaw) glEnable(0x8DB9);
    const GLenum discard[] = {GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, discard);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // the XR side waits for this drawing: a fence it waits on, or (the Android emulator, whose GL stalls waiting on
    // another context's fence) finished here
    static const bool emulator = std::strstr((const char*)glGetString(GL_RENDERER), "Emulator") != nullptr;
    if (emulator) { glFinish(); return nullptr; }
    GLsync s = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    return (void*)s;
}

/** The preview's scene: drawn into a framebuffer of the canvas' own and read back. */
std::map<GLCanvas3D*, EyeTarget> gImages; std::map<GLCanvas3D*, GLuint> gImageTex;
bool sceneImage(const float view[16], const float proj[16], int w, int h, float nearMm, float farMm, std::vector<unsigned char>& rgba) {
    GLCanvas3D* c = shownCanvas();
    if (!makeCanvasCurrent(c)) return false;
    EyeTarget& t = gImages[c]; GLuint& tex = gImageTex[c];
    if (t.fbo == 0) { glsafe(::glGenFramebuffers(1, &t.fbo)); glsafe(::glGenRenderbuffers(1, &t.depth)); glsafe(::glGenTextures(1, &tex)); }
    if (t.w != w || t.h != h) {
        glsafe(::glBindTexture(GL_TEXTURE_2D, tex));
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
        glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
        glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, t.depth));
        glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h));
        glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, 0));
        t.w = w; t.h = h;
    }
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, t.fbo));
    glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0));
    glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.depth));
    glsafe(::glViewport(0, 0, w, h));
    glsafe(::glClearColor(0, 0, 0, 0)); glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT));
    Transform3d v; Matrix4d p;
    for (int col = 0; col < 4; ++col) for (int row = 0; row < 4; ++row) { v.matrix()(row, col) = view[col * 4 + row]; p(row, col) = proj[col * 4 + row]; }
    c->xr_render_eye(v, p, w, h, nearMm, farMm);
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, t.fbo));
    rgba.resize((size_t)w * h * 4);
    glsafe(::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data()));
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, 0));
    return true;
}

bool bounds(V3* a, V3* b) {
    Plater* p = plater();
    if (p == nullptr) return false;
    PartPlate* plate = p->get_partplate_list().get_curr_plate();
    if (plate == nullptr) return false;
    BoundingBoxf3 bb = plate->get_bounding_box(false);
    if (!bb.defined) return false;
    *a = V3{(float)bb.min.x(), (float)bb.min.y(), (float)bb.min.z()};
    *b = V3{(float)bb.max.x(), (float)bb.max.y(), (float)std::max(bb.max.z(), bb.min.z() + 1.0)};
    return true;
}

float hit(V3 o, V3 d) {
    GLCanvas3D* c = shownCanvas();
    return c ? (float)c->xr_hit(vec(o), vec(d)) : -1.f;
}

void pointer(int action, V3 o, V3 d) {
    // run from Qt's event loop, outside the XR frame: Orca may open a menu or a dialog (a nested event loop) from it, and
    // frames must go on meanwhile
    postToGui([action, o, d] { if (GLCanvas3D* c = shownCanvas()) c->xr_pointer(action, vec(o), vec(d)); });
}

// ---- the ray on the window: a mouse, dispatched by the activity into the window's views (Qt's and the web views)
void panelMouse(int action, float x, float y, float amount) {
    QJniObject activity = QNativeInterface::QAndroidApplication::context();
    activity.callMethod<void>("panelMouse", "(IFFF)V", (jint)action, (jfloat)x, (jfloat)y, (jfloat)amount);
}

// ---- the keyboard: keys in rows of units, painted into an image; a press types into Qt's focus window
struct Key { QString label, lower, upper; int qtKey; float w; QRectF rect; };
std::vector<Key> gKeys;
QImage gKeysImage; unsigned gKeysVersion = 0; bool gShift = false, gCaps = false; int gKeysDrawn = -1;
const int kUnit = 100, kRows = 5, kCols = 14;

void makeKeys() {
    if (!gKeys.empty()) return;
    auto add = [](const char* label, const char* lower, const char* upper, int key, float w) {
        gKeys.push_back({QString::fromUtf8(label), QString::fromUtf8(lower), QString::fromUtf8(upper), key, w, {}});
    };
    auto chars = [&](const char* lo, const char* up) {
        for (int i = 0; lo[i]; ++i) { char l[2] = {lo[i], 0}, u[2] = {up[i], 0}; add(l, l, u, 0, 1); }
    };
    // row 0
    add("Esc", "", "", Qt::Key_Escape, 1); chars("1234567890-=", "!@#$%^&*()_+"); add("⌫", "", "", Qt::Key_Backspace, 1);
    // row 1
    add("Tab", "\t", "\t", Qt::Key_Tab, 1.5f); chars("qwertyuiop[]", "QWERTYUIOP{}"); add("\\", "\\", "|", 0, 0.5f);
    // row 2
    add("Caps", "", "", Qt::Key_CapsLock, 1.5f); chars("asdfghjkl;'", "ASDFGHJKL:\""); add("Enter", "", "", Qt::Key_Return, 1.5f);
    // row 3
    add("Shift", "", "", Qt::Key_Shift, 2); chars("zxcvbnm,./", "ZXCVBNM<>?"); add("↑", "", "", Qt::Key_Up, 1); add("Del", "", "", Qt::Key_Delete, 1);
    // row 4
    add("Home", "", "", Qt::Key_Home, 1.5f); add("End", "", "", Qt::Key_End, 1.5f); add(" ", " ", " ", Qt::Key_Space, 6);
    add("←", "", "", Qt::Key_Left, 1); add("↓", "", "", Qt::Key_Down, 1); add("→", "", "", Qt::Key_Right, 1); add("Done", "", "", -1, 2);
    // lay the rows out: each row is kCols units wide
    const int rowStarts[kRows + 1] = {0, 14, 28, 41, 54, (int)gKeys.size()};
    for (int r = 0; r < kRows; ++r) {
        float x = 0;
        for (int i = rowStarts[r]; i < rowStarts[r + 1]; ++i) { gKeys[i].rect = QRectF(x * kUnit, r * kUnit, gKeys[i].w * kUnit, kUnit); x += gKeys[i].w; }
    }
}

void paintKeys() {
    makeKeys();
    int state = (gShift ? 1 : 0) | (gCaps ? 2 : 0);
    if (state == gKeysDrawn && !gKeysImage.isNull()) return;
    gKeysDrawn = state; ++gKeysVersion;
    const int pad = 12;
    gKeysImage = QImage(kCols * kUnit + 2 * pad, kRows * kUnit + 2 * pad, QImage::Format_RGBA8888);
    gKeysImage.fill(QColor(22, 25, 30));
    QPainter p(&gKeysImage);
    p.setRenderHint(QPainter::Antialiasing);
    QFont f = p.font(); f.setPixelSize(38); p.setFont(f);
    for (const Key& k : gKeys) {
        QRectF r = k.rect.translated(pad, pad).adjusted(5, 5, -5, -5);
        bool on = (k.qtKey == Qt::Key_Shift && gShift) || (k.qtKey == Qt::Key_CapsLock && gCaps);
        bool special = k.lower.isEmpty() || k.qtKey == Qt::Key_Tab;
        QPainterPath path; path.addRoundedRect(r, 12, 12);
        p.fillPath(path, on ? QColor(42, 111, 201) : special ? QColor(52, 58, 66) : QColor(70, 77, 87));
        p.setPen(QColor(238, 241, 245));
        bool upper = gShift != gCaps;
        QString t = k.lower.isEmpty() || k.qtKey == Qt::Key_Tab || k.qtKey == Qt::Key_Space ? k.label : (gShift || (upper && k.lower[0].isLetter()) ? k.upper : k.lower);
        p.drawText(r, Qt::AlignCenter, t);
    }
}

/** A text field has the focus: something that takes typed text. */
bool wantsKeys() {
    QWidget* w = QApplication::focusWidget();
    return w && w->isVisible() && w->testAttribute(Qt::WA_InputMethodEnabled) && w->inputMethodQuery(Qt::ImEnabled).toBool();
}

bool keyboard(const unsigned char** rgba, int* w, int* h, unsigned* version) {
    if (!wantsKeys()) return false;
    paintKeys();
    *rgba = gKeysImage.constBits(); *w = gKeysImage.width(); *h = gKeysImage.height(); *version = gKeysVersion;
    return true;
}

void sendKey(int key, const QString& text, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QWindow* w = QGuiApplication::focusWindow();
    if (!w) return;
    QWindowSystemInterface::handleKeyEvent(w, QEvent::KeyPress, key, mods, text);
    QWindowSystemInterface::handleKeyEvent(w, QEvent::KeyRelease, key, mods, text);
}

void keyPress(float u, float v);
void keyboardPress(float u, float v) { postToGui([u, v] { keyPress(u, v); }); }
void keyPress(float u, float v) {
    makeKeys();
    const int pad = 12;
    QPointF at(u * gKeysImage.width() - pad, v * gKeysImage.height() - pad);
    for (const Key& k : gKeys) {
        if (!k.rect.contains(at)) continue;
        if (k.qtKey == Qt::Key_Shift) { gShift = !gShift; return; }
        if (k.qtKey == Qt::Key_CapsLock) { gCaps = !gCaps; return; }
        if (k.qtKey == -1) { if (QWidget* f = QApplication::focusWidget()) f->clearFocus(); return; }
        if (k.qtKey != 0 && k.qtKey != Qt::Key_Space && k.qtKey != Qt::Key_Tab) { sendKey(k.qtKey, QString()); return; }
        bool upper = k.lower.size() == 1 && k.lower[0].isLetter() ? (gShift != gCaps) : gShift;
        QString t = upper ? k.upper : k.lower;
        int key = k.qtKey ? k.qtKey : (t.size() == 1 ? (int)t[0].toUpper().unicode() : 0);
        sendKey(key, t, upper && t.size() == 1 && t[0].isLetter() ? Qt::ShiftModifier : Qt::NoModifier);
        if (gShift) gShift = false;
        return;
    }
}

// ---- start once Orca's main window and 3D view are up, then a frame whenever the XR side wants one
QTimer* gTimer = nullptr;

void tryBegin() {
    GLCanvas3D* c = homeCanvas();
    if (c == nullptr || !c->is_initialized()) { QTimer::singleShot(500, tryBegin); return; }
    LOG("OrcaSlicer is up: starting XR");
    // the main window fills the screen (it may have been made before Qt knew its screen was the virtual display)
    auto fill = [] {
        QScreen* sc = QGuiApplication::primaryScreen();
        wxWindow* top = wxGetApp().GetTopWindow();                        // (Orca's main frame)
        if (sc == nullptr || top == nullptr) return;
        QRect r = sc->availableGeometry();
        top->SetSize(r.x(), r.y(), r.width(), r.height());
    };
    fill();
    if (QScreen* sc = QGuiApplication::primaryScreen()) QObject::connect(sc, &QScreen::availableGeometryChanged, qApp, fill);
    AppHost h;
    h.makeCurrent = [] { return makeShareContextCurrent(); };
    h.bounds = bounds; h.hit = hit; h.drawScene = drawScene; h.sceneImage = sceneImage; h.pointer = pointer; h.panelMouse = panelMouse;
    h.keyboard = keyboard; h.keyboardPress = keyboardPress;
    begin(h);
    gTimer = new QTimer(qApp);
    gTimer->setSingleShot(true);
    QObject::connect(gTimer, &QTimer::timeout, [] {
        static bool inFrame = false;
        if (inFrame) { gTimer->start(5); return; }   // (a modal loop opened inside a frame)
        inFrame = true; int wait = frame(); inFrame = false;
        gTimer->start(std::max(0, wait));
    });
    gTimer->start(0);
}

}  // namespace

void startApp() {
    std::thread([] {
        while (QCoreApplication::instance() == nullptr) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        postToGui([] { QTimer::singleShot(1000, tryBegin); });
    }).detach();
}

}  // namespace oxr
