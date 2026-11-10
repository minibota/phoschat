#pragma once

#include "models/types.h"

#include <QAbstractListModel>
#include <QHash>

namespace phoschat::ui {

class MessageModel final : public QAbstractListModel {
public:
    enum Role {
        SenderRole = Qt::UserRole + 1,
        TimeRole,
        OutgoingRole,
        MentionRole,
        HistoricalRole,
        MessageIdRole,
        DeliveryRole
    };

    explicit MessageModel(QList<Message> messages, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    void append(const Message &message);
    void remove(const QString &id);
    void clear();
    void setRetainFullHistory(bool enabled) { m_retainFullHistory = enabled; }

private:
    void indexRows(int first = 0);
    QList<Message> m_messages;
    QHash<QString, int> m_rowsById;
    bool m_retainFullHistory {};
};

} // namespace phoschat::ui
