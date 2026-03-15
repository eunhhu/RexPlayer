#include "settings_dialog.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace rex::gui {

SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent)
{
    setupUi();
}

SettingsDialog::SettingsDialog(const RexConfig& initial_config, QWidget* parent)
    : QDialog(parent)
{
    setupUi();
    loadConfig(initial_config);
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void SettingsDialog::setupUi() {
    setWindowTitle(tr("Settings"));
    setMinimumSize(480, 400);

    auto* main_layout = new QVBoxLayout(this);

    // Tab widget
    tab_widget_ = new QTabWidget(this);
    tab_widget_->addTab(createGeneralTab(),  tr("General"));
    tab_widget_->addTab(createDisplayTab(),  tr("Display"));
    tab_widget_->addTab(createNetworkTab(),  tr("Network"));
    tab_widget_->addTab(createAdvancedTab(), tr("Advanced"));
    main_layout->addWidget(tab_widget_);

    // Button box: OK, Cancel, Apply
    button_box_ = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply,
        this);
    main_layout->addWidget(button_box_);

    connect(button_box_, &QDialogButtonBox::accepted, this, &SettingsDialog::onOk);
    connect(button_box_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(button_box_->button(QDialogButtonBox::Apply),
            &QPushButton::clicked, this, &SettingsDialog::onApply);
}

QWidget* SettingsDialog::createGeneralTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);

    // CPU group
    auto* cpu_group = new QGroupBox(tr("Processor"), page);
    auto* cpu_layout = new QFormLayout(cpu_group);

    cpu_cores_spin_ = new QSpinBox(cpu_group);
    cpu_cores_spin_->setRange(1, 16);
    cpu_cores_spin_->setValue(2);
    cpu_cores_spin_->setSuffix(tr(" cores"));
    cpu_layout->addRow(tr("CPU Cores:"), cpu_cores_spin_);

    layout->addWidget(cpu_group);

    // Memory group
    auto* mem_group = new QGroupBox(tr("Memory"), page);
    auto* mem_layout = new QFormLayout(mem_group);

    ram_spin_ = new QSpinBox(mem_group);
    ram_spin_->setRange(512, 16384);
    ram_spin_->setSingleStep(256);
    ram_spin_->setValue(2048);
    ram_spin_->setSuffix(tr(" MB"));
    mem_layout->addRow(tr("RAM:"), ram_spin_);

    layout->addWidget(mem_group);
    layout->addStretch();

    return page;
}

QWidget* SettingsDialog::createDisplayTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);

    auto* res_group = new QGroupBox(tr("Resolution"), page);
    auto* res_layout = new QFormLayout(res_group);

    width_spin_ = new QSpinBox(res_group);
    width_spin_->setRange(320, 3840);
    width_spin_->setSingleStep(1);
    width_spin_->setValue(1080);
    width_spin_->setSuffix(tr(" px"));
    res_layout->addRow(tr("Width:"), width_spin_);

    height_spin_ = new QSpinBox(res_group);
    height_spin_->setRange(480, 3840);
    height_spin_->setSingleStep(1);
    height_spin_->setValue(1920);
    height_spin_->setSuffix(tr(" px"));
    res_layout->addRow(tr("Height:"), height_spin_);

    dpi_spin_ = new QSpinBox(res_group);
    dpi_spin_->setRange(120, 640);
    dpi_spin_->setSingleStep(40);
    dpi_spin_->setValue(440);
    dpi_spin_->setSuffix(tr(" dpi"));
    res_layout->addRow(tr("DPI:"), dpi_spin_);

    layout->addWidget(res_group);

    auto* orient_group = new QGroupBox(tr("Orientation"), page);
    auto* orient_layout = new QFormLayout(orient_group);

    orientation_combo_ = new QComboBox(orient_group);
    orientation_combo_->addItem(tr("Portrait"),  0);
    orientation_combo_->addItem(tr("Landscape"), 1);
    orientation_combo_->addItem(tr("Auto"),      2);
    orient_layout->addRow(tr("Mode:"), orientation_combo_);

    layout->addWidget(orient_group);
    layout->addStretch();

    return page;
}

QWidget* SettingsDialog::createNetworkTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);

    auto* proxy_group = new QGroupBox(tr("HTTP Proxy"), page);
    auto* proxy_layout = new QFormLayout(proxy_group);

    proxy_check_ = new QCheckBox(tr("Enable proxy"), proxy_group);
    proxy_layout->addRow(proxy_check_);

    proxy_host_edit_ = new QLineEdit(proxy_group);
    proxy_host_edit_->setPlaceholderText(tr("e.g. 127.0.0.1"));
    proxy_layout->addRow(tr("Host:"), proxy_host_edit_);

    proxy_port_spin_ = new QSpinBox(proxy_group);
    proxy_port_spin_->setRange(1, 65535);
    proxy_port_spin_->setValue(8080);
    proxy_layout->addRow(tr("Port:"), proxy_port_spin_);

    // Enable/disable fields based on checkbox
    auto updateProxyFields = [this](bool enabled) {
        proxy_host_edit_->setEnabled(enabled);
        proxy_port_spin_->setEnabled(enabled);
    };
    connect(proxy_check_, &QCheckBox::toggled, proxy_group, updateProxyFields);
    updateProxyFields(false);

    layout->addWidget(proxy_group);
    layout->addStretch();

    return page;
}

QWidget* SettingsDialog::createAdvancedTab() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);

    auto* debug_group = new QGroupBox(tr("Debugging"), page);
    auto* debug_layout = new QFormLayout(debug_group);

    frida_check_ = new QCheckBox(tr("Enable Frida instrumentation"), debug_group);
    debug_layout->addRow(frida_check_);

    root_check_ = new QCheckBox(tr("Enable root access (adb root)"), debug_group);
    debug_layout->addRow(root_check_);

    layout->addWidget(debug_group);

    auto* security_group = new QGroupBox(tr("Security"), page);
    auto* security_layout = new QFormLayout(security_group);

    selinux_combo_ = new QComboBox(security_group);
    selinux_combo_->addItem(tr("Disabled"),   0);
    selinux_combo_->addItem(tr("Permissive"), 1);
    selinux_combo_->addItem(tr("Enforcing"),  2);
    selinux_combo_->setCurrentIndex(1); // Default: Permissive
    security_layout->addRow(tr("SELinux Mode:"), selinux_combo_);

    layout->addWidget(security_group);
    layout->addStretch();

    return page;
}

// ---------------------------------------------------------------------------
// Config load/save
// ---------------------------------------------------------------------------

void SettingsDialog::loadConfig(const RexConfig& config) {
    cpu_cores_spin_->setValue(static_cast<int>(config.cpu_cores));
    ram_spin_->setValue(static_cast<int>(config.ram_mb));

    width_spin_->setValue(static_cast<int>(config.display_width));
    height_spin_->setValue(static_cast<int>(config.display_height));
    dpi_spin_->setValue(static_cast<int>(config.dpi));

    // Set orientation combo by data value
    for (int i = 0; i < orientation_combo_->count(); ++i) {
        if (orientation_combo_->itemData(i).toInt() == config.orientation) {
            orientation_combo_->setCurrentIndex(i);
            break;
        }
    }

    proxy_check_->setChecked(config.proxy_enabled);
    proxy_host_edit_->setText(config.proxy_host);
    proxy_port_spin_->setValue(config.proxy_port);

    frida_check_->setChecked(config.frida_enabled);
    root_check_->setChecked(config.root_enabled);

    for (int i = 0; i < selinux_combo_->count(); ++i) {
        if (selinux_combo_->itemData(i).toInt() == config.selinux_mode) {
            selinux_combo_->setCurrentIndex(i);
            break;
        }
    }
}

RexConfig SettingsDialog::currentConfig() const {
    RexConfig config;

    config.cpu_cores = static_cast<uint32_t>(cpu_cores_spin_->value());
    config.ram_mb    = static_cast<uint32_t>(ram_spin_->value());

    config.display_width  = static_cast<uint32_t>(width_spin_->value());
    config.display_height = static_cast<uint32_t>(height_spin_->value());
    config.dpi            = static_cast<uint32_t>(dpi_spin_->value());
    config.orientation    = orientation_combo_->currentData().toInt();

    config.proxy_enabled = proxy_check_->isChecked();
    config.proxy_host    = proxy_host_edit_->text();
    config.proxy_port    = static_cast<uint16_t>(proxy_port_spin_->value());

    config.frida_enabled = frida_check_->isChecked();
    config.root_enabled  = root_check_->isChecked();
    config.selinux_mode  = selinux_combo_->currentData().toInt();

    return config;
}

// ---------------------------------------------------------------------------
// Button handlers
// ---------------------------------------------------------------------------

void SettingsDialog::onApply() {
    emit configApplied(currentConfig());
}

void SettingsDialog::onOk() {
    emit configApplied(currentConfig());
    accept();
}

} // namespace rex::gui
