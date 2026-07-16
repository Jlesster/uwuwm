#include "qml_bar.hpp"

#include "layershell.hpp"
#include "output.hpp"
#include "qt_runtime.hpp"
#include "server.hpp"

extern "C" {
#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
}

#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace {

// ---------------------------------------------------------------------------
// Same recipe as bar.cpp's BarPixelBuffer/wrapBarPixels -- duplicated
// rather than shared, for the same reason wallpaper.cpp's own
// near-identical buffer struct already is (see that file's comment):
// each holds its own owned copy since the source keeps getting mutated
// (there: the next round of Lua draw calls; here: the next commit()'s
// re-render) as soon as this commit() returns.
// ---------------------------------------------------------------------------
struct QmlBarPixelBuffer {
    wlr_buffer           base;
    std::vector<uint8_t> pixels;
    int                  width, height;
};

void qmlBarBufferDestroy(wlr_buffer* buffer) {
    QmlBarPixelBuffer* self = wl_container_of(buffer, self, base);
    delete self;
}

bool qmlBarBufferBeginDataPtrAccess(wlr_buffer* buffer,
                                    uint32_t /*flags*/,
                                    void**    data,
                                    uint32_t* format,
                                    size_t*   stride) {
    QmlBarPixelBuffer* self = wl_container_of(buffer, self, base);
    *data                   = self->pixels.data();
    // Same R,G,B,A-in-memory-order reasoning as bar.cpp/wallpaper.cpp's
    // identical format choice -- QImage::Format_RGBA8888's byte layout
    // matches this exactly (confirmed against the raw bytes while
    // developing the render pipeline this is fed from).
    *format = DRM_FORMAT_ABGR8888;
    *stride = static_cast<size_t>(self->width) * 4;
    return true;
}

void qmlBarBufferEndDataPtrAccess(wlr_buffer* /*buffer*/) {}

const wlr_buffer_impl kQmlBarBufferImpl = {
    .destroy               = qmlBarBufferDestroy,
    .get_dmabuf            = nullptr,
    .get_shm               = nullptr,
    .begin_data_ptr_access = qmlBarBufferBeginDataPtrAccess,
    .end_data_ptr_access   = qmlBarBufferEndDataPtrAccess,
};

wlr_buffer*
wrapQmlBarPixels(std::vector<uint8_t> pixels, int width, int height) {
    auto* buf   = new QmlBarPixelBuffer{};
    buf->pixels = std::move(pixels);
    buf->width  = width;
    buf->height = height;
    wlr_buffer_init(&buf->base, &kQmlBarBufferImpl, width, height);
    return &buf->base;
}

// 0xRRGGBBAA -> QColor -- same bit layout as Bar::fillRect (bar.hpp),
// so rc.lua color literals carry over between uwu.bar.* and uwu.qml.*
// unchanged.
QColor colorFromPacked(uint32_t packed) {
    uint8_t r = (packed >> 24) & 0xff;
    uint8_t g = (packed >> 16) & 0xff;
    uint8_t b = (packed >> 8) & 0xff;
    uint8_t a = packed & 0xff;
    return QColor(r, g, b, a);
}

}  // namespace

// ---------------------------------------------------------------------------
// QmlBar::Impl -- every Qt object this bar owns.
//
// No QOpenGLContext/QOffscreenSurface/QOpenGLFramebufferObject here
// (there used to be): this used to render into an FBO via a real GL
// context, but on uwuwm's actual bare-TTY/DRM target "offscreen" QPA
// cannot hand out a working GL context at all -- confirmed directly,
// see qt_runtime.cpp's comment on QT_QUICK_BACKEND for the full story
// and the black-screen-hang this was causing. QT_QUICK_BACKEND=
// software (set there) makes QQuickRenderControl rasterize on the CPU
// instead, so commit() below just calls QQuickWindow::grabWindow() for
// a plain QImage -- no GL objects for this Impl to own or tear down at
// all.
// ---------------------------------------------------------------------------
struct QmlBar::Impl {
    QQmlEngine* engine = nullptr;  // borrowed from QtRuntime, not owned
    QQuickRenderControl* renderControl = nullptr;
    QQuickWindow*        quickWindow   = nullptr;
    QQuickItem*          root =
        nullptr;  // owned via Qt's item/QObject tree, not directly

    std::unordered_map<int, QQuickItem*> widgets;

    int w = 0, h = 0;

    ~Impl() {
        delete quickWindow;  // cascades: destroys root and every rect/text
                             // child parented under it via Qt's item tree
        if(renderControl) { renderControl->invalidate(); }
        delete renderControl;
    }
};

QmlBar::QmlBar(Server&     server_,
               Output&     output_,
               BarPosition position_,
               int         height_,
               QtRuntime&  qt_runtime)
    : server(server_), output(&output_), position(position_), height(height_),
      impl(std::make_unique<Impl>()) {
    impl->engine        = qt_runtime.engine();
    impl->renderControl = new QQuickRenderControl();
    impl->quickWindow   = new QQuickWindow(impl->renderControl);

    QQmlComponent rootComponent(impl->engine);
    rootComponent.setData(QByteArray("import QtQuick 2.15\nItem { }"), QUrl());
    impl->root = qobject_cast<QQuickItem*>(rootComponent.create());
    impl->root->setParentItem(impl->quickWindow->contentItem());

    impl->renderControl->initialize();

    reposition();
    commit();  // opaque black until rc.lua's first real rect()/text() calls,
               // same convention Bar::Bar()'s clear(0x000000ff) establishes
}

QmlBar::~QmlBar() {
    if(scene_node) { wlr_scene_node_destroy(&scene_node->node); }
    if(output) {
        auto& v = output->qml_bars;
        v.erase(std::remove(v.begin(), v.end(), this), v.end());
    }
}

void QmlBar::detachFromOutput() {
    if(scene_node) {
        wlr_scene_node_destroy(&scene_node->node);
        scene_node = nullptr;
    }
    output = nullptr;
}

void QmlBar::reposition() {
    if(!output) { return; }
    width = output->layout_box.width;
    if(width <= 0 || height <= 0) { return; }

    if(impl->w != width || impl->h != height) {
        impl->quickWindow->setGeometry(0, 0, width, height);
        impl->root->setSize(QSizeF(width, height));
        impl->w = width;
        impl->h = height;
    }

    int y = (position == BarPosition::Top)
                ? output->layout_box.y
                : output->layout_box.y + output->layout_box.height - height;

    if(!scene_node) {
        // Same TOP-layer placement Bar::reposition() uses -- see that
        // function's comment in bar.cpp for why.
        std::vector<uint8_t> empty(static_cast<size_t>(width) * height * 4, 0);
        wlr_buffer* buf = wrapQmlBarPixels(std::move(empty), width, height);
        scene_node      = wlr_scene_buffer_create(
            server.layer_tree[ZWLR_LAYER_SHELL_V1_LAYER_TOP], buf);
        wlr_buffer_drop(buf);
        // Tags this node as belonging to a QmlBar for input.cpp's
        // barAt() hit-test, same convention Bar::reposition() uses --
        // see that function's identical comment in bar.cpp.
        scene_node->node.data = this;
    } else {
        wlr_scene_buffer_set_dest_size(scene_node, width, height);
    }
    wlr_scene_node_set_position(&scene_node->node, output->layout_box.x, y);
}

int QmlBar::createRect(int x, int y, int w, int h, uint32_t color) {
    QQmlComponent component(impl->engine);
    component.setData(QByteArray("import QtQuick 2.15\nRectangle { }"), QUrl());
    auto* item = qobject_cast<QQuickItem*>(component.create());
    if(!item) { return 0; }
    item->setParentItem(impl->root);
    item->setPosition(QPointF(x, y));
    item->setSize(QSizeF(w, h));
    item->setProperty("color", colorFromPacked(color));

    int id            = next_widget_id++;
    impl->widgets[id] = item;
    return id;
}

int QmlBar::createText(
    int x, int y, const std::string& text, int pixel_size, uint32_t color) {
    QQmlComponent component(impl->engine);
    component.setData(QByteArray("import QtQuick 2.15\nText { }"), QUrl());
    auto* item = qobject_cast<QQuickItem*>(component.create());
    if(!item) { return 0; }
    item->setParentItem(impl->root);
    item->setPosition(QPointF(x, y));
    item->setProperty("text", QString::fromStdString(text));
    item->setProperty("color", colorFromPacked(color));

    QFont font = item->property("font").value<QFont>();
    font.setPixelSize(pixel_size);
    item->setProperty("font", QVariant::fromValue(font));

    int id            = next_widget_id++;
    impl->widgets[id] = item;
    return id;
}

void QmlBar::setText(int widget_id, const std::string& text) {
    auto it = impl->widgets.find(widget_id);
    if(it == impl->widgets.end()) { return; }
    it->second->setProperty("text", QString::fromStdString(text));
}

void QmlBar::setColor(int widget_id, uint32_t color) {
    auto it = impl->widgets.find(widget_id);
    if(it == impl->widgets.end()) { return; }
    it->second->setProperty("color", colorFromPacked(color));
}

void QmlBar::setPos(int widget_id, int x, int y) {
    auto it = impl->widgets.find(widget_id);
    if(it == impl->widgets.end()) { return; }
    it->second->setPosition(QPointF(x, y));
}

void QmlBar::setSize(int widget_id, int w, int h) {
    auto it = impl->widgets.find(widget_id);
    if(it == impl->widgets.end()) { return; }
    it->second->setSize(QSizeF(w, h));
}

void QmlBar::commit() {
    if(!output || !scene_node || width <= 0 || height <= 0) { return; }

    impl->renderControl->polishItems();
    impl->renderControl->beginFrame();
    impl->renderControl->sync();
    impl->renderControl->render();
    impl->renderControl->endFrame();

    // grabWindow() reads back whatever the QSG software adaptation
    // just rasterized -- no separate FBO/texture readback step needed
    // (that was the GL path's job; see this file's Impl comment).
    QImage image = impl->quickWindow->grabWindow().convertToFormat(
        QImage::Format_RGBA8888);

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    for(int row = 0; row < height; row++) {
        std::memcpy(pixels.data() + static_cast<size_t>(row) * width * 4,
                    image.constScanLine(row),
                    static_cast<size_t>(width) * 4);
    }

    wlr_buffer* buf = wrapQmlBarPixels(std::move(pixels), width, height);
    wlr_scene_buffer_set_buffer(scene_node, buf);
    wlr_buffer_drop(buf);
}
