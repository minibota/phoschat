#pragma once

#include <QListView>

namespace phoschat::ui {

class MessageModel;

class MessageView final : public QListView {
public:
    explicit MessageView(MessageModel &model, QWidget *parent = nullptr);
    void showLatest();

protected:
    bool viewportEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void copyCurrentMessage();
    bool m_wasAtBottom { true };
};

} // namespace phoschat::ui
