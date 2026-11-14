//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_WIDGETS_CHIPS_HPP
#define VSS_GUI_WIDGETS_CHIPS_HPP

#include "widgets/common.hpp"

#include <QLabel>
#include <QSize>
#include <QString>
#include <QWidget>

namespace vss::gui
{

/// Port state pill: text plus a shape, never colour alone.
class StatusChip final : public QLabel
{
public:
    enum class kind
    {
        open,
        healthy,
        waiting,
        connected,
        studio_only,
        closed,
        failed,
        fixture,
        /// A plain fact: grey with a filled dot, where `closed` has a hollow
        /// one.
        neutral
    };

    explicit StatusChip(QWidget* parent = nullptr);
    StatusChip(const QString& text, kind value, QWidget* parent = nullptr);

    void set_kind(kind value);
    kind get_kind() const noexcept { return kind_; }
    void set_text(const QString& text);
    void set_small(bool small);
    bool is_small() const noexcept { return small_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    kind kind_ = kind::open;
    bool small_ = false;
};

/// RX, TX and DROP badge for a monitor row.
class DirectionBadge final : public QLabel
{
public:
    enum class direction
    {
        rx,
        tx,
        drop
    };

    explicit DirectionBadge(QWidget* parent = nullptr);
    explicit DirectionBadge(direction value, QWidget* parent = nullptr);

    void set_direction(direction value);
    direction get_direction() const noexcept { return direction_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    direction direction_ = direction::rx;
};

/// Small tag: New, Advanced or a custom label.
class Tag final : public QLabel
{
public:
    enum class variant
    {
        s,
        a,
        n,
        ok
    };

    explicit Tag(QWidget* parent = nullptr);
    Tag(const QString& text, variant value, QWidget* parent = nullptr);

    void set_variant(variant value);
    void set_text(const QString& text);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    variant variant_ = variant::s;
};

/// The uppercase ADVANCED badge.
class Badge final : public QLabel
{
public:
    explicit Badge(const QString& text = QStringLiteral("ADVANCED"),
                   QWidget* parent = nullptr);

    void set_text(const QString& text);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
};

/// A standalone state dot or cross, for rows and headings.
class StateDot final : public QWidget
{
public:
    enum class kind
    {
        filled,
        hollow,
        cross
    };

    explicit StateDot(QWidget* parent = nullptr);

    void set_kind(kind value);
    void set_colour(colour_role role);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    kind kind_ = kind::filled;
    colour_role colour_ = colour_role::ink3;
};

/// A keyboard key rendered as a small cap.
class KeyCap final : public QLabel
{
public:
    explicit KeyCap(const QString& text = {}, QWidget* parent = nullptr);

    void set_text(const QString& text);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
};

} // namespace vss::gui

#endif // VSS_GUI_WIDGETS_CHIPS_HPP
