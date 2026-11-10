#pragma once

#include <QIcon>
#include <QPixmap>

class QApplication;

namespace phoschat::ui {

enum class Theme { Dark, Light };

Theme preferredTheme();
void applyTheme(QApplication &application, Theme theme);
void setPreferredTheme(QApplication &application, Theme theme);
QIcon brandIcon();
QIcon conversationIcon(bool channel);
QPixmap photoAvatar(const QByteArray &jpeg, int side, qreal devicePixelRatio);
QIcon searchIcon();
QIcon moreIcon();
QIcon addIcon();

} // namespace phoschat::ui
