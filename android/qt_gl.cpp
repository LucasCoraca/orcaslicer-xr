// Qt's GL context calls, in a file of their own: Qt's OpenGL headers clash with OrcaSlicer's GL loader (glad).
#include <QtGui/QOffscreenSurface>
#include <QtGui/QOpenGLContext>
#include <QtOpenGLWidgets/QOpenGLWidget>
#include <map>

namespace oxr {

/** Makes the GL context of a canvas (a QOpenGLWidget) current on this thread, on an offscreen surface (the caller binds
 *  its own framebuffer). Through Qt, so Qt knows which context is current; QOpenGLWidget::makeCurrent() would skip it
 *  while Qt believes its context is still current, though the XR side made its own current in between. */
bool makeWidgetContextCurrent(QWidget* widget) {
    auto* gl = qobject_cast<QOpenGLWidget*>(widget);
    QOpenGLContext* ctx = gl ? gl->context() : nullptr;
    if (ctx == nullptr) return false;                         // (a canvas never shown has none yet)
    static std::map<QOpenGLContext*, QOffscreenSurface*> surfaces;
    QOffscreenSurface*& s = surfaces[ctx];
    if (s == nullptr) { s = new QOffscreenSurface(ctx->screen()); s->setFormat(ctx->format()); s->create(); }
    if (QOpenGLContext* cur = QOpenGLContext::currentContext(); cur != nullptr && cur != ctx) cur->doneCurrent();
    else if (cur == ctx) ctx->doneCurrent();                   // (it may not really be current: the XR side's may be)
    return ctx->makeCurrent(s);
}

/** Makes Qt's global share context current (every canvas' context shares objects with it): the XR side's context shares
 *  with it. */
bool makeShareContextCurrent() {
    QOpenGLContext* ctx = QOpenGLContext::globalShareContext();
    if (ctx == nullptr) return false;
    static QOffscreenSurface* s = nullptr;
    if (s == nullptr) { s = new QOffscreenSurface(ctx->screen()); s->setFormat(ctx->format()); s->create(); }
    if (QOpenGLContext* cur = QOpenGLContext::currentContext()) cur->doneCurrent();
    return ctx->makeCurrent(s);
}

}  // namespace oxr
