#include "settings_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace rex::gui {

// ---------------------------------------------------------------------------
// Helper: browse button wired to a QLineEdit
// ---------------------------------------------------------------------------
static QPushButton* makeBrowseButton(QLineEdit* target, QWidget* parent,
                                     bool pick_dir = false) {
    auto* btn = new QPushButton(QObject::tr("Browse…"), parent);
    btn->setFixedWidth(80);
    QObject::connect(btn, &QPushButton::clicked, parent, [target, pick_dir, parent]() {
        QString path = pick_dir
            ? QFileDialog::getExistingDirectory(parent, QObject::tr("Select Directory"),
                                                target->text())
            : QFileDialog::getOpenFileName(parent, QObject::tr("Select File"),
                                           target->text());
        if (!path.isEmpty())
            target->setText(path);
    });
    return btn;
}

// Helper: HBox with a line-edit and browse button
static QWidget* makePathRow(QLineEdit** out_edit, QWidget* parent,
                             bool pick_dir = false) {
    auto* row  = new QWidget(parent);
    auto* hbox = new QHBoxLayout(row);
    hbox->setContentsMargins(0, 0, 0, 0);
    auto* edit = new QLineEdit(row);
    hbox->addWidget(edit);
    hbox->addWidget(makeBrowseButton(edit, parent, pick_dir));
    if (out_edit) *out_edit = edit;
    return row;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Settings"));
    resize(820, 560);
    setMinimumSize(640, 440);

    // ---- Top-level layout --------------------------------------------------
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- Inner split (sidebar | pages) -------------------------------------
    auto* split = new QHBoxLayout;
    split->setContentsMargins(0, 0, 0, 0);
    split->setSpacing(0);
    root->addLayout(split, 1);

    // ---- Sidebar panel -----------------------------------------------------
    auto* sidebar_panel = new QWidget(this);
    sidebar_panel->setFixedWidth(190);
    sidebar_panel->setObjectName("sidebarPanel");
    sidebar_panel->setStyleSheet(
        "#sidebarPanel { background: #1e1e2e; }"
        "QListWidget { background: transparent; border: none; color: #cdd6f4;"
        "              font-size: 13px; padding: 8px 0; }"
        "QListWidget::item { padding: 8px 16px; border-radius: 6px; margin: 2px 8px; }"
        "QListWidget::item:selected { background: #89b4fa; color: #1e1e2e; font-weight: bold; }"
        "QListWidget::item:hover:!selected { background: #313244; }"
        "QLineEdit { background: #313244; border: 1px solid #45475a;"
        "            border-radius: 6px; color: #cdd6f4; padding: 5px 8px;"
        "            margin: 8px 10px 4px 10px; font-size: 12px; }"
    );

    auto* sidebar_layout = new QVBoxLayout(sidebar_panel);
    sidebar_layout->setContentsMargins(0, 8, 0, 8);
    sidebar_layout->setSpacing(4);

    search_bar_ = new QLineEdit(sidebar_panel);
    search_bar_->setPlaceholderText(tr("Search settings…"));
    sidebar_layout->addWidget(search_bar_);

    sidebar_ = new QListWidget(sidebar_panel);
    sidebar_->setFocusPolicy(Qt::NoFocus);
    sidebar_layout->addWidget(sidebar_);

    split->addWidget(sidebar_panel);

    // Divider
    auto* divider = new QFrame(this);
    divider->setFrameShape(QFrame::VLine);
    divider->setStyleSheet("color: #45475a;");
    split->addWidget(divider);

    // ---- Pages panel -------------------------------------------------------
    pages_ = new QStackedWidget(this);
    pages_->setStyleSheet(
        "QStackedWidget { background: #181825; }"
        "QWidget { color: #cdd6f4; }"
        "QGroupBox { color: #89b4fa; font-weight: bold; border: 1px solid #313244;"
        "            border-radius: 8px; margin-top: 12px; padding-top: 8px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 12px;"
        "                   padding: 0 4px; }"
        "QLabel { color: #cdd6f4; }"
        "QLineEdit, QPlainTextEdit, QSpinBox, QComboBox {"
        "    background: #313244; border: 1px solid #45475a; border-radius: 5px;"
        "    color: #cdd6f4; padding: 4px 6px; }"
        "QLineEdit:focus, QPlainTextEdit:focus, QSpinBox:focus, QComboBox:focus {"
        "    border: 1px solid #89b4fa; }"
        "QCheckBox { color: #cdd6f4; spacing: 8px; }"
        "QCheckBox::indicator { width: 16px; height: 16px; border-radius: 3px;"
        "    border: 1px solid #45475a; background: #313244; }"
        "QCheckBox::indicator:checked { background: #89b4fa; border-color: #89b4fa; }"
        "QPushButton { background: #313244; border: 1px solid #45475a;"
        "    border-radius: 5px; color: #cdd6f4; padding: 4px 12px; }"
        "QPushButton:hover { background: #45475a; }"
        "QPushButton:pressed { background: #585b70; }"
        "QSlider::groove:horizontal { height: 4px; background: #45475a;"
        "    border-radius: 2px; }"
        "QSlider::handle:horizontal { background: #89b4fa; width: 14px; height: 14px;"
        "    border-radius: 7px; margin: -5px 0; }"
        "QSlider::sub-page:horizontal { background: #89b4fa; border-radius: 2px; }"
        "QTableWidget { background: #1e1e2e; gridline-color: #313244;"
        "    border: 1px solid #313244; border-radius: 5px; }"
        "QTableWidget::item { padding: 4px 8px; }"
        "QHeaderView::section { background: #313244; color: #89b4fa;"
        "    border: none; padding: 6px 8px; font-weight: bold; }"
    );
    split->addWidget(pages_, 1);

    // ---- Build sidebar items and pages -------------------------------------
    createPages();

    // ---- Button box --------------------------------------------------------
    auto* btn_box = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply,
        this);
    btn_box->setStyleSheet(
        "QDialogButtonBox { background: #1e1e2e; padding: 10px 16px; }"
        "QPushButton { min-width: 80px; }"
    );
    root->addWidget(btn_box);

    connect(btn_box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btn_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // Apply just emits accepted for now; can be wired to save later
    connect(btn_box->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, [this]() { /* TODO: apply without closing */ });

    // ---- Sidebar → page switching ------------------------------------------
    connect(sidebar_, &QListWidget::currentRowChanged,
            pages_, &QStackedWidget::setCurrentIndex);

    // ---- Search filter -----------------------------------------------------
    connect(search_bar_, &QLineEdit::textChanged,
            this, &SettingsDialog::filterSettings);

    sidebar_->setCurrentRow(0);
}

// ---------------------------------------------------------------------------
// createPages — populates sidebar_ and pages_
// ---------------------------------------------------------------------------
void SettingsDialog::createPages() {
    struct Entry { QString label; QWidget* (SettingsDialog::*fn)(); };
    const std::initializer_list<Entry> entries = {
        { tr("General"),     &SettingsDialog::createGeneralPage     },
        { tr("Display"),     &SettingsDialog::createDisplayPage     },
        { tr("Performance"), &SettingsDialog::createPerformancePage },
        { tr("Network"),     &SettingsDialog::createNetworkPage     },
        { tr("Input"),       &SettingsDialog::createInputPage       },
        { tr("Frida"),       &SettingsDialog::createFridaPage       },
        { tr("Advanced"),    &SettingsDialog::createAdvancedPage    },
    };

    for (auto& [label, fn] : entries) {
        sidebar_->addItem(new QListWidgetItem(label, sidebar_));
        pages_->addWidget((this->*fn)());
    }
}

// ---------------------------------------------------------------------------
// Utility: wrap a form layout in a scroll area with a title
// ---------------------------------------------------------------------------
static QWidget* wrapPage(const QString& title, QFormLayout* form) {
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setContentsMargins(24, 20, 24, 20);
    vbox->setSpacing(16);

    auto* heading = new QLabel(title);
    heading->setStyleSheet("font-size: 18px; font-weight: bold; color: #cdd6f4;");
    vbox->addWidget(heading);

    auto* sep = new QFrame;
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("color: #313244;");
    vbox->addWidget(sep);

    auto* container = new QWidget;
    container->setLayout(form);
    vbox->addWidget(container);
    vbox->addStretch();
    return page;
}

// Small warning badge shown next to labels that require VM restart
static QWidget* labelWithWarning(const QString& text, QWidget* parent) {
    auto* w    = new QWidget(parent);
    auto* hbox = new QHBoxLayout(w);
    hbox->setContentsMargins(0, 0, 0, 0);
    hbox->setSpacing(4);
    hbox->addWidget(new QLabel(text, w));
    auto* warn = new QLabel("⚠", w);
    warn->setToolTip(QObject::tr("Requires VM restart"));
    warn->setStyleSheet("color: #f9e2af; font-size: 12px;");
    hbox->addWidget(warn);
    hbox->addStretch();
    return w;
}

// ---------------------------------------------------------------------------
// 1. General
// ---------------------------------------------------------------------------
QWidget* SettingsDialog::createGeneralPage() {
    auto* form = new QFormLayout;
    form->setSpacing(12);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto* theme = new QComboBox;
    theme->addItems({ tr("Dark"), tr("Light"), tr("System") });
    form->addRow(tr("Theme:"), theme);

    QLineEdit* qemu_edit = nullptr;
    form->addRow(tr("QEMU binary:"), makePathRow(&qemu_edit, nullptr));

    auto* auto_start = new QCheckBox(tr("Auto-start VM on launch"));
    form->addRow(QString(), auto_start);

    auto* check_updates = new QCheckBox(tr("Check for updates at startup"));
    form->addRow(QString(), check_updates);

    return wrapPage(tr("General"), form);
}

// ---------------------------------------------------------------------------
// 2. Display
// ---------------------------------------------------------------------------
QWidget* SettingsDialog::createDisplayPage() {
    auto* form = new QFormLayout;
    form->setSpacing(12);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto* preset = new QComboBox;
    preset->addItems({ tr("Phone (1080×1920)"), tr("Tablet (1600×2560)"), tr("Custom") });
    form->addRow(labelWithWarning(tr("Resolution preset:"), nullptr), preset);

    auto* width_spin = new QSpinBox;
    width_spin->setRange(320, 7680);
    width_spin->setValue(1080);
    width_spin->setSuffix(tr(" px"));
    form->addRow(labelWithWarning(tr("Width:"), nullptr), width_spin);

    auto* height_spin = new QSpinBox;
    height_spin->setRange(320, 7680);
    height_spin->setValue(1920);
    height_spin->setSuffix(tr(" px"));
    form->addRow(labelWithWarning(tr("Height:"), nullptr), height_spin);

    auto* dpi_spin = new QSpinBox;
    dpi_spin->setRange(120, 640);
    dpi_spin->setValue(420);
    form->addRow(labelWithWarning(tr("DPI:"), nullptr), dpi_spin);

    auto* fps = new QComboBox;
    fps->addItems({ tr("30"), tr("60"), tr("120"), tr("Unlimited") });
    fps->setCurrentIndex(1);
    form->addRow(tr("FPS limit:"), fps);

    // Disable width/height when preset != Custom
    auto toggleCustom = [width_spin, height_spin](int idx) {
        bool custom = (idx == 2);
        width_spin->setEnabled(custom);
        height_spin->setEnabled(custom);
        if (idx == 0) { width_spin->setValue(1080); height_spin->setValue(1920); }
        if (idx == 1) { width_spin->setValue(1600); height_spin->setValue(2560); }
    };
    connect(preset, &QComboBox::currentIndexChanged, toggleCustom);
    toggleCustom(0);

    return wrapPage(tr("Display"), form);
}

// ---------------------------------------------------------------------------
// 3. Performance
// ---------------------------------------------------------------------------
QWidget* SettingsDialog::createPerformancePage() {
    auto* form = new QFormLayout;
    form->setSpacing(12);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto* vcpu = new QSpinBox;
    vcpu->setRange(1, 16);
    vcpu->setValue(4);
    form->addRow(labelWithWarning(tr("vCPU count:"), nullptr), vcpu);

    auto* ram = new QComboBox;
    for (int mb : { 512, 1024, 2048, 4096, 8192, 16384 })
        ram->addItem(QString("%1 MB").arg(mb), mb);
    ram->setCurrentIndex(2); // 2048
    form->addRow(labelWithWarning(tr("RAM:"), nullptr), ram);

    auto* accel = new QComboBox;
    accel->addItems({ tr("Auto"), tr("HVF"), tr("KVM"), tr("WHPX"), tr("TCG") });
    form->addRow(labelWithWarning(tr("Accelerator:"), nullptr), accel);

    auto* cache = new QComboBox;
    cache->addItems({ tr("none"), tr("writethrough"), tr("writeback") });
    cache->setCurrentIndex(2);
    form->addRow(tr("Disk cache mode:"), cache);

    return wrapPage(tr("Performance"), form);
}

// ---------------------------------------------------------------------------
// 4. Network
// ---------------------------------------------------------------------------
QWidget* SettingsDialog::createNetworkPage() {
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setContentsMargins(24, 20, 24, 20);
    vbox->setSpacing(16);

    auto* heading = new QLabel(tr("Network"));
    heading->setStyleSheet("font-size: 18px; font-weight: bold; color: #cdd6f4;");
    vbox->addWidget(heading);

    auto* sep = new QFrame;
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("color: #313244;");
    vbox->addWidget(sep);

    // Port forwarding group
    auto* pf_group = new QGroupBox(tr("Port Forwarding"));
    auto* pf_vbox  = new QVBoxLayout(pf_group);
    pf_vbox->setSpacing(8);

    auto* table = new QTableWidget(0, 2, page);
    table->setHorizontalHeaderLabels({ tr("Host Port"), tr("Guest Port") });
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setMinimumHeight(160);
    pf_vbox->addWidget(table);

    auto* pf_btn_row = new QHBoxLayout;
    auto* add_btn    = new QPushButton(tr("+ Add Rule"), page);
    auto* rm_btn     = new QPushButton(tr("− Remove"), page);
    pf_btn_row->addWidget(add_btn);
    pf_btn_row->addWidget(rm_btn);
    pf_btn_row->addStretch();
    pf_vbox->addLayout(pf_btn_row);

    connect(add_btn, &QPushButton::clicked, page, [table]() {
        int row = table->rowCount();
        table->insertRow(row);
        table->setItem(row, 0, new QTableWidgetItem("8080"));
        table->setItem(row, 1, new QTableWidgetItem("8080"));
    });
    connect(rm_btn, &QPushButton::clicked, page, [table]() {
        int row = table->currentRow();
        if (row >= 0) table->removeRow(row);
    });

    vbox->addWidget(pf_group);

    // DNS / proxy group
    auto* misc_group = new QGroupBox(tr("Other"));
    auto* misc_form  = new QFormLayout(misc_group);
    misc_form->setSpacing(10);
    misc_form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto* dns = new QLineEdit;
    dns->setPlaceholderText("8.8.8.8");
    misc_form->addRow(tr("DNS server:"), dns);

    auto* proxy_chk  = new QCheckBox(tr("Enable HTTP proxy"));
    auto* proxy_host = new QLineEdit;
    proxy_host->setPlaceholderText("host:port");
    proxy_host->setEnabled(false);
    misc_form->addRow(proxy_chk);
    misc_form->addRow(tr("Proxy host:"), proxy_host);
    connect(proxy_chk, &QCheckBox::toggled, proxy_host, &QWidget::setEnabled);

    vbox->addWidget(misc_group);
    vbox->addStretch();
    return page;
}

// ---------------------------------------------------------------------------
// 5. Input
// ---------------------------------------------------------------------------
QWidget* SettingsDialog::createInputPage() {
    auto* form = new QFormLayout;
    form->setSpacing(12);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto* keymap = new QComboBox;
    keymap->addItems({ tr("Default"), tr("US QWERTY"), tr("US AZERTY"), tr("UK"), tr("DE") });
    form->addRow(tr("Keymap profile:"), keymap);

    auto* sens_row  = new QWidget;
    auto* sens_hbox = new QHBoxLayout(sens_row);
    sens_hbox->setContentsMargins(0, 0, 0, 0);
    auto* sens_slider = new QSlider(Qt::Horizontal);
    sens_slider->setRange(1, 100);
    sens_slider->setValue(50);
    auto* sens_label = new QLabel("50");
    sens_label->setFixedWidth(28);
    sens_hbox->addWidget(sens_slider);
    sens_hbox->addWidget(sens_label);
    connect(sens_slider, &QSlider::valueChanged,
            sens_label, [sens_label](int v){ sens_label->setText(QString::number(v)); });
    form->addRow(tr("Mouse sensitivity:"), sens_row);

    auto* gamepad = new QCheckBox(tr("Enable gamepad / controller support"));
    form->addRow(QString(), gamepad);

    return wrapPage(tr("Input"), form);
}

// ---------------------------------------------------------------------------
// 6. Frida
// ---------------------------------------------------------------------------
QWidget* SettingsDialog::createFridaPage() {
    auto* form = new QFormLayout;
    form->setSpacing(12);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    QLineEdit* frida_edit = nullptr;
    form->addRow(tr("Frida server path:"), makePathRow(&frida_edit, nullptr));

    auto* auto_frida = new QCheckBox(tr("Auto-start Frida server with VM"));
    form->addRow(QString(), auto_frida);

    auto* frida_port = new QSpinBox;
    frida_port->setRange(1024, 65535);
    frida_port->setValue(27042);
    form->addRow(tr("Port:"), frida_port);

    QLineEdit* script_dir = nullptr;
    form->addRow(tr("Script directory:"), makePathRow(&script_dir, nullptr, true));

    return wrapPage(tr("Frida"), form);
}

// ---------------------------------------------------------------------------
// 7. Advanced
// ---------------------------------------------------------------------------
QWidget* SettingsDialog::createAdvancedPage() {
    auto* form = new QFormLayout;
    form->setSpacing(12);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto* extra_args = new QPlainTextEdit;
    extra_args->setPlaceholderText(tr("e.g. -device usb-tablet -machine q35"));
    extra_args->setFixedHeight(90);
    form->addRow(labelWithWarning(tr("Extra QEMU args:"), nullptr), extra_args);

    auto* log_level = new QComboBox;
    log_level->addItems({ tr("Error"), tr("Warn"), tr("Info"), tr("Debug") });
    log_level->setCurrentIndex(2);
    form->addRow(tr("Log level:"), log_level);

    QLineEdit* snap_path = nullptr;
    form->addRow(tr("Snapshot save path:"), makePathRow(&snap_path, nullptr, true));

    return wrapPage(tr("Advanced"), form);
}

// ---------------------------------------------------------------------------
// filterSettings — hide sidebar items whose page has no matching text
// ---------------------------------------------------------------------------
void SettingsDialog::filterSettings(const QString& text) {
    if (text.isEmpty()) {
        for (int i = 0; i < sidebar_->count(); ++i)
            sidebar_->item(i)->setHidden(false);
        return;
    }

    const QString lower = text.toLower();
    for (int i = 0; i < sidebar_->count(); ++i) {
        QWidget* page = pages_->widget(i);
        // Gather all descendant text (labels, checkboxes, etc.)
        bool found = false;
        for (auto* lbl : page->findChildren<QLabel*>())
            if (lbl->text().toLower().contains(lower)) { found = true; break; }
        if (!found)
            for (auto* cb : page->findChildren<QCheckBox*>())
                if (cb->text().toLower().contains(lower)) { found = true; break; }
        if (!found)
            for (auto* gb : page->findChildren<QGroupBox*>())
                if (gb->title().toLower().contains(lower)) { found = true; break; }
        sidebar_->item(i)->setHidden(!found);
    }
}

} // namespace rex::gui
