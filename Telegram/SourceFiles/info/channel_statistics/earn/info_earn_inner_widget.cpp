/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/channel_statistics/earn/info_earn_inner_widget.h"

#include "api/api_earn.h"
#include "api/api_statistics.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "boxes/peers/edit_peer_color_box.h" // AddLevelBadge.
#include "chat_helpers/stickers_emoji_pack.h"
#include "core/ui_integration.h" // Core::MarkedTextContext.
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "data/data_premium_limits.h"
#include "data/data_session.h"
#include "data/stickers/data_custom_emoji.h"
#include "info/channel_statistics/earn/info_earn_widget.h"
#include "info/info_controller.h"
#include "info/profile/info_profile_values.h" // Info::Profile::NameValue.
#include "info/statistics/info_statistics_inner_widget.h" // FillLoading.
#include "lang/lang_keys.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "statistics/chart_widget.h"
#include "ui/boxes/boost_box.h"
#include "ui/controls/userpic_button.h"
#include "ui/effects/animation_value_f.h"
#include "ui/effects/fade_animation.h"
#include "ui/effects/toggle_arrow.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "ui/text/text_utilities.h"
#include "ui/vertical_list.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/wrap/slide_wrap.h"
#include "styles/style_boxes.h"
#include "styles/style_channel_earn.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_statistics.h"
#include "styles/style_window.h" // mainMenuToggleFourStrokes.

#include <QtWidgets/QApplication>

namespace Info::ChannelEarn {
namespace {

using EarnInt = Data::EarnInt;

constexpr auto kMinorPartLength = 9;
constexpr auto kZero = QChar('0');
constexpr auto kDot = QChar('.');

[[nodiscard]] QString MajorPart(EarnInt value) {
	const auto string = QString::number(value);
	const auto diff = int(string.size()) - kMinorPartLength;
	return (diff <= 0) ? QString(kZero) : string.mid(0, diff);
}

[[nodiscard]] QString MinorPart(EarnInt value) {
	if (!value) {
		return QString(kDot) + kZero + kZero;
	}
	const auto string = QString::number(value);
	const auto diff = int(string.size()) - kMinorPartLength;
	const auto result = (diff < 0)
		? kDot + u"%1"_q.arg(0, std::abs(diff), 10, kZero) + string
		: kDot + string.mid(diff);
	const auto begin = (result.constData());
	const auto end = (begin + result.size());
	auto ch = end - 1;
	auto zeroCount = 0;
	while (ch != begin) {
		if ((*ch) == kZero) {
			zeroCount++;
		} else {
			break;
		}
		ch--;
	}
	return result.chopped(zeroCount);
}

[[nodiscard]] QString ToUsd(EarnInt value, float64 rate) {
	constexpr auto kApproximately = QChar(0x2248);
	const auto multiplier = EarnInt(rate * Data::kEarnMultiplier);
	const auto result = (value * multiplier) / Data::kEarnMultiplier;
	return QString(kApproximately)
		+ QChar('$')
		+ MajorPart(result)
		+ MinorPart(result);
}

[[nodiscard]] bool WithdrawalEnabled(not_null<Main::Session*> session) {
	const auto key = u"channel_revenue_withdrawal_enabled"_q;
	return session->appConfig().get<bool>(key, false);
}

void AddArrow(not_null<Ui::RpWidget*> parent) {
	const auto arrow = Ui::CreateChild<Ui::RpWidget>(parent.get());
	arrow->paintRequest(
	) | rpl::start_with_next([=](const QRect &r) {
		auto p = QPainter(arrow);

		const auto path = Ui::ToggleUpDownArrowPath(
			st::statisticsShowMoreButtonArrowSize,
			st::statisticsShowMoreButtonArrowSize,
			st::statisticsShowMoreButtonArrowSize,
			st::mainMenuToggleFourStrokes,
			0.);

		auto hq = PainterHighQualityEnabler(p);
		p.fillPath(path, st::lightButtonFg);
	}, arrow->lifetime());
	arrow->resize(Size(st::statisticsShowMoreButtonArrowSize * 2));
	arrow->move(st::statisticsShowMoreButtonArrowPosition);
	arrow->show();
}

void AddHeader(
		not_null<Ui::VerticalLayout*> content,
		tr::phrase<> text) {
	Ui::AddSkip(content);
	const auto header = content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			text(),
			st::channelEarnSemiboldLabel),
		st::boxRowPadding);
	header->resizeToWidth(header->width());
}

void AddRecipient(not_null<Ui::GenericBox*> box, const TextWithEntities &t) {
	const auto wrap = box->addRow(
		object_ptr<Ui::CenterWrap<Ui::RoundButton>>(
			box,
			object_ptr<Ui::RoundButton>(
				box,
				rpl::single(QString()),
				st::channelEarnHistoryRecipientButton)));
	const auto container = wrap->entity();
	const auto label = Ui::CreateChild<Ui::FlatLabel>(
		container,
		rpl::single(t),
		st::channelEarnHistoryRecipientButtonLabel);
	label->setAttribute(Qt::WA_TransparentForMouseEvents);
	label->setBreakEverywhere(true);
	label->setTryMakeSimilarLines(true);
	label->resizeToWidth(container->width());
	label->sizeValue(
	) | rpl::start_with_next([=](const QSize &s) {
		const auto padding = QMargins(
			st::chatGiveawayPeerPadding.right(),
			st::chatGiveawayPeerPadding.top(),
			st::chatGiveawayPeerPadding.right(),
			st::chatGiveawayPeerPadding.top());
		container->resize(
			container->width(),
			(Rect(s) + padding).height());
		label->moveToLeft(0, padding.top());
	}, container->lifetime());
	container->setClickedCallback([=] {
		QGuiApplication::clipboard()->setText(t.text);
		box->showToast(tr::lng_text_copied(tr::now));
	});
}

[[nodiscard]] TextWithEntities EmojiCurrency(
		not_null<Main::Session*> session) {
	auto emoji = TextWithEntities{
		.text = (QString(QChar(0xD83D)) + QChar(0xDC8E)),
	};
	if (const auto e = Ui::Emoji::Find(emoji.text)) {
		const auto sticker = session->emojiStickersPack().stickerForEmoji(e);
		if (sticker.document) {
			emoji = Data::SingleCustomEmoji(sticker.document);
		}
	}
	return emoji;
}

[[nodiscard]] QString FormatDate(const QDateTime &date) {
	return tr::lng_group_call_starts_short_date(
		tr::now,
		lt_date,
		langDayOfMonth(date.date()),
		lt_time,
		QLocale().toString(date.time(), QLocale::ShortFormat));
}

} // namespace

InnerWidget::InnerWidget(
	QWidget *parent,
	not_null<Controller*> controller,
	not_null<PeerData*> peer)
: VerticalLayout(parent)
, _controller(controller)
, _peer(peer)
, _show(controller->uiShow()) {
}

void InnerWidget::load() {
	const auto api = lifetime().make_state<Api::EarnStatistics>(
		_peer->asChannel());

	Info::Statistics::FillLoading(
		this,
		_loaded.events_starting_with(false) | rpl::map(!rpl::mappers::_1),
		_showFinished.events());

	_showFinished.events(
	) | rpl::take(1) | rpl::start_with_next([=] {
		api->request(
		) | rpl::start_with_error_done([](const QString &error) {
		}, [=] {
			_state = api->data();
			_loaded.fire(true);
			fill();
		}, lifetime());
	}, lifetime());
}

void InnerWidget::fill() {
	const auto container = this;
	const auto &data = _state;

	constexpr auto kMinus = QChar(0x2212);
	constexpr auto kApproximately = QChar(0x2248);
	const auto currency = u"TON"_q;
	const auto multiplier = data.usdRate;

	const auto session = &_peer->session();
	const auto channel = _peer->asChannel();
	const auto withdrawalEnabled = WithdrawalEnabled(session);
	const auto makeContext = [=](not_null<Ui::FlatLabel*> l) {
		return Core::MarkedTextContext{
			.session = session,
			.customEmojiRepaint = [=] { l->update(); },
		};
	};
	const auto addEmojiToMajor = [=](
			not_null<Ui::FlatLabel*> label,
			EarnInt value) {
		auto emoji = EmojiCurrency(session);
		label->setMarkedText(
			emoji.append(' ').append(MajorPart(value)),
			makeContext(label));
	};

	const auto arrow = Ui::Text::SingleCustomEmoji(
		session->data().customEmojiManager().registerInternalEmoji(
			st::topicButtonArrow,
			st::channelEarnLearnArrowMargins,
			false));
	const auto addAboutWithLearn = [&](const tr::phrase<lngtag_link> &text) {
		auto label = object_ptr<Ui::FlatLabel>(
			container,
			st::boxDividerLabel);
		const auto raw = label.data();
		text(
			lt_link,
			tr::lng_channel_earn_about_link(
				lt_emoji,
				rpl::single(arrow),
				Ui::Text::RichLangValue
			) | rpl::map([](TextWithEntities text) {
				return Ui::Text::Link(std::move(text), 1);
			}),
			Ui::Text::RichLangValue
		) | rpl::start_with_next([=](const TextWithEntities &text) {
			raw->setMarkedText(text, makeContext(raw));
		}, label->lifetime());
		label->setLink(1, std::make_shared<LambdaClickHandler>([=] {
			_show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
				box->setNoContentMargin(true);

				const auto content = box->verticalLayout().get();

				Ui::AddSkip(content);
				Ui::AddSkip(content);
				Ui::AddSkip(content);
				{
					const auto &icon = st::channelEarnLearnTitleIcon;
					const auto rect = Rect(icon.size() * 1.4);
					auto owned = object_ptr<Ui::RpWidget>(content);
					owned->resize(rect.size());
					const auto widget = box->addRow(
						object_ptr<Ui::CenterWrap<>>(
							content,
							std::move(owned)))->entity();
					widget->paintRequest(
					) | rpl::start_with_next([=] {
						auto p = Painter(widget);
						p.setPen(Qt::NoPen);
						p.setBrush(st::activeButtonBg);
						p.drawEllipse(rect);
						icon.paintInCenter(p, rect);
					}, widget->lifetime());
				}
				Ui::AddSkip(content);
				Ui::AddSkip(content);
				box->addRow(object_ptr<Ui::CenterWrap<>>(
					content,
					object_ptr<Ui::FlatLabel>(
						content,
						tr::lng_channel_earn_learn_title(),
						st::boxTitle)));
				Ui::AddSkip(content);
				Ui::AddSkip(content);
				Ui::AddSkip(content);
				Ui::AddSkip(content);
				{
					const auto padding = QMargins(
						st::settingsButton.padding.left(),
						st::boxRowPadding.top(),
						st::boxRowPadding.right(),
						st::boxRowPadding.bottom());
					const auto addEntry = [&](
							rpl::producer<QString> title,
							rpl::producer<QString> about,
							const style::icon &icon) {
						const auto top = content->add(
							object_ptr<Ui::FlatLabel>(
								content,
								std::move(title),
								st::channelEarnSemiboldLabel),
							padding);
						Ui::AddSkip(content, st::channelEarnHistoryThreeSkip);
						content->add(
							object_ptr<Ui::FlatLabel>(
								content,
								std::move(about),
								st::channelEarnHistoryRecipientLabel),
							padding);
						const auto left = Ui::CreateChild<Ui::RpWidget>(
							box->verticalLayout().get());
						left->paintRequest(
						) | rpl::start_with_next([=] {
							auto p = Painter(left);
							icon.paint(p, 0, 0, left->width());
						}, left->lifetime());
						left->resize(icon.size());
						top->geometryValue(
						) | rpl::start_with_next([=](const QRect &g) {
							left->moveToLeft(
								(g.left() - left->width()) / 2,
								g.top() + st::channelEarnHistoryThreeSkip);
						}, left->lifetime());
					};
					addEntry(
						tr::lng_channel_earn_learn_in_subtitle(),
						tr::lng_channel_earn_learn_in_about(),
						st::channelEarnLearnChannelIcon);
					Ui::AddSkip(content);
					Ui::AddSkip(content);
					addEntry(
						tr::lng_channel_earn_learn_split_subtitle(),
						tr::lng_channel_earn_learn_split_about(),
						st::sponsoredAboutSplitIcon);
					Ui::AddSkip(content);
					Ui::AddSkip(content);
					addEntry(
						tr::lng_channel_earn_learn_out_subtitle(),
						tr::lng_channel_earn_learn_out_about(),
						st::channelEarnLearnWithdrawalsIcon);
					Ui::AddSkip(content);
					Ui::AddSkip(content);
				}
				Ui::AddSkip(content);
				Ui::AddSkip(content);
				{
					const auto l = box->addRow(
						object_ptr<Ui::CenterWrap<Ui::FlatLabel>>(
							content,
							object_ptr<Ui::FlatLabel>(
								content,
								st::boxTitle)))->entity();
					tr::lng_channel_earn_learn_coin_title(
						lt_emoji,
						rpl::single(
							Ui::Text::Link(EmojiCurrency(session), 1)),
						Ui::Text::RichLangValue
					) | rpl::start_with_next([=](TextWithEntities t) {
						l->setMarkedText(std::move(t), makeContext(l));
					}, l->lifetime());
					const auto diamonds = l->lifetime().make_state<int>(0);
					l->setLink(1, std::make_shared<LambdaClickHandler>([=] {
						const auto count = (*diamonds);
						box->showToast((count == 100)
							? u"You are rich now!"_q
							: (u"You have earned "_q
								+ QString::number(++(*diamonds))
								+ (!count
									? u" diamond!"_q
									: u" diamonds!"_q)));
					}));
				}
				Ui::AddSkip(content);
				{
					const auto label = box->addRow(
						object_ptr<Ui::FlatLabel>(
							content,
							st::channelEarnLearnDescription));
					tr::lng_channel_earn_learn_coin_about(
						lt_link,
						tr::lng_channel_earn_about_link(
							lt_emoji,
							rpl::single(arrow),
							Ui::Text::RichLangValue
						) | rpl::map([](TextWithEntities text) {
							return Ui::Text::Link(std::move(text), 1);
						}),
						Ui::Text::RichLangValue
					) | rpl::start_with_next([=, l = label](
							TextWithEntities t) {
						l->setMarkedText(std::move(t), makeContext(l));
						l->resizeToWidth(box->width()
							- rect::m::sum::h(st::boxRowPadding));
					}, label->lifetime());
				}
				Ui::AddSkip(content);
				Ui::AddSkip(content);
				{
					const auto &st = st::premiumPreviewDoubledLimitsBox;
					box->setStyle(st);
					auto button = object_ptr<Ui::RoundButton>(
						container,
						tr::lng_channel_earn_learn_close(),
						st::defaultActiveButton);
					button->resizeToWidth(box->width()
						- st.buttonPadding.left()
						- st.buttonPadding.left());
					button->setClickedCallback([=] { box->closeBox(); });
					box->addButton(std::move(button));
				}
			}));
		}));
		container->add(object_ptr<Ui::DividerLabel>(
			container,
			std::move(label),
			st::defaultBoxDividerLabelPadding,
			RectPart::Top | RectPart::Bottom));
	};
	addAboutWithLearn(tr::lng_channel_earn_about);
	{
		using Type = Statistic::ChartViewType;
		Ui::AddSkip(container);
		Ui::AddSkip(container);
		if (data.topHoursGraph.chart) {
			const auto widget = container->add(
				object_ptr<Statistic::ChartWidget>(container),
				st::statisticsLayerMargins);

			widget->setChartData(data.topHoursGraph.chart, Type::Linear);
			widget->setTitle(tr::lng_channel_earn_chart_top_hours());
		}
		if (data.revenueGraph.chart) {
			Ui::AddSkip(container);
			Ui::AddDivider(container);
			Ui::AddSkip(container);
			Ui::AddSkip(container);
			const auto widget = container->add(
				object_ptr<Statistic::ChartWidget>(container),
				st::statisticsLayerMargins);

			widget->setChartData(data.revenueGraph.chart, Type::StackBar);
			widget->setTitle(tr::lng_channel_earn_chart_revenue());
		}
		Ui::AddSkip(container);
	}
	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	{
		AddHeader(container, tr::lng_channel_earn_overview_title);
		Ui::AddSkip(container, st::channelEarnOverviewTitleSkip);

		const auto addOverview = [&](
				EarnInt value,
				const tr::phrase<> &text) {
			const auto line = container->add(
				Ui::CreateSkipWidget(container, 0),
				st::boxRowPadding);
			const auto majorLabel = Ui::CreateChild<Ui::FlatLabel>(
				line,
				st::channelEarnOverviewMajorLabel);
			addEmojiToMajor(majorLabel, value);
			const auto minorLabel = Ui::CreateChild<Ui::FlatLabel>(
				line,
				MinorPart(value),
				st::channelEarnOverviewMinorLabel);
			const auto secondMinorLabel = Ui::CreateChild<Ui::FlatLabel>(
				line,
				ToUsd(value, multiplier),
				st::channelEarnOverviewSubMinorLabel);
			rpl::combine(
				line->widthValue(),
				majorLabel->sizeValue()
			) | rpl::start_with_next([=](int available, const QSize &size) {
				line->resize(line->width(), size.height());
				minorLabel->moveToLeft(
					size.width(),
					st::channelEarnOverviewMinorLabelSkip);
				secondMinorLabel->resizeToWidth(available
					- size.width()
					- minorLabel->width());
				secondMinorLabel->moveToLeft(
					rect::right(minorLabel)
						+ st::channelEarnOverviewSubMinorLabelPos.x(),
					st::channelEarnOverviewSubMinorLabelPos.y());
			}, minorLabel->lifetime());
			Ui::ToggleChildrenVisibility(line, true);

			Ui::AddSkip(container);
			const auto sub = container->add(
				object_ptr<Ui::FlatLabel>(
					container,
					text(),
					st::channelEarnOverviewSubMinorLabel),
				st::boxRowPadding);
			sub->setTextColorOverride(st::windowSubTextFg->c);
		};
		addOverview(data.availableBalance, tr::lng_channel_earn_available);
		Ui::AddSkip(container);
		Ui::AddSkip(container);
		addOverview(data.currentBalance, tr::lng_channel_earn_reward);
		Ui::AddSkip(container);
		Ui::AddSkip(container);
		addOverview(data.overallRevenue, tr::lng_channel_earn_total);
		Ui::AddSkip(container);
	}
	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	if (channel) {
		const auto value = data.availableBalance;
		Ui::AddSkip(container);
		AddHeader(container, tr::lng_channel_earn_balance_title);
		Ui::AddSkip(container);

		const auto labels = container->add(
			object_ptr<Ui::CenterWrap<Ui::RpWidget>>(
				container,
				object_ptr<Ui::RpWidget>(container)))->entity();

		const auto majorLabel = Ui::CreateChild<Ui::FlatLabel>(
			labels,
			st::channelEarnBalanceMajorLabel);
		addEmojiToMajor(majorLabel, value);
		majorLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
		const auto minorLabel = Ui::CreateChild<Ui::FlatLabel>(
			labels,
			MinorPart(value),
			st::channelEarnBalanceMinorLabel);
		minorLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
		rpl::combine(
			majorLabel->sizeValue(),
			minorLabel->sizeValue()
		) | rpl::start_with_next([=](
				const QSize &majorSize,
				const QSize &minorSize) {
			labels->resize(
				majorSize.width() + minorSize.width(),
				majorSize.height());
			majorLabel->moveToLeft(0, 0);
			minorLabel->moveToRight(
				0,
				st::channelEarnBalanceMinorLabelSkip);
		}, labels->lifetime());
		Ui::ToggleChildrenVisibility(labels, true);

		Ui::AddSkip(container);
		container->add(
			object_ptr<Ui::CenterWrap<>>(
				container,
				object_ptr<Ui::FlatLabel>(
					container,
					ToUsd(value, multiplier),
					st::channelEarnOverviewSubMinorLabel)));

		Ui::AddSkip(container);

		const auto &stButton = st::defaultActiveButton;
		const auto button = container->add(
			object_ptr<Ui::RoundButton>(
				container,
				rpl::never<QString>(),
				stButton),
			st::boxRowPadding);

		const auto label = Ui::CreateChild<Ui::FlatLabel>(
			button,
			tr::lng_channel_earn_balance_button(tr::now),
			st::channelEarnSemiboldLabel);
		label->setTextColorOverride(stButton.textFg->c);
		label->setAttribute(Qt::WA_TransparentForMouseEvents);
		rpl::combine(
			button->sizeValue(),
			label->sizeValue()
		) | rpl::start_with_next([=](const QSize &b, const QSize &l) {
			label->moveToLeft(
				(b.width() - l.width()) / 2,
				(b.height() - l.height()) / 2);
		}, label->lifetime());

		const auto fadeAnimation =
			label->lifetime().make_state<Ui::Animations::Simple>();

		const auto colorText = [=](float64 value) {
			label->setTextColorOverride(
				anim::with_alpha(
					stButton.textFg->c,
					anim::interpolateF(.5, 1., value)));
		};
		colorText(withdrawalEnabled ? 1. : 0.);
#ifndef _DEBUG
		button->setAttribute(
			Qt::WA_TransparentForMouseEvents,
			!withdrawalEnabled);
#endif

		Api::HandleWithdrawalButton(channel, button, _controller->uiShow());
		Ui::ToggleChildrenVisibility(button, true);

		Ui::AddSkip(container);
		Ui::AddSkip(container);
		addAboutWithLearn(withdrawalEnabled
			? tr::lng_channel_earn_balance_about
			: tr::lng_channel_earn_balance_about_temp);
		Ui::AddSkip(container);
	}
	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	{
		AddHeader(container, tr::lng_channel_earn_history_title);
		Ui::AddSkip(container);

		const auto historyList = container->add(
			object_ptr<Ui::VerticalLayout>(container));
		const auto addHistoryEntry = [=](
				const Data::EarnHistoryEntry &entry,
				const tr::phrase<> &text) {
			const auto wrap = historyList->add(
				object_ptr<Ui::PaddingWrap<Ui::VerticalLayout>>(
					historyList,
					object_ptr<Ui::VerticalLayout>(historyList),
					QMargins()));
			const auto inner = wrap->entity();
			inner->setAttribute(Qt::WA_TransparentForMouseEvents);
			inner->add(object_ptr<Ui::FlatLabel>(
				inner,
				text(),
				st::channelEarnSemiboldLabel));

			const auto isIn = entry.type == Data::EarnHistoryEntry::Type::In;
			const auto recipient = Ui::Text::Wrapped(
				{ entry.provider },
				EntityType::Code);
			if (!recipient.text.isEmpty()) {
				Ui::AddSkip(inner, st::channelEarnHistoryThreeSkip);
				const auto label = inner->add(object_ptr<Ui::FlatLabel>(
					inner,
					rpl::single(recipient),
					st::channelEarnHistoryRecipientLabel));
				label->setBreakEverywhere(true);
				label->setTryMakeSimilarLines(true);
				Ui::AddSkip(inner, st::channelEarnHistoryThreeSkip);
			} else {
				Ui::AddSkip(inner, st::channelEarnHistoryTwoSkip);
			}

			const auto isFailed = entry.status
				== Data::EarnHistoryEntry::Status::Failed;
			const auto isPending = entry.status
				== Data::EarnHistoryEntry::Status::Pending;
			const auto dateText = (!entry.dateTo.isNull() || isFailed)
				? (FormatDate(entry.date)
					+ ' '
					+ QChar(8212)
					+ ' '
					+ (isFailed
						? tr::lng_channel_earn_history_out_failed(tr::now)
						: FormatDate(entry.dateTo)))
				: isPending
				? tr::lng_channel_earn_history_pending(tr::now)
				: FormatDate(entry.date);
			inner->add(object_ptr<Ui::FlatLabel>(
				inner,
				dateText,
				st::channelEarnHistorySubLabel)
			)->setTextColorOverride(isFailed
				? std::make_optional<QColor>(st::menuIconAttentionColor->c)
				: std::nullopt);

			const auto color = (isIn
				? st::boxTextFgGood
				: st::menuIconAttentionColor)->c;
			const auto majorText = (isIn ? '+' : kMinus)
				+ MajorPart(entry.amount);
			const auto majorLabel = Ui::CreateChild<Ui::FlatLabel>(
				wrap,
				majorText,
				st::channelEarnHistoryMajorLabel);
			majorLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
			majorLabel->setTextColorOverride(color);
			const auto minorText = MinorPart(entry.amount)
				+ ' '
				+ currency;
			const auto minorLabel = Ui::CreateChild<Ui::FlatLabel>(
				wrap,
				minorText,
				st::channelEarnHistoryMinorLabel);
			minorLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
			minorLabel->setTextColorOverride(color);
			const auto button = Ui::CreateChild<Ui::SettingsButton>(
				wrap,
				rpl::single(QString()));
			Ui::ToggleChildrenVisibility(wrap, true);

			const auto detailsBox = [=, peer = _peer](
					not_null<Ui::GenericBox*> box) {
				Ui::AddSkip(box->verticalLayout());
				Ui::AddSkip(box->verticalLayout());
				const auto labels = box->addRow(
					object_ptr<Ui::CenterWrap<Ui::RpWidget>>(
						box,
						object_ptr<Ui::RpWidget>(box)))->entity();

				const auto majorLabel = Ui::CreateChild<Ui::FlatLabel>(
					labels,
					majorText,
					st::channelEarnOverviewMajorLabel);
				majorLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
				majorLabel->setTextColorOverride(color);
				const auto minorLabel = Ui::CreateChild<Ui::FlatLabel>(
					labels,
					minorText,
					st::channelEarnOverviewMinorLabel);
				minorLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
				minorLabel->setTextColorOverride(color);
				rpl::combine(
					majorLabel->sizeValue(),
					minorLabel->sizeValue()
				) | rpl::start_with_next([=](
						const QSize &majorSize,
						const QSize &minorSize) {
					labels->resize(
						majorSize.width() + minorSize.width(),
						majorSize.height());
					majorLabel->moveToLeft(0, 0);
					minorLabel->moveToRight(
						0,
						st::channelEarnOverviewMinorLabelSkip);
				}, box->lifetime());

				Ui::AddSkip(box->verticalLayout());
				box->addRow(object_ptr<Ui::CenterWrap<>>(
					box,
					object_ptr<Ui::FlatLabel>(
						box,
						dateText,
						st::channelEarnHistorySubLabel)));
				Ui::AddSkip(box->verticalLayout());
				Ui::AddSkip(box->verticalLayout());
				Ui::AddSkip(box->verticalLayout());
				box->addRow(object_ptr<Ui::CenterWrap<>>(
					box,
					object_ptr<Ui::FlatLabel>(
						box,
						isIn
							? tr::lng_channel_earn_history_in_about()
							: tr::lng_channel_earn_history_out(),
						st::channelEarnHistoryMajorLabel)));
				Ui::AddSkip(box->verticalLayout());
				if (isIn) {
					Ui::AddSkip(box->verticalLayout());
				}

				if (!recipient.text.isEmpty()) {
					AddRecipient(box, recipient);
				}
				if (isIn) {
					const auto peerBubble = box->addRow(
						object_ptr<Ui::CenterWrap<>>(
							box,
							object_ptr<Ui::RpWidget>(box)))->entity();
					peerBubble->setAttribute(
						Qt::WA_TransparentForMouseEvents);
					const auto left = Ui::CreateChild<Ui::UserpicButton>(
						peerBubble,
						peer,
						st::uploadUserpicButton);
					const auto right = Ui::CreateChild<Ui::FlatLabel>(
						peerBubble,
						Info::Profile::NameValue(peer),
						st::channelEarnSemiboldLabel);
					rpl::combine(
						left->sizeValue(),
						right->sizeValue()
					) | rpl::start_with_next([=](
							const QSize &leftSize,
							const QSize &rightSize) {
						const auto padding = QMargins(
							st::chatGiveawayPeerPadding.left() * 2,
							st::chatGiveawayPeerPadding.top(),
							st::chatGiveawayPeerPadding.right(),
							st::chatGiveawayPeerPadding.bottom());
						peerBubble->resize(
							leftSize.width()
								+ rightSize.width()
								+ rect::m::sum::h(padding),
							leftSize.height());
						left->moveToLeft(0, 0);
						right->moveToRight(padding.right(), padding.top());
						const auto maxRightSize = box->width()
							- rect::m::sum::h(st::boxRowPadding)
							- rect::m::sum::h(padding)
							- leftSize.width();
						if (rightSize.width() > maxRightSize) {
							right->resizeToWidth(maxRightSize);
						}
					}, peerBubble->lifetime());
					peerBubble->paintRequest(
					) | rpl::start_with_next([=] {
						auto p = QPainter(peerBubble);
						auto hq = PainterHighQualityEnabler(p);
						p.setPen(Qt::NoPen);
						p.setBrush(st::windowBgOver);
						const auto rect = peerBubble->rect();
						const auto radius = rect.height() / 2;
						p.drawRoundedRect(rect, radius, radius);
					}, peerBubble->lifetime());
				}
				Ui::AddSkip(box->verticalLayout());
				Ui::AddSkip(box->verticalLayout());
				box->addButton(tr::lng_box_ok(), [=] { box->closeBox(); });
			};

			button->setClickedCallback([=] {
				_show->showBox(Box(detailsBox));
			});
			wrap->geometryValue(
			) | rpl::start_with_next([=](const QRect &g) {
				const auto &padding = st::boxRowPadding;
				const auto majorTop = (g.height() - majorLabel->height()) / 2;
				minorLabel->moveToRight(
					padding.right(),
					majorTop + st::channelEarnHistoryMinorLabelSkip);
				majorLabel->moveToRight(
					padding.right() + minorLabel->width(),
					majorTop);
				const auto rightWrapPadding = rect::m::sum::h(padding)
					+ minorLabel->width()
					+ majorLabel->width();
				wrap->setPadding(
					st::channelEarnHistoryOuter
						+ QMargins(padding.left(), 0, rightWrapPadding, 0));
				button->resize(g.size());
				button->lower();
			}, wrap->lifetime());
		};
		const auto handleSlice = [=](const Data::EarnHistorySlice &slice) {
			for (const auto &entry : slice.list) {
				addHistoryEntry(
					entry,
					(entry.type == Data::EarnHistoryEntry::Type::In)
						? tr::lng_channel_earn_history_in
						: (entry.type == Data::EarnHistoryEntry::Type::Return)
						? tr::lng_channel_earn_history_return
						: tr::lng_channel_earn_history_out);
			}
			historyList->resizeToWidth(container->width());
		};
		handleSlice(data.firstHistorySlice);
		if (!data.firstHistorySlice.allLoaded) {
			struct ShowMoreState final {
				ShowMoreState(not_null<ChannelData*> channel)
				: api(channel) {
				}
				Api::EarnStatistics api;
				bool loading = false;
				Data::EarnHistorySlice::OffsetToken token;
				rpl::variable<int> showed = 0;
			};
			const auto state = lifetime().make_state<ShowMoreState>(channel);
			state->token = data.firstHistorySlice.token;
			state->showed = data.firstHistorySlice.list.size();
			const auto max = data.firstHistorySlice.total;
			const auto wrap = container->add(
				object_ptr<Ui::SlideWrap<Ui::SettingsButton>>(
					container,
					object_ptr<Ui::SettingsButton>(
						container,
						tr::lng_channel_earn_history_show_more(
							lt_count,
							state->showed.value(
							) | rpl::map(
								max - rpl::mappers::_1
							) | tr::to_count()),
						st::statisticsShowMoreButton)));
			const auto button = wrap->entity();
			AddArrow(button);

			wrap->toggle(true, anim::type::instant);
			const auto handleReceived = [=](Data::EarnHistorySlice slice) {
				state->loading = false;
				handleSlice(slice);
				wrap->toggle(!slice.allLoaded, anim::type::instant);
				state->token = slice.token;
				state->showed = state->showed.current() + slice.list.size();
			};
			button->setClickedCallback([=] {
				if (!state->loading) {
					state->loading = true;
					state->api.requestHistory(state->token, handleReceived);
				}
			});
		}
	}
	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	if (channel) {
		constexpr auto kMaxCPM = 50; // Debug.
		const auto requiredLevel = Data::LevelLimits(session)
			.channelRestrictSponsoredLevelMin();
		const auto &phrase = tr::lng_channel_earn_off;
		const auto button = container->add(object_ptr<Ui::SettingsButton>(
			container,
			phrase(),
			st::settingsButtonNoIconLocked));
		const auto toggled = lifetime().make_state<rpl::event_stream<bool>>();
		const auto isLocked = channel->levelHint() < requiredLevel;
		const auto reason = Ui::AskBoostReason{
			.data = Ui::AskBoostCpm{ .requiredLevel = requiredLevel },
		};

		AddLevelBadge(
			requiredLevel,
			button,
			nullptr,
			channel,
			QMargins(st::boxRowPadding.left(), 0, 0, 0),
			phrase());

		button->toggleOn(rpl::single(
			data.switchedOff
		) | rpl::then(toggled->events()));
		button->setToggleLocked(isLocked);

		button->toggledChanges(
		) | rpl::start_with_next([=](bool value) {
			if (isLocked && value) {
				toggled->fire(false);
				CheckBoostLevel(
					_controller->uiShow(),
					_peer,
					[=](int level) {
						return (level < requiredLevel)
							? std::make_optional(reason)
							: std::nullopt;
					},
					[] {});
			}
			if (!isLocked) {
				Api::RestrictSponsored(channel, value, [=](const QString &e) {
					toggled->fire(false);
					_controller->uiShow()->showToast(e);
				});
			}
		}, button->lifetime());

		Ui::AddSkip(container);
		Ui::AddDividerText(container, tr::lng_channel_earn_off_about());
	}
	Ui::AddSkip(container);

	Ui::ToggleChildrenVisibility(container, true);
	Ui::RpWidget::resizeToWidth(width());
}

void InnerWidget::saveState(not_null<Memento*> memento) {
	memento->setState(base::take(_state));
}

void InnerWidget::restoreState(not_null<Memento*> memento) {
	_state = memento->state();
	if (_state) {
		fill();
	} else {
		load();
	}
	Ui::RpWidget::resizeToWidth(width());
}

rpl::producer<Ui::ScrollToRequest> InnerWidget::scrollToRequests() const {
	return _scrollToRequests.events();
}

auto InnerWidget::showRequests() const -> rpl::producer<ShowRequest> {
	return _showRequests.events();
}

void InnerWidget::showFinished() {
	_showFinished.fire({});
}

void InnerWidget::setInnerFocus() {
	_focusRequested.fire({});
}

not_null<PeerData*> InnerWidget::peer() const {
	return _peer;
}

} // namespace Info::ChannelEarn
