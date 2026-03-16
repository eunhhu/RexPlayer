#include "keymap_editor.h"
#include "input_handler.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QVBoxLayout>

namespace rex::gui {

KeymapEditor::KeymapEditor(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Keymap Editor"));
    setMinimumSize(600, 500);

    // Initialize default profiles
    profiles_.push_back(defaultProfile());
    profiles_.push_back(fpsProfile());
    profiles_.push_back(mobaProfile());

    setupUi();
    populateTable();
}

void KeymapEditor::setupUi() {
    auto* layout = new QVBoxLayout(this);

    // Profile selector
    auto* profile_layout = new QHBoxLayout();
    profile_layout->addWidget(new QLabel(tr("Profile:")));
    profile_combo_ = new QComboBox();
    for (const auto& p : profiles_) {
        profile_combo_->addItem(p.name);
    }
    connect(profile_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &KeymapEditor::onProfileChanged);
    profile_layout->addWidget(profile_combo_, 1);
    layout->addLayout(profile_layout);

    // Key bindings table
    table_ = new QTableWidget(0, 4);
    table_->setHorizontalHeaderLabels({
        tr("Action"), tr("Category"), tr("Key"), tr("Linux Code")
    });
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(table_);

    // Buttons row
    auto* btn_layout = new QHBoxLayout();
    add_btn_ = new QPushButton(tr("Add Binding"));
    remove_btn_ = new QPushButton(tr("Remove"));
    reset_btn_ = new QPushButton(tr("Reset to Defaults"));
    export_btn_ = new QPushButton(tr("Export..."));
    import_btn_ = new QPushButton(tr("Import..."));

    connect(add_btn_, &QPushButton::clicked, this, &KeymapEditor::onAddBinding);
    connect(remove_btn_, &QPushButton::clicked, this, &KeymapEditor::onRemoveBinding);
    connect(reset_btn_, &QPushButton::clicked, this, &KeymapEditor::onResetDefaults);
    connect(export_btn_, &QPushButton::clicked, this, &KeymapEditor::onExport);
    connect(import_btn_, &QPushButton::clicked, this, &KeymapEditor::onImport);

    btn_layout->addWidget(add_btn_);
    btn_layout->addWidget(remove_btn_);
    btn_layout->addStretch();
    btn_layout->addWidget(import_btn_);
    btn_layout->addWidget(export_btn_);
    btn_layout->addWidget(reset_btn_);
    layout->addLayout(btn_layout);

    // OK / Cancel / Apply
    auto* button_box = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply);
    connect(button_box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(button_box->button(QDialogButtonBox::Apply),
            &QPushButton::clicked, this, &KeymapEditor::onApply);
    connect(button_box, &QDialogButtonBox::accepted, this, &KeymapEditor::onApply);
    layout->addWidget(button_box);
}

void KeymapEditor::populateTable() {
    const auto& profile = profiles_[current_profile_idx_];
    table_->setRowCount(static_cast<int>(profile.bindings.size()));

    for (int i = 0; i < static_cast<int>(profile.bindings.size()); ++i) {
        const auto& b = profile.bindings[i];

        table_->setItem(i, 0, new QTableWidgetItem(b.action_name));
        table_->setItem(i, 1, new QTableWidgetItem(b.category));

        // Key sequence editor for the Qt key
        auto* key_edit = new QKeySequenceEdit(QKeySequence(b.qt_key));
        table_->setCellWidget(i, 2, key_edit);

        table_->setItem(i, 3, new QTableWidgetItem(QString::number(b.linux_keycode)));
    }
}

void KeymapEditor::loadProfile(const KeymapProfile& profile) {
    // Replace current profile
    if (current_profile_idx_ < static_cast<int>(profiles_.size())) {
        profiles_[current_profile_idx_] = profile;
    }
    populateTable();
}

KeymapProfile KeymapEditor::currentProfile() const {
    KeymapProfile profile;
    profile.name = profile_combo_->currentText();

    for (int i = 0; i < table_->rowCount(); ++i) {
        KeyBinding binding;
        binding.action_name = table_->item(i, 0)->text();
        binding.category = table_->item(i, 1)->text();

        auto* key_edit = qobject_cast<QKeySequenceEdit*>(table_->cellWidget(i, 2));
        if (key_edit && !key_edit->keySequence().isEmpty()) {
            binding.qt_key = key_edit->keySequence()[0].key();
        }

        binding.linux_keycode = static_cast<uint16_t>(
            table_->item(i, 3)->text().toUInt());

        profile.bindings.push_back(binding);
    }

    return profile;
}

void KeymapEditor::onProfileChanged(int index) {
    if (index >= 0 && index < static_cast<int>(profiles_.size())) {
        current_profile_idx_ = index;
        populateTable();
    }
}

void KeymapEditor::onAddBinding() {
    int row = table_->rowCount();
    table_->insertRow(row);
    table_->setItem(row, 0, new QTableWidgetItem(tr("New Action")));
    table_->setItem(row, 1, new QTableWidgetItem(tr("Custom")));
    table_->setCellWidget(row, 2, new QKeySequenceEdit());
    table_->setItem(row, 3, new QTableWidgetItem("0"));
}

void KeymapEditor::onRemoveBinding() {
    auto selection = table_->selectionModel()->selectedRows();
    for (int i = selection.size() - 1; i >= 0; --i) {
        table_->removeRow(selection[i].row());
    }
}

void KeymapEditor::onResetDefaults() {
    profiles_[current_profile_idx_] =
        (current_profile_idx_ == 0) ? defaultProfile() :
        (current_profile_idx_ == 1) ? fpsProfile() : mobaProfile();
    populateTable();
}

void KeymapEditor::onApply() {
    auto profile = currentProfile();
    if (current_profile_idx_ < static_cast<int>(profiles_.size())) {
        profiles_[current_profile_idx_] = profile;
    }
    emit profileApplied(profile);
}

void KeymapEditor::onExport() {
    QString path = QFileDialog::getSaveFileName(
        this, tr("Export Keymap"), "keymap.json",
        tr("JSON Files (*.json)"));
    if (path.isEmpty()) return;

    saveToFile(path, profiles_);
}

void KeymapEditor::onImport() {
    QString path = QFileDialog::getOpenFileName(
        this, tr("Import Keymap"), QString(),
        tr("JSON Files (*.json)"));
    if (path.isEmpty()) return;

    auto imported = loadFromFile(path);
    if (!imported.empty()) {
        profiles_ = std::move(imported);
        profile_combo_->clear();
        for (const auto& p : profiles_) {
            profile_combo_->addItem(p.name);
        }
        current_profile_idx_ = 0;
        populateTable();
    }
}

std::vector<KeymapProfile> KeymapEditor::loadFromFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};

    auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray()) return {};

    std::vector<KeymapProfile> profiles;
    for (const auto& profile_val : doc.array()) {
        auto obj = profile_val.toObject();
        KeymapProfile profile;
        profile.name = obj["name"].toString();

        for (const auto& binding_val : obj["bindings"].toArray()) {
            auto bobj = binding_val.toObject();
            KeyBinding binding;
            binding.action_name = bobj["action"].toString();
            binding.category = bobj["category"].toString();
            binding.qt_key = bobj["qt_key"].toInt();
            binding.linux_keycode = static_cast<uint16_t>(bobj["linux_code"].toInt());
            profile.bindings.push_back(binding);
        }
        profiles.push_back(profile);
    }
    return profiles;
}

bool KeymapEditor::saveToFile(const QString& path,
                               const std::vector<KeymapProfile>& profiles) {
    QJsonArray arr;
    for (const auto& profile : profiles) {
        QJsonObject obj;
        obj["name"] = profile.name;

        QJsonArray bindings;
        for (const auto& b : profile.bindings) {
            QJsonObject bobj;
            bobj["action"] = b.action_name;
            bobj["category"] = b.category;
            bobj["qt_key"] = b.qt_key;
            bobj["linux_code"] = b.linux_keycode;
            bindings.append(bobj);
        }
        obj["bindings"] = bindings;
        arr.append(obj);
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    return true;
}

// ============================================================================
// Default profiles
// ============================================================================

KeymapProfile KeymapEditor::defaultProfile() {
    KeymapProfile p;
    p.name = "Default";
    p.bindings = {
        {"Back",       Qt::Key_Escape,    linux_input::KEY_BACK,       "Android"},
        {"Home",       Qt::Key_Home,      linux_input::KEY_HOME,       "Android"},
        {"Menu",       Qt::Key_Menu,      linux_input::KEY_MENU,       "Android"},
        {"Volume Up",  Qt::Key_VolumeUp,  linux_input::KEY_VOLUMEUP,   "Android"},
        {"Volume Down",Qt::Key_VolumeDown,linux_input::KEY_VOLUMEDOWN, "Android"},
        {"Enter",      Qt::Key_Return,    linux_input::KEY_ENTER,      "Navigation"},
        {"Space",      Qt::Key_Space,     linux_input::KEY_SPACE,      "Navigation"},
        {"Tab",        Qt::Key_Tab,       linux_input::KEY_TAB,        "Navigation"},
        {"Up",         Qt::Key_Up,        linux_input::KEY_UP,         "Navigation"},
        {"Down",       Qt::Key_Down,      linux_input::KEY_DOWN,       "Navigation"},
        {"Left",       Qt::Key_Left,      linux_input::KEY_LEFT,       "Navigation"},
        {"Right",      Qt::Key_Right,     linux_input::KEY_RIGHT,      "Navigation"},
    };
    return p;
}

KeymapProfile KeymapEditor::fpsProfile() {
    KeymapProfile p;
    p.name = "FPS";
    p.bindings = {
        {"Move Forward",  Qt::Key_W, linux_input::KEY_DPAD_UP,    "Gaming"},
        {"Move Left",     Qt::Key_A, linux_input::KEY_DPAD_LEFT,  "Gaming"},
        {"Move Back",     Qt::Key_S, linux_input::KEY_DPAD_DOWN,  "Gaming"},
        {"Move Right",    Qt::Key_D, linux_input::KEY_DPAD_RIGHT, "Gaming"},
        {"Jump",          Qt::Key_Space,  linux_input::KEY_SPACE,  "Gaming"},
        {"Crouch",        Qt::Key_C,      linux_input::KEY_C,      "Gaming"},
        {"Reload",        Qt::Key_R,      linux_input::KEY_R,      "Gaming"},
        {"Sprint",        Qt::Key_Shift,  linux_input::KEY_LEFTSHIFT, "Gaming"},
        {"Interact",      Qt::Key_E,      linux_input::KEY_E,      "Gaming"},
        {"Inventory",     Qt::Key_Tab,    linux_input::KEY_TAB,    "Gaming"},
        {"Map",           Qt::Key_M,      linux_input::KEY_M,      "Gaming"},
        {"Back",          Qt::Key_Escape, linux_input::KEY_BACK,   "Android"},
    };
    return p;
}

KeymapProfile KeymapEditor::mobaProfile() {
    KeymapProfile p;
    p.name = "MOBA";
    p.bindings = {
        {"Skill 1",   Qt::Key_Q, linux_input::KEY_Q,          "Gaming"},
        {"Skill 2",   Qt::Key_W, linux_input::KEY_W,          "Gaming"},
        {"Skill 3",   Qt::Key_E, linux_input::KEY_E,          "Gaming"},
        {"Ultimate",  Qt::Key_R, linux_input::KEY_R,          "Gaming"},
        {"Summoner 1",Qt::Key_D, linux_input::KEY_D,          "Gaming"},
        {"Summoner 2",Qt::Key_F, linux_input::KEY_F,          "Gaming"},
        {"Item 1",    Qt::Key_1, linux_input::KEY_1,          "Gaming"},
        {"Item 2",    Qt::Key_2, linux_input::KEY_2,          "Gaming"},
        {"Item 3",    Qt::Key_3, linux_input::KEY_3,          "Gaming"},
        {"Recall",    Qt::Key_B, linux_input::KEY_B,          "Gaming"},
        {"Shop",      Qt::Key_P, linux_input::KEY_P,          "Gaming"},
        {"Tab/Score", Qt::Key_Tab, linux_input::KEY_TAB,      "Gaming"},
        {"Back",      Qt::Key_Escape, linux_input::KEY_BACK,  "Android"},
    };
    return p;
}

} // namespace rex::gui
