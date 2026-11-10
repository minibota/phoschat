#pragma once

#include <QListView>
#include <QPersistentModelIndex>

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
    void scheduleScrollAdjustment();
    QPersistentModelIndex m_scrollAnchor;
    int m_anchorTop {};
    bool m_wasAtBottom { true };
    bool m_adjustmentPending {};
    bool m_followLatest {};
};

} // namespace phoschat::ui
