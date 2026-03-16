#pragma once

#include <QDialog>
#include <QTabWidget>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QDialogButtonBox>

namespace rex::gui {

#if defined(__aarch64__)
inline constexpr uint32_t kDefaultCpuCores = 1;
#else
inline constexpr uint32_t kDefaultCpuCores = 2;
#endif
inline constexpr uint32_t kDefaultRamMb = 2048;
inline constexpr uint32_t kDefaultDisplayWidth = 1080;
inline constexpr uint32_t kDefaultDisplayHeight = 1920;
inline constexpr uint32_t kDefaultDisplayDpi = 440;

/// Configuration data exchanged with SettingsDialog.
/// Mirrors the TOML config structure for RexPlayer.
struct RexConfig {
    // General
    uint32_t cpu_cores  = kDefaultCpuCores;
    uint32_t ram_mb     = kDefaultRamMb;

    // Display
    uint32_t display_width  = kDefaultDisplayWidth;
    uint32_t display_height = kDefaultDisplayHeight;
    uint32_t dpi            = kDefaultDisplayDpi;
    int orientation         = 0; // 0=Portrait, 1=Landscape, 2=Auto

    // Network
    bool proxy_enabled       = false;
    QString proxy_host;
    uint16_t proxy_port      = 8080;

    // Advanced
    bool frida_enabled       = false;
    int selinux_mode         = 1; // 0=Disabled, 1=Permissive, 2=Enforcing
    bool root_enabled        = false;
};

/// SettingsDialog provides a tabbed UI for configuring the VM.
///
/// Tabs:
///   General  — CPU cores, RAM size
///   Display  — Resolution, DPI, orientation
///   Network  — HTTP proxy settings
///   Advanced — Frida toggle, SELinux mode, root access
///
/// The dialog operates on a RexConfig snapshot. Changes are only
/// applied when the user clicks OK or Apply.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    SettingsDialog(const RexConfig& initial_config, QWidget* parent = nullptr);
    ~SettingsDialog() override = default;

    /// Load settings from a config struct into the UI controls
    void loadConfig(const RexConfig& config);

    /// Read current UI values into a config struct
    RexConfig currentConfig() const;

signals:
    /// Emitted when settings are applied (OK or Apply button)
    void configApplied(const RexConfig& config);

private slots:
    void onApply();
    void onOk();

private:
    void setupUi();
    QWidget* createGeneralTab();
    QWidget* createDisplayTab();
    QWidget* createNetworkTab();
    QWidget* createAdvancedTab();

    // Tab widget
    QTabWidget* tab_widget_ = nullptr;
    QDialogButtonBox* button_box_ = nullptr;

    // General tab
    QSpinBox* cpu_cores_spin_ = nullptr;
    QSpinBox* ram_spin_ = nullptr;

    // Display tab
    QSpinBox* width_spin_ = nullptr;
    QSpinBox* height_spin_ = nullptr;
    QSpinBox* dpi_spin_ = nullptr;
    QComboBox* orientation_combo_ = nullptr;

    // Network tab
    QCheckBox* proxy_check_ = nullptr;
    QLineEdit* proxy_host_edit_ = nullptr;
    QSpinBox* proxy_port_spin_ = nullptr;

    // Advanced tab
    QCheckBox* frida_check_ = nullptr;
    QComboBox* selinux_combo_ = nullptr;
    QCheckBox* root_check_ = nullptr;
};

} // namespace rex::gui
