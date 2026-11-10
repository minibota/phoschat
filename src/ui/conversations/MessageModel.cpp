#include "ui/conversations/MessageModel.hpp"

#include <algorithm>
#include <utility>

namespace phoschat::ui {
namespace {

    constexpr int kTimelineLimit = 1000;
    constexpr int kTrimBatch = 100;

} // namespace

MessageModel::MessageModel(QList<Message> messages, QObject *parent)
    : QAbstractListModel(parent)
    , m_messages(std::move(messages))
{
    indexRows();
}

void MessageModel::indexRows(int first)
{
    for (int row = first; row < m_messages.size(); ++row) {
        if (!m_messages.at(row).id.isEmpty()) {
            m_rowsById.insert(m_messages.at(row).id, row);
        }
    }
}

int MessageModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_messages.size());
}

QVariant MessageModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_messages.size()) {
        return {};
    }
    const auto &message = m_messages.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return message.text;
    case Qt::AccessibleTextRole:
        return tr("%1 wrote %2").arg(message.sender, message.text);
    case SenderRole:
        return message.sender;
    case TimeRole:
        return message.timestamp;
    case OutgoingRole:
        return message.outgoing;
    case MentionRole:
        return message.mention;
    default:
        return {};
    }
}

void MessageModel::append(const Message &message)
{
    const auto existing = m_rowsById.constFind(message.id);
    if (existing != m_rowsById.cend()) {
        const int row = *existing;
        m_messages[row] = message;
        emit dataChanged(index(row), index(row));
        return;
    }

    if (m_messages.size() >= kTimelineLimit) {
        beginRemoveRows({}, 0, kTrimBatch - 1);
        m_messages.remove(0, kTrimBatch);
        m_rowsById.clear();
        indexRows();
        endRemoveRows();
    }

    const auto position = std::upper_bound(m_messages.cbegin(), m_messages.cend(), message.timestamp,
        [](const QDateTime &time, const Message &entry) { return time < entry.timestamp; });
    const auto row = static_cast<int>(position - m_messages.cbegin());
    beginInsertRows({}, row, row);
    m_messages.insert(row, message);
    indexRows(row);
    endInsertRows();
}

void MessageModel::remove(const QString &id)
{
    const auto found = m_rowsById.find(id);
    if (found == m_rowsById.end())
        return;
    const int row = *found;
    beginRemoveRows({}, row, row);
    m_rowsById.erase(found);
    m_messages.removeAt(row);
    indexRows(row);
    endRemoveRows();
}

void MessageModel::clear()
{
    beginResetModel();
    m_messages.clear();
    m_rowsById.clear();
    endResetModel();
}

} // namespace phoschat::ui
