#include "providers/irc/irc_message.h"

#include <QStringDecoder>

namespace phoschat::irc {
namespace {

    QByteArray unescapeTag(QByteArrayView value)
    {
        QByteArray unescaped;
        unescaped.reserve(value.size());
        bool escaped = false;
        for (const auto byte : value) {
            if (escaped) {
                switch (byte) {
                case ':':
                    unescaped.append(';');
                    break;
                case 's':
                    unescaped.append(' ');
                    break;
                case 'r':
                    unescaped.append('\r');
                    break;
                case 'n':
                    unescaped.append('\n');
                    break;
                default:
                    unescaped.append(byte);
                    break;
                }
            } else if (byte != '\\') {
                unescaped.append(byte);
            }
            escaped = byte == '\\' && !escaped;
        }
        return unescaped;
    }

    QByteArrayView takeWord(QByteArrayView &remainder)
    {
        const auto end = remainder.indexOf(' ');
        const auto word = end < 0 ? remainder : remainder.first(end);
        remainder = remainder.sliced(word.size());
        while (!remainder.empty() && remainder.front() == ' ') {
            remainder = remainder.sliced(1);
        }
        return word;
    }

    bool validCommand(QByteArrayView command)
    {
        if (command.empty()) {
            return false;
        }
        if (command.front() >= '0' && command.front() <= '9') {
            if (command.size() != 3) {
                return false;
            }
            for (const auto byte : command) {
                if (byte < '0' || byte > '9') {
                    return false;
                }
            }
            return true;
        }
        for (const auto byte : command) {
            if (!(byte >= 'A' && byte <= 'Z') && !(byte >= 'a' && byte <= 'z')) {
                return false;
            }
        }
        return true;
    }

    bool isAsciiDigit(QChar character) { return character >= u'0' && character <= u'9'; }

    bool isHexDigit(QChar character)
    {
        const auto lower = character.toLower();
        return isAsciiDigit(character) || (lower >= u'a' && lower <= u'f');
    }

    qsizetype skipColor(QStringView text, qsizetype begin, int maximumDigits, bool hexadecimal)
    {
        const auto scan = [text, maximumDigits, hexadecimal](qsizetype position) {
            const auto start = position;
            while (position < text.size() && position - start < maximumDigits
                && (hexadecimal ? isHexDigit(text.at(position)) : isAsciiDigit(text.at(position)))) {
                ++position;
            }
            return hexadecimal && position - start != maximumDigits ? start : position;
        };
        const auto foreground = scan(begin);
        if (foreground == begin || foreground >= text.size() || text.at(foreground) != u',') {
            return foreground;
        }
        const auto background = scan(foreground + 1);
        return background == foreground + 1 ? foreground : background;
    }

} // namespace

QString IrcMessage::nickname() const
{
    const auto separator = source.indexOf('!') >= 0 ? source.indexOf('!') : source.indexOf('@');
    return QString::fromUtf8(separator < 0 ? source : source.left(separator));
}

std::optional<IrcMessage> parseMessage(QByteArrayView line)
{
    if (line.empty() || line.contains('\0') || line.contains('\r') || line.contains('\n')) {
        return std::nullopt;
    }

    IrcMessage message;
    if (line.front() == '@') {
        const auto tagsEnd = line.indexOf(' ');
        if (tagsEnd < 0 || tagsEnd + 1 > MaximumServerTagsBytes) {
            return std::nullopt;
        }
        auto tags = line.sliced(1, tagsEnd - 1);
        while (!tags.empty()) {
            const auto next = tags.indexOf(';');
            const auto tag = next < 0 ? tags : tags.first(next);
            tags = next < 0 ? QByteArrayView {} : tags.sliced(next + 1);
            const auto separator = tag.indexOf('=');
            const auto key = separator < 0 ? tag : tag.first(separator);
            if (key.empty()) {
                continue;
            }
            QString value;
            if (separator >= 0) {
                QStringDecoder decode(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
                value = decode(unescapeTag(tag.sliced(separator + 1)));
                if (decode.hasError()) {
                    value.clear();
                }
            }
            message.tags.insert(key.toByteArray(), value);
        }
        line = line.sliced(tagsEnd + 1);
    }

    if (line.empty() || line.size() > MaximumMessageBytes - 2) {
        return std::nullopt;
    }
    while (!line.empty() && line.front() == ' ') {
        line = line.sliced(1);
    }
    if (line.empty()) {
        return std::nullopt;
    }
    if (line.front() == ':') {
        const auto prefix = takeWord(line);
        if (prefix.size() == 1) {
            return std::nullopt;
        }
        message.source = prefix.sliced(1).toByteArray();
    }

    const auto command = takeWord(line);
    if (!validCommand(command)) {
        return std::nullopt;
    }
    message.command = command.toByteArray().toUpper();

    while (!line.empty()) {
        if (line.front() == ':') {
            message.parameters.append(line.sliced(1).toByteArray());
            break;
        }
        message.parameters.append(takeWord(line).toByteArray());
    }
    return message;
}

QList<IrcMessage> FrameDecoder::append(QByteArrayView bytes)
{
    QList<IrcMessage> messages;
    constexpr auto maximumPartial = MaximumServerTagsBytes + MaximumMessageBytes - 1;
    while (!bytes.empty()) {
        const auto end = bytes.indexOf('\n');
        const auto fragment = end < 0 ? bytes : bytes.first(end);
        if (!discarding_) {
            if (partial_.size() + fragment.size() > maximumPartial) {
                partial_.clear();
                discarding_ = true;
            } else {
                partial_.append(fragment);
            }
        }
        if (end < 0) {
            break;
        }
        if (!discarding_ && partial_.endsWith('\r')) {
            partial_.chop(1);
            if (auto message = parseMessage(partial_)) {
                messages.append(std::move(*message));
            }
        }
        partial_.clear();
        discarding_ = false;
        bytes = bytes.sliced(end + 1);
    }
    return messages;
}

void FrameDecoder::clear()
{
    partial_.clear();
    discarding_ = false;
}

QString caseFold(QStringView name, CaseMapping mapping)
{
    auto folded = name.toString();
    for (auto &character : folded) {
        if (character >= u'A' && character <= u'Z') {
            character = QChar(character.unicode() + 32);
        } else if (mapping != CaseMapping::Ascii) {
            switch (character.unicode()) {
            case '[':
                character = u'{';
                break;
            case ']':
                character = u'}';
                break;
            case '\\':
                character = u'|';
                break;
            case '^':
                if (mapping == CaseMapping::Rfc1459) {
                    character = u'~';
                }
                break;
            default:
                break;
            }
        }
    }
    return folded;
}

QString plainText(QStringView formatted)
{
    QString result;
    result.reserve(formatted.size());
    for (qsizetype i = 0; i < formatted.size(); ++i) {
        const auto character = formatted.at(i);
        if (character.unicode() == 0x03 || character.unicode() == 0x04) {
            const auto hexadecimal = character.unicode() == 0x04;
            i = skipColor(formatted, i + 1, hexadecimal ? 6 : 2, hexadecimal) - 1;
        } else if (character.unicode() >= 32 && character.unicode() != 127) {
            result.append(character);
        }
    }
    return result;
}

} // namespace phoschat::irc
