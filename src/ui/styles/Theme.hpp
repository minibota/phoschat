#pragma once

#include <QIcon>

class QApplication;

namespace phoschat::ui {

enum class Theme { Dark, Light };

Theme preferredTheme();
void applyTheme(QApplication &application, Theme theme);
void setPreferredTheme(QApplication &application, Theme theme);
QIcon brandIcon();
QIcon conversationIcon(bool channel);
QIcon searchIcon();
QIcon moreIcon();
QIcon addIcon();

} // namespace phoschat::ui
