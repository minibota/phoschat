#include "ui/conversations/MessageView.hpp"

#include "ui/conversations/MessageModel.hpp"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QMenu>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShortcut>
#include <QStyledItemDelegate>
#include <QTextDocument>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace phoschat::ui {
namespace {

    constexpr int kInset = 12;
    constexpr int kBubbleInset = 11;

    class MessageDelegate final : public QStyledItemDelegate {
    public:
        explicit MessageDelegate(QListView &view)
            : QStyledItemDelegate(&view)
            , m_view(view)
        {
        }

        QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
        {
            const int width = m_view.viewport()->width();
            QTextDocument document;
            layoutDocument(document, option.font, index, bubbleWidth(width));
            return { width, static_cast<int>(std::ceil(document.size().height())) + 43 };
        }

        void paint(
            QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
        {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setClipRect(option.rect);
            const bool outgoing = index.data(MessageModel::OutgoingRole).toBool();
            const bool mention = index.data(MessageModel::MentionRole).toBool();
            const bool selected = option.state.testFlag(QStyle::State_Selected);
            const int availableWidth = m_view.viewport()->width();
            const int width = bubbleWidth(availableWidth);
            const int left = option.rect.left() + (outgoing ? availableWidth - width - kInset : kInset);
            const QRect bubble(left, option.rect.top() + 4, width, option.rect.height() - 8);

            const auto colors = QApplication::palette();
            if (selected || mention) {
                auto glow = colors.color(QPalette::Accent);
                glow.setAlpha(66);
                painter->setBrush(Qt::NoBrush);
                painter->setPen(QPen(glow, 3));
                painter->drawRoundedRect(bubble.adjusted(-1, -1, 1, 1), 10, 10);
            }
            painter->setPen(colors.color(selected || mention ? QPalette::Link : QPalette::Mid));
            painter->setBrush(colors.brush(outgoing ? QPalette::AlternateBase : QPalette::Base));
            painter->drawRoundedRect(bubble, 9, 9);

            const auto timestamp = index.data(MessageModel::TimeRole).toDateTime().toLocalTime();
            auto time = timestamp.date() == QDate::currentDate()
                ? timestamp.toString(QStringLiteral("HH:mm"))
                : timestamp.toString(QStringLiteral("dd MMM · HH:mm"));
            const auto delivery = index.data(MessageModel::DeliveryRole).toString();
            if (!delivery.isEmpty())
                time += QStringLiteral(" · ") + delivery;
            auto metadata = option.font;
            metadata.setPointSizeF(std::max(8.0, metadata.pointSizeF() - 0.8));
            painter->setFont(metadata);
            painter->setPen(colors.color(QPalette::PlaceholderText));
            const auto header = bubble.adjusted(kBubbleInset, 8, -kBubbleInset, 0);
            const int timeWidth = painter->fontMetrics().horizontalAdvance(time);
            painter->drawText(QRect(header.right() - timeWidth, header.top(), timeWidth + 1, 16),
                Qt::AlignLeft | Qt::AlignVCenter, time);

            metadata.setBold(true);
            painter->setFont(metadata);
            painter->setPen(colors.color(QPalette::Link));
            const auto sender = index.data(MessageModel::SenderRole).toString();
            const int senderWidth = std::max(0, header.width() - timeWidth - 8);
            painter->drawText(QRect(header.left(), header.top(), senderWidth, 16),
                Qt::AlignLeft | Qt::AlignVCenter,
                painter->fontMetrics().elidedText(sender, Qt::ElideRight, senderWidth));

            QTextDocument document;
            layoutDocument(document, option.font, index, width);
            QAbstractTextDocumentLayout::PaintContext context;
            context.palette.setColor(QPalette::Text, colors.color(QPalette::Text));
            painter->translate(bubble.left() + kBubbleInset, bubble.top() + 27);
            document.documentLayout()->draw(painter, context);
            painter->restore();
        }

    private:
        static int bubbleWidth(int width) { return std::max(110, width * 82 / 100 - kInset); }

        static void layoutDocument(
            QTextDocument &document, const QFont &font, const QModelIndex &index, int width)
        {
            document.setDefaultFont(font);
            document.setDocumentMargin(0);
            QTextOption options;
            options.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            document.setDefaultTextOption(options);
            document.setPlainText(index.data(Qt::DisplayRole).toString());
            document.setTextWidth(width - 2 * kBubbleInset);
        }

        QListView &m_view;
    };

} // namespace

MessageView::MessageView(MessageModel &model, QWidget *parent)
    : QListView(parent)
{
    setProperty("role", QStringLiteral("timeline"));
    setModel(&model);
    setItemDelegate(new MessageDelegate(*this));
    setResizeMode(QListView::Adjust);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setAccessibleName(tr("Message history"));
    setContextMenuPolicy(Qt::CustomContextMenu);

    connect(&model, &MessageModel::rowsAboutToBeInserted, this, [this] {
        if (!m_adjustmentPending) {
            m_wasAtBottom = verticalScrollBar()->maximum() - verticalScrollBar()->value() < 20;
            m_scrollAnchor = indexAt(QPoint(viewport()->width() / 2, 0));
            m_anchorTop = m_scrollAnchor.isValid() ? visualRect(m_scrollAnchor).top() : 0;
        }
    });
    connect(
        &model, &MessageModel::rowsInserted, this, [this, &model](const QModelIndex &, int first, int last) {
            const auto message = model.index(last);
            const bool historical = message.data(MessageModel::HistoricalRole).toBool();
            const bool insertedBeforeExisting = first < model.rowCount() - (last - first + 1);
            if (historical && insertedBeforeExisting && m_scrollAnchor.isValid()) {
                // Keep the same visible message, including its partial-row offset,
                // when an older page arrives. Historical outgoing messages are not sends.
                m_followLatest = false;
            } else if (m_wasAtBottom || (!historical && message.data(MessageModel::OutgoingRole).toBool())) {
                m_followLatest = true;
            }
            scheduleScrollAdjustment();
        });
    connect(this, &MessageView::customContextMenuRequested, this, [this](const QPoint &pos) {
        const auto index = indexAt(pos);
        if (!index.isValid()) {
            return;
        }
        setCurrentIndex(index);
        QMenu menu(this);
        menu.addAction(tr("Copy message"), this, &MessageView::copyCurrentMessage);
        menu.exec(viewport()->mapToGlobal(pos));
    });
    auto *copy = new QShortcut(QKeySequence::Copy, this);
    copy->setContext(Qt::WidgetShortcut);
    connect(copy, &QShortcut::activated, this, &MessageView::copyCurrentMessage);
}

void MessageView::showLatest()
{
    m_followLatest = true;
    scheduleScrollAdjustment();
}

void MessageView::scheduleScrollAdjustment()
{
    if (m_adjustmentPending)
        return;
    m_adjustmentPending = true;
    QTimer::singleShot(0, this, [this] {
        doItemsLayout();
        if (m_followLatest) {
            scrollToBottom();
        } else if (m_scrollAnchor.isValid()) {
            scrollTo(m_scrollAnchor, QAbstractItemView::PositionAtTop);
            verticalScrollBar()->setValue(verticalScrollBar()->value() - m_anchorTop);
        }
        m_scrollAnchor = QPersistentModelIndex {};
        m_adjustmentPending = false;
        m_followLatest = false;
    });
}

bool MessageView::viewportEvent(QEvent *event)
{
    const bool handled = QListView::viewportEvent(event);
    if (event->type() == QEvent::Resize) {
        const auto *resize = static_cast<QResizeEvent *>(event);
        if (resize->size().width() != resize->oldSize().width())
            scheduleDelayedItemsLayout();
    }
    return handled;
}

void MessageView::paintEvent(QPaintEvent *event)
{
    QListView::paintEvent(event);
    if (!model() || model()->rowCount() == 0) {
        QPainter painter(viewport());
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(viewport()->rect().adjusted(18, 18, -18, -18), Qt::AlignCenter | Qt::TextWordWrap,
            tr("Messages will appear here."));
    }
}

void MessageView::copyCurrentMessage()
{
    if (currentIndex().isValid()) {
        QApplication::clipboard()->setText(currentIndex().data(Qt::DisplayRole).toString());
    }
}

} // namespace phoschat::ui
