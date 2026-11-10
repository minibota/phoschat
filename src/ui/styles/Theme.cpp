#include "ui/styles/Theme.hpp"

#include <QApplication>
#include <QBuffer>
#include <QColor>
#include <QCryptographicHash>
#include <QImageReader>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPixmapCache>
#include <QRadialGradient>
#include <QSettings>
#include <QStyleFactory>
#include <QStyleHints>
#include <array>

namespace phoschat::ui {
namespace {

    constexpr auto kThemePreference = "ui/theme";
    constexpr auto kBrandGreen = "#bcf492";
    constexpr auto kBrandMint = "#83e5bc";
    constexpr auto kBrandInk = "#14312a";

    struct Colors {
        const char *window;
        const char *panel;
        const char *outgoing;
        const char *text;
        const char *muted;
        const char *disabled;
        const char *accent;
        const char *focus;
        const char *border;
        const char *raisedBorder;
        const char *button;
        const char *buttonHover;
        const char *rowHover;
        const char *selection;
        const char *badgeInk;
        const char *badgeTop;
        const char *badgeBase;
        const char *badgeBorder;
        const char *offlineInk;
        const char *offlineBase;
        const char *offlineBorder;
        const char *error;
        const char *primaryTop;
        const char *primaryBase;
        const char *primaryInk;
        const char *primaryHover;
        const char *primaryDisabled;
        const char *toolTip;
        const char *menuSelection;
    };

    constexpr Colors dark {
        .window = "#111922",
        .panel = "#17222e",
        .outgoing = "#203a34",
        .text = "#e5eef2",
        .muted = "#a5bac5",
        .disabled = "#869aa6",
        .accent = "#abe69c",
        .focus = "#83e5bc",
        .border = "#2a3a47",
        .raisedBorder = "#344956",
        .button = "#1a2934",
        .buttonHover = "#253742",
        .rowHover = "#192832",
        .selection = "#253e42",
        .badgeInk = "#bdf5bd",
        .badgeTop = "#29483e",
        .badgeBase = "#243f3a",
        .badgeBorder = "#3d6254",
        .offlineInk = "#b9c7cf",
        .offlineBase = "#24313c",
        .offlineBorder = "#344654",
        .error = "#ffadac",
        .primaryTop = "#bce982",
        .primaryBase = "#83e5bc",
        .primaryInk = "#153228",
        .primaryHover = "#c4f3a8",
        .primaryDisabled = "#202e38",
        .toolTip = "#253541",
        .menuSelection = "#254038",
    };

    constexpr Colors light {
        .window = "#f2f5ee",
        .panel = "#fbfdf7",
        .outgoing = "#e2f1dd",
        .text = "#1b2c23",
        .muted = "#506457",
        .disabled = "#64776b",
        .accent = "#286547",
        .focus = "#31774f",
        .border = "#b8caba",
        .raisedBorder = "#98b5a4",
        .button = "#e9f1e6",
        .buttonHover = "#dae9d9",
        .rowHover = "#e8efe5",
        .selection = "#d6e9d6",
        .badgeInk = "#23553c",
        .badgeTop = "#e7f4d5",
        .badgeBase = "#dbeddf",
        .badgeBorder = "#a1c7ab",
        .offlineInk = "#495b51",
        .offlineBase = "#e8eee8",
        .offlineBorder = "#bccbbf",
        .error = "#a33736",
        .primaryTop = "#def39c",
        .primaryBase = "#ace6bc",
        .primaryInk = "#173827",
        .primaryHover = "#e2f5b2",
        .primaryDisabled = "#e6ede4",
        .toolTip = "#f2f6e9",
        .menuSelection = "#deeedc",
    };

    enum class Glyph { Brand, Channel, Direct, Search, More, Add, Count };

    QIcon makeIcon(Glyph glyph, const QColor &ink)
    {
        QIcon icon;
        for (const auto side : { 16, 20, 24, 32, 48, 64, 128 }) {
            QPixmap pixmap(side, side);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.scale(side / 32.0, side / 32.0);
            painter.setPen(QPen(ink, 2.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            switch (glyph) {
            case Glyph::Brand: {
                painter.setPen(Qt::NoPen);
                QRadialGradient bloom(QPointF(15.5, 14), 21);
                bloom.setColorAt(0, QColor(184, 241, 138, 180));
                bloom.setColorAt(0.64, QColor(144, 225, 130, 95));
                bloom.setColorAt(1, QColor(131, 229, 188, 0));
                painter.setBrush(bloom);
                painter.drawRoundedRect(QRectF(0, 0, 32, 32), 9, 9);
                QLinearGradient phosphor(3, 2, 29, 30);
                phosphor.setColorAt(0, QColor(kBrandGreen));
                phosphor.setColorAt(1, QColor(kBrandMint));
                painter.setBrush(phosphor);
                painter.drawRoundedRect(QRectF(1.5, 1.5, 29, 29), 7, 7);
                painter.setBrush(QColor(kBrandInk));
                QPainterPath bubble;
                bubble.addRoundedRect(QRectF(7, 7.5, 18, 14), 4, 4);
                bubble.moveTo(11, 19.5);
                bubble.lineTo(17, 20.5);
                bubble.lineTo(10.5, 25);
                bubble.closeSubpath();
                bubble.setFillRule(Qt::WindingFill);
                painter.drawPath(bubble);
                painter.setPen(QPen(QColor(kBrandGreen), 1.65, Qt::SolidLine, Qt::RoundCap));
                painter.drawLine(QPointF(11, 12.6), QPointF(21, 12.6));
                painter.drawLine(QPointF(11, 16.3), QPointF(17.5, 16.3));
                break;
            }
            case Glyph::Channel:
                painter.setPen(QPen(ink, 2.1, Qt::SolidLine, Qt::RoundCap));
                painter.drawLine(QPointF(13.2, 7), QPointF(10, 25));
                painter.drawLine(QPointF(22, 7), QPointF(18.8, 25));
                painter.drawLine(QPointF(7.5, 12.5), QPointF(25.5, 12.5));
                painter.drawLine(QPointF(6.5, 19.5), QPointF(24.5, 19.5));
                break;
            case Glyph::Direct:
                painter.setPen(QPen(ink, 2.1, Qt::SolidLine, Qt::RoundCap));
                painter.drawEllipse(QRectF(12, 6, 8, 8));
                painter.drawArc(QRectF(7, 18, 18, 13), 0, 180 * 16);
                break;
            case Glyph::Search:
                painter.drawEllipse(QRectF(5.5, 5.5, 15.5, 15.5));
                painter.drawLine(QPointF(19, 19), QPointF(26, 26));
                break;
            case Glyph::More:
                painter.setPen(Qt::NoPen);
                painter.setBrush(ink);
                for (const auto y : { 8, 16, 24 }) {
                    painter.drawEllipse(QPointF(16, y), 1.8, 1.8);
                }
                break;
            case Glyph::Add:
                painter.setPen(QPen(ink, 2.1, Qt::SolidLine, Qt::RoundCap));
                painter.drawLine(QPointF(8, 16), QPointF(24, 16));
                painter.drawLine(QPointF(16, 8), QPointF(16, 24));
                break;
            case Glyph::Count:
                break;
            }
            icon.addPixmap(pixmap);
        }
        return icon;
    }

    QString styleSheet(const Colors &colors)
    {
        QString sheet = QStringLiteral(R"(
        QLabel[role="muted"] { color: @muted@; }
        QLabel[role="section"] { color: @muted@; font-weight: 700; }
        QLabel[role="heading"] { color: @text@; font-size: 19px; font-weight: 700; }
        QLabel[role="profile"] { color: @text@; font-weight: 700; }
        QLabel[role="badge"] {
            color: @badge-ink@; border: 1px solid @badge-border@;
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 @badge-top@, stop:1 @badge-base@);
            border-radius: 8px; padding: 3px 7px; font-size: 10px; font-weight: 700;
        }
        QLabel[role="badge"][connected="false"] {
            color: @offline-ink@; background: @offline-base@; border-color: @offline-border@;
        }
        QLabel[role="error"] { color: @error@; }
        QWidget[role="profileCard"] { background: @panel@; border: 1px solid @border@; border-radius: 10px; }
        QLabel[role="avatar"] { color: @badge-ink@; background: @badge-top@; border-radius: 15px; font-weight: 700; }
        QLineEdit, QSpinBox, QComboBox, QPlainTextEdit, QTreeWidget, QListWidget, QListView {
            background: @panel@; border: 1px solid @border@; border-radius: 8px;
        }
        QLineEdit, QSpinBox, QComboBox { min-height: 25px; padding: 3px 8px; }
        QLineEdit:focus, QSpinBox:focus, QComboBox:focus, QPlainTextEdit:focus {
            border-color: @focus@;
        }
        QLineEdit[role="search"] { min-height: 29px; padding-left: 4px; }
        QPlainTextEdit { padding: 5px; }
        QTreeWidget[role="conversations"], QListView[role="timeline"] {
            border: 0; border-radius: 0; background: @window@; outline: 0;
        }
        QTreeWidget[role="conversations"]::item { min-height: 34px; }
        QTreeWidget[role="conversations"]::branch:selected { background: @selection@; }
        QTreeWidget[role="conversations"]::item:selected, QListView[role="timeline"]::item:selected {
            background: @selection@; color: @text@;
        }
        QTreeWidget[role="conversations"]::item:hover:!selected { background: @row-hover@; }
        QToolButton, QPushButton {
            border: 1px solid @border@; border-radius: 7px; background: @button@;
            padding: 5px 9px; min-height: 20px;
        }
        QToolButton:hover, QPushButton:hover { background: @button-hover@; }
        QToolButton:focus, QPushButton:focus { border-color: @focus@; }
        /* QLineEdit positions its embedded action buttons. Keep the general
           button padding/minimum height from moving their icons off-center. */
        QLineEdit QToolButton {
            border: none; border-radius: 0; background: transparent;
            padding: 0; min-width: 0; min-height: 0;
        }
        QToolButton[role="accountHeader"] {
            color: @muted@; border: 1px solid transparent; border-radius: 3px;
            background: transparent; padding: 3px 4px; min-height: 17px; font-weight: 700;
        }
        QToolButton[role="accountHeader"]:hover { background: @row-hover@; }
        QToolButton[role="accountHeader"]:checked { color: @text@; background: @panel@; }
        QToolButton[role="accountHeader"]:focus { border-color: @focus@; }
        QToolButton[role="icon"] { padding: 3px; min-width: 23px; min-height: 23px; }
        QToolButton[role="icon"]::menu-indicator { image: none; width: 0; }
        QToolButton[role="chatProfile"], QToolButton[role="chatProfileTitle"] {
            border: 1px solid transparent; border-radius: 4px; background: transparent;
            padding: 0; min-width: 0; min-height: 0;
        }
        QToolButton[role="chatProfileTitle"] { color: @text@; font-size: 19px; font-weight: 700; }
        QToolButton[role="chatProfile"]:hover, QToolButton[role="chatProfileTitle"]:hover {
            background: @row-hover@;
        }
        QToolButton[role="chatProfile"]:focus, QToolButton[role="chatProfileTitle"]:focus {
            border-color: @focus@;
        }
        QPushButton[role="primary"] {
            color: @primary-ink@; border-color: @primary-base@;
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 @primary-top@, stop:1 @primary-base@);
            font-weight: 700; padding: 5px 12px;
        }
        QPushButton[role="primary"]:hover { background: @primary-hover@; }
        QPushButton[role="primary"]:focus { border-color: @focus@; }
        QPushButton[role="primary"]:disabled {
            color: @disabled@; background: @primary-disabled@; border-color: @border@;
        }
        QMenu { background: @panel@; border: 1px solid @raised-border@; padding: 4px; }
        QMenu::item { padding: 6px 22px 6px 10px; border-radius: 4px; }
        QMenu::item:selected { background: @menu-selection@; color: @text@; }
        QMenu::item:disabled { color: @disabled@; }
        QMenu::separator { height: 1px; background: @border@; margin: 4px 5px; }
        QFrame[role="rule"] { background: @border@; border: none; }
        QToolTip { color: @text@; background: @tooltip@; border: 1px solid @raised-border@; padding: 4px; }
        )");
        for (const auto &[token, color] : {
                 std::pair { "@window@", colors.window },
                 { "@panel@", colors.panel },
                 { "@text@", colors.text },
                 { "@muted@", colors.muted },
                 { "@disabled@", colors.disabled },
                 { "@focus@", colors.focus },
                 { "@border@", colors.border },
                 { "@raised-border@", colors.raisedBorder },
                 { "@button@", colors.button },
                 { "@button-hover@", colors.buttonHover },
                 { "@row-hover@", colors.rowHover },
                 { "@selection@", colors.selection },
                 { "@badge-ink@", colors.badgeInk },
                 { "@badge-top@", colors.badgeTop },
                 { "@badge-base@", colors.badgeBase },
                 { "@badge-border@", colors.badgeBorder },
                 { "@offline-ink@", colors.offlineInk },
                 { "@offline-base@", colors.offlineBase },
                 { "@offline-border@", colors.offlineBorder },
                 { "@error@", colors.error },
                 { "@primary-top@", colors.primaryTop },
                 { "@primary-base@", colors.primaryBase },
                 { "@primary-ink@", colors.primaryInk },
                 { "@primary-hover@", colors.primaryHover },
                 { "@primary-disabled@", colors.primaryDisabled },
                 { "@tooltip@", colors.toolTip },
                 { "@menu-selection@", colors.menuSelection },
             }) {
            sheet.replace(QLatin1String(token), QLatin1String(color));
        }
        return sheet;
    }

    QIcon contextualIcon(Glyph glyph, QPalette::ColorRole role)
    {
        static std::array<std::pair<QColor, QIcon>, static_cast<size_t>(Glyph::Count)> icons;
        auto &[ink, icon] = icons[static_cast<size_t>(glyph)];
        const auto color = QApplication::palette().color(role);
        if (ink != color) {
            ink = color;
            icon = makeIcon(glyph, ink);
        }
        return icon;
    }

} // namespace

Theme preferredTheme()
{
    return QSettings().value(QLatin1String(kThemePreference)).toString() == QStringLiteral("light")
        ? Theme::Light
        : Theme::Dark;
}

void applyTheme(QApplication &application, Theme theme)
{
    if (application.styleSheet().isEmpty()) {
        application.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    }
    application.styleHints()->setColorScheme(
        theme == Theme::Light ? Qt::ColorScheme::Light : Qt::ColorScheme::Dark);
    const auto &colors = theme == Theme::Light ? light : dark;
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(colors.window));
    palette.setColor(QPalette::WindowText, QColor(colors.text));
    palette.setColor(QPalette::Base, QColor(colors.panel));
    palette.setColor(QPalette::AlternateBase, QColor(colors.outgoing));
    palette.setColor(QPalette::Text, QColor(colors.text));
    palette.setColor(QPalette::Button, QColor(colors.button));
    palette.setColor(QPalette::ButtonText, QColor(colors.text));
    palette.setColor(QPalette::PlaceholderText, QColor(colors.muted));
    palette.setColor(QPalette::Highlight, QColor(colors.selection));
    palette.setColor(QPalette::HighlightedText, QColor(colors.text));
    palette.setColor(QPalette::Accent, QColor(colors.focus));
    palette.setColor(QPalette::Link, QColor(colors.accent));
    palette.setColor(QPalette::Mid, QColor(colors.border));
    palette.setColor(QPalette::ToolTipBase, QColor(colors.toolTip));
    palette.setColor(QPalette::ToolTipText, QColor(colors.text));
    for (const auto role : { QPalette::Text, QPalette::ButtonText, QPalette::WindowText }) {
        palette.setColor(QPalette::Disabled, role, QColor(colors.disabled));
    }
    application.setPalette(palette);
    application.setWindowIcon(brandIcon());
    application.setStyleSheet(styleSheet(colors));
}

void setPreferredTheme(QApplication &application, Theme theme)
{
    QSettings().setValue(QLatin1String(kThemePreference),
        theme == Theme::Light ? QStringLiteral("light") : QStringLiteral("dark"));
    applyTheme(application, theme);
}

QIcon brandIcon()
{
    static const auto icon = makeIcon(Glyph::Brand, QColor(kBrandInk));
    return icon;
}

QIcon conversationIcon(bool channel)
{
    return contextualIcon(channel ? Glyph::Channel : Glyph::Direct, QPalette::Link);
}

QPixmap photoAvatar(const QByteArray &jpeg, int side, qreal devicePixelRatio)
{
    if (jpeg.isEmpty() || jpeg.size() > 128 * 1024)
        return {};
    const int pixels = qBound(1, qRound(side * devicePixelRatio), 256);
    const auto key
        = QStringLiteral("phoschat.avatar/%1/%2/%3")
              .arg(pixels)
              .arg(devicePixelRatio)
              .arg(QString::fromLatin1(QCryptographicHash::hash(jpeg, QCryptographicHash::Sha256).toHex()));
    QPixmap cached;
    if (QPixmapCache::find(key, &cached))
        return cached;

    QBuffer source;
    source.setData(jpeg);
    source.open(QIODevice::ReadOnly);
    QImageReader reader(&source, "jpeg");
    reader.setAutoDetectImageFormat(false);
    const auto size = reader.size();
    if (size.isEmpty() || size.width() > 512 || size.height() > 512)
        return {};
    const auto image = reader.read();
    if (image.isNull())
        return {};
    const auto scaled
        = image.scaled(pixels, pixels, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);

    QPixmap result(pixels, pixels);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath circle;
    circle.addEllipse(QRectF(0, 0, pixels, pixels));
    painter.setClipPath(circle);
    painter.drawImage(QPointF((pixels - scaled.width()) / 2.0, (pixels - scaled.height()) / 2.0), scaled);
    painter.end();
    result.setDevicePixelRatio(devicePixelRatio);
    QPixmapCache::insert(key, result);
    return result;
}

QIcon searchIcon() { return contextualIcon(Glyph::Search, QPalette::PlaceholderText); }

QIcon moreIcon() { return contextualIcon(Glyph::More, QPalette::Text); }

QIcon addIcon() { return contextualIcon(Glyph::Add, QPalette::Link); }

} // namespace phoschat::ui
