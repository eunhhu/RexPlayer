#pragma once

#include <QDialog>
#include <QTableWidget>
#include <QPushButton>
#include <QComboBox>
#include <QKeySequenceEdit>
#include <QJsonObject>
#include <QJsonArray>

#include <string>
#include <unordered_map>
#include <vector>

namespace rex::gui {

/// A single key binding entry
struct KeyBinding {
    QString action_name;       // e.g., "Move Up", "Jump", "Fire"
    int qt_key = 0;            // Qt::Key_W
    uint16_t linux_keycode = 0; // Linux KEY_W or custom target
    QString category;          // "Navigation", "Gaming", "Android"
};

/// Persistent keymap profile
struct KeymapProfile {
    QString name;              // "Default", "FPS", "MOBA"
    std::vector<KeyBinding> bindings;
};

/// Keymap editor dialog — allows users to customize key bindings
///
/// Features:
/// - View and modify all key mappings in a table
/// - Multiple profiles (Default, FPS, MOBA, Custom)
/// - Save/load profiles to JSON
/// - "Record key" mode for easy binding
/// - Reset to defaults
class KeymapEditor : public QDialog {
    Q_OBJECT

public:
    explicit KeymapEditor(QWidget* parent = nullptr);

    /// Load a keymap profile
    void loadProfile(const KeymapProfile& profile);

    /// Get the current profile
    KeymapProfile currentProfile() const;

    /// Load profiles from a JSON file
    static std::vector<KeymapProfile> loadFromFile(const QString& path);

    /// Save profiles to a JSON file
    static bool saveToFile(const QString& path, const std::vector<KeymapProfile>& profiles);

    /// Get the default profile
    static KeymapProfile defaultProfile();

    /// Get the FPS gaming profile
    static KeymapProfile fpsProfile();

    /// Get the MOBA gaming profile
    static KeymapProfile mobaProfile();

signals:
    /// Emitted when the user applies changes
    void profileApplied(const KeymapProfile& profile);

private slots:
    void onProfileChanged(int index);
    void onAddBinding();
    void onRemoveBinding();
    void onResetDefaults();
    void onApply();
    void onExport();
    void onImport();

private:
    void setupUi();
    void populateTable();

    QComboBox* profile_combo_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPushButton* add_btn_ = nullptr;
    QPushButton* remove_btn_ = nullptr;
    QPushButton* reset_btn_ = nullptr;
    QPushButton* export_btn_ = nullptr;
    QPushButton* import_btn_ = nullptr;

    std::vector<KeymapProfile> profiles_;
    int current_profile_idx_ = 0;
};

} // namespace rex::gui
