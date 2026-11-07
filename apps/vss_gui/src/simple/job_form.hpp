//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include "backend/studio_backend.hpp"
#include "simple/job_plan.hpp"
#include "simple/setup_kinds.hpp"

#include <QHash>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QVBoxLayout;

namespace vss::gui
{
class NoteBanner;
class PickCard;
class ResultSentence;
} // namespace vss::gui

namespace vss::gui::simple
{

/// What a setup remembers beyond its ports and links, for this session only
/// (see `08-engine-gaps.md`).
struct setup_prefs
{
    QString name;
    bool type_here = false;
    bool show_data = false;
};

/// The ends of a job drawn as two columns joined by lines.
class WireDiagram final : public QWidget
{
    Q_OBJECT

public:
    enum class arrows
    {
        both,
        right,
        line,
        turns
    };

    explicit WireDiagram(QWidget* parent = nullptr);

    void set_ends(const QStringList& left, const QStringList& right,
                  arrows style);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QStringList left_;
    QStringList right_;
    arrows style_ = arrows::both;
};

/// One form for every job: the picture, the ends, the device settings, the
/// options and the result sentence, with Cancel and Create in a header that
/// stays at the top.  With `previous` it changes that setup instead.
class JobForm final : public QWidget
{
    Q_OBJECT

public:
    JobForm(StudioBackend& backend, job kind,
            const std::optional<setup_view>& previous = std::nullopt,
            const setup_prefs& prefs = {}, QWidget* parent = nullptr);

    job kind() const noexcept { return kind_; }
    const job_plan& plan() const noexcept { return plan_; }
    bool editing() const noexcept { return previous_.has_value(); }

    QPushButton* create_button() const noexcept { return create_; }
    QString result_text() const;
    QString blocker() const { return blocker_; }

    /// Test and keyboard hooks.
    void add_program();
    void add_device();
    void remove_end(bool second, int index);
    void set_label(bool second, int index, const QString& label);
    void set_device(int index, const QString& path);
    void set_echo(bool on);
    void set_extras(bool type_here, bool show_data);

    /// Builds or changes the setup.  True on success.
    bool submit();

Q_SIGNALS:
    void cancelled();
    /// The error offered "Set up Virtual Serial Studio" and it was chosen.
    void driver_setup_requested();
    void done(vss::endpoint_id port,
              const vss::gui::simple::setup_prefs& prefs);

private:
    struct list_spec
    {
        QString heading;
        bool device = false;
        int min = 0;
        int max = 0;
        bool devices_too = false;
    };

    void build();
    void rebuild_ends();
    void build_list(bool second);
    QWidget* program_row(bool second, int index);
    QWidget* device_row(bool second, int index, bool allow_network);
    void update_all();
    void update_diagram();
    void update_result();
    void update_changes();
    void validate();

    std::optional<list_spec> spec(bool second) const;
    std::vector<end_plan>& list(bool second);
    const std::vector<end_plan>& list(bool second) const;
    end_plan new_program();
    end_plan new_device();
    std::vector<unsigned> free_com_numbers(const end_plan* keep) const;
    QStringList free_devices() const;
    QString end_name(const end_plan& end) const;
    QStringList end_names(const std::vector<end_plan>& ends) const;
    bool com_numbers_shown() const;

    StudioBackend& backend_;
    job kind_;
    std::optional<setup_view> previous_;
    setup_prefs prefs_;
    job_plan plan_;
    QHash<vss::endpoint_id, QString> original_labels_;
    QString blocker_;

    QPushButton* create_ = nullptr;
    QLabel* blocker_label_ = nullptr;
    WireDiagram* diagram_ = nullptr;
    QWidget* ends_ = nullptr;
    QVBoxLayout* ends_layout_ = nullptr;
    PickCard* answer_type_ = nullptr;
    PickCard* answer_echo_ = nullptr;
    QSpinBox* listen_ = nullptr;
    QComboBox* baud_ = nullptr;
    QWidget* device_fields_ = nullptr;
    QComboBox* data_bits_ = nullptr;
    QComboBox* parity_ = nullptr;
    QComboBox* stop_bits_ = nullptr;
    QComboBox* flow_ = nullptr;
    QLineEdit* name_ = nullptr;
    QCheckBox* type_here_ = nullptr;
    QCheckBox* show_data_ = nullptr;
    NoteBanner* changes_ = nullptr;
    ResultSentence* result_ = nullptr;
    NoteBanner* error_ = nullptr;
};

} // namespace vss::gui::simple
