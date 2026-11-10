#pragma once

class QWidget;

namespace phoschat {
class AccountManager;
struct Conversation;

namespace ui {
    void showConversationProfile(QWidget &parent, AccountManager &accounts, const Conversation &conversation);
}
}
