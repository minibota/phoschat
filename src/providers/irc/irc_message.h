#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringView>

#include <optional>

namespace phoschat::irc {

inline constexpr qsizetype MaximumMessageBytes = 512;
inline constexpr qsizetype MaximumServerTagsBytes = 8191;

struct IrcMessage {
    QHash<QByteArray, QString> tags;
    QByteArray source;
    QByteArray command;
    QList<QByteArray> parameters;

    [[nodiscard]] QString nickname() const;
};

[[nodiscard]] std::optional<IrcMessage> parseMessage(QByteArrayView line);

class FrameDecoder {
public:
    [[nodiscard]] QList<IrcMessage> append(QByteArrayView bytes);
    void clear();

private:
    QByteArray partial_;
    bool discarding_ = false;
};

enum class CaseMapping { Ascii, StrictRfc1459, Rfc1459 };

[[nodiscard]] QString caseFold(QStringView name, CaseMapping mapping);
[[nodiscard]] QString plainText(QStringView formatted);

} // namespace phoschat::irc
