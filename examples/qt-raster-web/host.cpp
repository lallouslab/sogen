#include <QtWidgets>
#include <emscripten.h>
#include <qt_raster_protocol.hpp>
#include <cstring>

namespace qb = sogen::qt_raster;
// clang-format off
EM_JS(void, host_send, (const void* data, int size), { globalThis.rasterHostSend(HEAPU8.slice(data, data + size)); });
EM_JS(void, host_ready, (), { globalThis.rasterHostReady(Module); });
EM_JS(void, host_map, (const char* key, int native), { (globalThis.rasterWindows ||= {})[UTF8ToString(key)] = native; });
EM_JS(void, host_unmap, (const char* key), { delete globalThis.rasterWindows?.[UTF8ToString(key)]; });
EM_JS(int, pointer_buttons, (), { return globalThis.rasterPointerButtons || 0; });
EM_JS(void, host_focus, (int native), { Module.specialHTMLTargets["!qtwindow" + native]?.focus(); });
// clang-format on

namespace
{
    class Surface;
    QHash<QString, Surface*> surfaces;
    uint32_t sequence{};

    QString key(const uint64_t session, const uint32_t id)
    {
        return QString::number(session) + ":" + QString::number(id);
    }

    QPoint origin()
    {
        return QGuiApplication::primaryScreen()->geometry().topLeft();
    }

    class Surface final : public QWidget
    {
      public:
        uint64_t session;
        uint32_t id;
        QRect guest_geometry;
        QImage frame;
        qreal scale{1};
        bool configuring{};
        bool resizing{};
        QPointer<Surface> owner;
        QHash<int, int> keys;
        Qt::MouseButtons buttons{};
        QPointF pointer;

        Surface(const uint64_t identity, const uint32_t value, Surface* parent, const Qt::WindowFlags flags)
            : QWidget(parent, flags),
              session(identity),
              id(value),
              owner(parent)
        {
            this->setFocusPolicy(Qt::StrongFocus);
            this->setMouseTracking(true);
            this->setAttribute(Qt::WA_OpaquePaintEvent);
        }

        void send(const qb::Op op, const std::array<int32_t, 8>& args = {}, const QByteArray& text = {})
        {
            qb::Packet packet{};
            packet.session = this->session;
            packet.id = this->id;
            packet.op = op;
            packet.seq = ++sequence;
            packet.a = args;
            packet.size = uint32_t(text.size());
            QByteArray bytes(qsizetype(sizeof(packet)) + text.size(), Qt::Uninitialized);
            std::memcpy(bytes.data(), &packet, sizeof(packet));
            if (!text.isEmpty())
            {
                std::memcpy(bytes.data() + sizeof(packet), text.data(), size_t(text.size()));
            }
            host_send(bytes.data(), int(bytes.size()));
        }

        bool overlay() const
        {
            return this->windowType() == Qt::Popup || this->windowType() == Qt::ToolTip;
        }

        void apply_geometry()
        {
            if (this->resizing)
            {
                return;
            }
            const auto& g = this->guest_geometry;
            const QRect expected(qRound(g.x() / this->scale), qRound(g.y() / this->scale), qRound(g.width() / this->scale),
                                 qRound(g.height() / this->scale));
            this->configuring = true;
            this->setGeometry(expected.translated(origin()));
            this->configuring = false;
        }

        void release_input()
        {
            for (auto it = this->keys.cbegin(); it != this->keys.cend(); ++it)
            {
                this->send(qb::Key, {7, it.value()});
            }
            this->keys.clear();
            for (const int32_t button : {1, 2, 4, 8, 16})
            {
                if ((int(this->buttons) & button) != 0)
                {
                    this->buttons &= ~Qt::MouseButtons(button);
                    this->send(qb::Mouse, {qRound(this->pointer.x() * this->scale), qRound(this->pointer.y() * this->scale),
                                           int(this->buttons), button, 3});
                }
            }
        }

        void activate()
        {
            if (this->windowType() == Qt::ToolTip)
            {
                return;
            }
            this->windowHandle()->requestActivate();
            this->setFocus();
            host_focus(int(this->winId()));
        }

        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            painter.fillRect(this->rect(), Qt::white);
            if (!this->frame.isNull())
            {
                painter.drawImage(QPoint(), this->frame);
            }
        }

        void changed_geometry()
        {
            if (this->configuring || this->overlay())
            {
                return;
            }
            if (pointer_buttons() != 0)
            {
                this->resizing = true;
                return;
            }
            const QRect value(qRound((this->geometry().x() - origin().x()) * this->scale),
                              qRound((this->geometry().y() - origin().y()) * this->scale), qRound(this->width() * this->scale),
                              qRound(this->height() * this->scale));
            if (value != this->guest_geometry)
            {
                this->guest_geometry = value;
                this->send(qb::Resize, {value.x(), value.y(), value.width(), value.height()});
            }
        }

        void resizeEvent(QResizeEvent* event) override
        {
            QWidget::resizeEvent(event);
            this->changed_geometry();
        }

        void moveEvent(QMoveEvent* event) override
        {
            QWidget::moveEvent(event);
            this->changed_geometry();
        }

        void closeEvent(QCloseEvent* event) override
        {
            event->ignore();
            this->send(qb::Close);
        }

        void mouse(QMouseEvent* event, const int32_t type)
        {
            this->buttons = event->buttons();
            this->pointer = event->position();
            this->send(qb::Mouse, {qRound(this->pointer.x() * this->scale), qRound(this->pointer.y() * this->scale), int(event->buttons()),
                                   int(event->button()), type, int(event->modifiers())});
            event->accept();
        }

        void mousePressEvent(QMouseEvent* event) override
        {
            this->activate();
            this->send(qb::Focus, {1});
            this->mouse(event, 2);
        }

        void mouseDoubleClickEvent(QMouseEvent* event) override
        {
            this->mousePressEvent(event);
        }

        void mouseReleaseEvent(QMouseEvent* event) override
        {
            this->mouse(event, 3);
        }

        void mouseMoveEvent(QMouseEvent* event) override
        {
            this->mouse(event, 5);
        }

        void wheelEvent(QWheelEvent* event) override
        {
            this->send(qb::Wheel, {qRound(event->position().x() * this->scale), qRound(event->position().y() * this->scale),
                                   event->angleDelta().x(), event->angleDelta().y(), int(event->modifiers())});
            event->accept();
        }

        bool event(QEvent* event) override
        {
            if (event->type() == QEvent::ShortcutOverride)
            {
                event->accept();
                return true;
            }
            if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease)
            {
                auto* key = static_cast<QKeyEvent*>(event);
                if (event->type() == QEvent::KeyPress)
                {
                    this->keys.insert(key->key(), key->key());
                }
                else
                {
                    this->keys.remove(key->key());
                }
                this->send(qb::Key, {int(event->type()), key->key(), int(key->modifiers()), key->isAutoRepeat() ? 1 : 0},
                           key->text().toUtf8());
                event->accept();
                return true;
            }
            if (event->type() == QEvent::WindowActivate)
            {
                this->send(qb::Focus, {1});
            }
            if (event->type() == QEvent::WindowDeactivate)
            {
                this->release_input();
                this->send(qb::Focus, {0});
            }
            return QWidget::event(event);
        }
    };
}

extern "C" EMSCRIPTEN_KEEPALIVE void raster_release_geometry()
{
    for (auto* surface : surfaces)
    {
        if (surface->resizing)
        {
            surface->resizing = false;
            surface->changed_geometry();
        }
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void raster_release_input()
{
    for (auto* surface : surfaces)
    {
        surface->release_input();
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void raster_clear()
{
    while (!surfaces.isEmpty())
    {
        auto* surface = surfaces.begin().value();
        for (auto* candidate : surfaces)
        {
            if (!candidate->owner)
            {
                surface = candidate;
                break;
            }
        }
        QList<QString> retired;
        for (auto it = surfaces.cbegin(); it != surfaces.cend(); ++it)
        {
            if (it.value() == surface || surface->isAncestorOf(it.value()))
            {
                retired.append(it.key());
            }
        }
        for (const auto& value : retired)
        {
            const auto encoded = value.toUtf8();
            host_unmap(encoded.constData());
            surfaces.remove(value);
        }
        delete surface;
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE int raster_receive(const char* data, const int size)
{
    if (size < int(sizeof(qb::Packet)))
    {
        return 0;
    }
    qb::Packet packet{};
    std::memcpy(&packet, data, sizeof(packet));
    if (!qb::valid(packet, size_t(size)) || packet.session == 0)
    {
        return 0;
    }
    const auto value = key(packet.session, packet.id);
    if (packet.op == qb::Create)
    {
        if (!qb::valid_geometry(packet) || packet.id == 0 || packet.a[7] < 500 || packet.a[7] > 4000 || packet.a[5] < 0 ||
            packet.a[5] > 2 || surfaces.contains(value) || surfaces.size() >= int(qb::max_windows))
        {
            return 0;
        }
        auto* parent = surfaces.value(key(packet.session, uint32_t(packet.a[6])), nullptr);
        if (packet.a[6] != 0 && !parent)
        {
            return 0;
        }
        auto* owner = parent;
        const auto flags = Qt::WindowFlags(packet.a[4]) == Qt::Widget ? Qt::WindowFlags(Qt::Window) : Qt::WindowFlags(packet.a[4]);
        if (!parent && (flags & Qt::WindowType_Mask) == Qt::Popup)
        {
            for (auto* candidate : surfaces)
            {
                if (candidate->session == packet.session && candidate == QApplication::activeWindow())
                {
                    owner = candidate;
                    break;
                }
            }
        }
        auto* surface = new Surface(packet.session, packet.id, parent, flags);
        surface->owner = owner;
        surfaces.insert(value, surface);
        surface->guest_geometry = {packet.a[0], packet.a[1], packet.a[2], packet.a[3]};
        surface->scale = packet.a[7] / 1000.;
        surface->apply_geometry();
        surface->setWindowTitle(QString::fromUtf8(data + sizeof(packet), int(packet.size)));
        surface->setWindowModality(Qt::WindowModality(packet.a[5]));
        const auto encoded = value.toUtf8();
        host_map(encoded.constData(), int(surface->winId()));
        return 1;
    }
    if (packet.op == qb::Hello || packet.op == qb::End)
    {
        return 1;
    }
    auto* surface = surfaces.value(value, nullptr);
    if (!surface)
    {
        return 0;
    }
    switch (packet.op)
    {
    case qb::Geometry:
        if (!qb::valid_geometry(packet))
        {
            return 0;
        }
        surface->guest_geometry = {packet.a[0], packet.a[1], packet.a[2], packet.a[3]};
        surface->apply_geometry();
        break;
    case qb::Visible: {
        if (packet.a[0] < 0 || packet.a[0] > 1 || packet.a[1] < 0 || packet.a[1] > 2)
        {
            return 0;
        }
        const bool was_visible = surface->isVisible();
        const bool owner_active = QApplication::activeWindow() == surface->owner || QApplication::activeWindow() == surface;
        surface->setWindowModality(Qt::WindowModality(packet.a[1]));
        if (!packet.a[0])
        {
            surface->release_input();
        }
        surface->setVisible(packet.a[0] != 0);
        if (was_visible && !packet.a[0] && surface->overlay() && surface->owner && owner_active &&
            QApplication::applicationState() == Qt::ApplicationActive)
        {
            surface->owner->activate();
            surface->owner->send(qb::Focus, {1});
        }
        break;
    }
    case qb::Destroy:
        for (auto* child : surfaces)
        {
            if (child->parentWidget() == surface)
            {
                return 0;
            }
        }
        surface->release_input();
        surfaces.remove(value);
        host_unmap(value.toUtf8().constData());
        surface->hide();
        delete surface;
        break;
    case qb::Title:
        surface->setWindowTitle(QString::fromUtf8(data + sizeof(packet), int(packet.size)));
        break;
    case qb::Raise:
        if (packet.a[0])
        {
            surface->raise();
        }
        else
        {
            surface->lower();
        }
        break;
    case qb::Focus:
        if (packet.a[0])
        {
            surface->activate();
        }
        break;
    case qb::Cursor:
        if (packet.a[0] < 0 || packet.a[0] > 24)
        {
            return 0;
        }
        surface->setCursor(packet.a[0] == 24 ? Qt::ArrowCursor : Qt::CursorShape(packet.a[0]));
        break;
    case qb::Frame:
        if (!qb::valid_frame(packet))
        {
            return 0;
        }
        surface->scale = packet.a[3] / 1000.;
        surface->apply_geometry();
        surface->frame = QImage(reinterpret_cast<const uchar*>(data + sizeof(packet)), packet.a[0], packet.a[1], packet.a[2],
                                QImage::Format_ARGB32_Premultiplied)
                             .copy();
        surface->frame.setDevicePixelRatio(surface->scale);
        surface->update();
        break;
    default:
        return 0;
    }
    return 1;
}

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    host_ready();
    return application.exec();
}
