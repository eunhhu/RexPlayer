#include "keymap_editor.h"

#include <QPainter>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSlider>
#include <QSpinBox>
#include <QDrag>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QJsonDocument>
#include <QDir>
#include <QStandardPaths>
#include <cstdio>

namespace rex::gui {

// ===========================================================================
// KeymapWidgetData
// ===========================================================================

QJsonObject KeymapWidgetData::toJson() const {
    QJsonObject obj;
    obj["type"] = type;
    obj["x"] = x;
    obj["y"] = y;
    obj["size"] = size;
    obj["opacity"] = static_cast<double>(opacity);
    obj["bound_key"] = bound_key;
    obj["label"] = label;
    if (!extra.isEmpty()) obj["extra"] = extra;
    return obj;
}

KeymapWidgetData KeymapWidgetData::fromJson(const QJsonObject& obj) {
    KeymapWidgetData d;
    d.type = obj["type"].toString("key");
    d.x = obj["x"].toInt();
    d.y = obj["y"].toInt();
    d.size = obj["size"].toInt(50);
    d.opacity = static_cast<float>(obj["opacity"].toDouble(0.7));
    d.bound_key = obj["bound_key"].toString();
    d.label = obj["label"].toString();
    d.extra = obj["extra"].toObject();
    return d;
}

// ===========================================================================
// KeymapProfile
// ===========================================================================

QJsonObject KeymapProfile::toJson() const {
    QJsonObject obj;
    obj["name"] = name;
    QJsonArray arr;
    for (const auto& w : widgets)
        arr.append(w.toJson());
    obj["widgets"] = arr;
    return obj;
}

KeymapProfile KeymapProfile::fromJson(const QJsonObject& obj) {
    KeymapProfile p;
    p.name = obj["name"].toString();
    auto arr = obj["widgets"].toArray();
    for (const auto& v : arr)
        p.widgets.append(KeymapWidgetData::fromJson(v.toObject()));
    return p;
}

// ===========================================================================
// KeymapOverlayWidget
// ===========================================================================

KeymapOverlayWidget::KeymapOverlayWidget(const KeymapWidgetData& data, QWidget* parent)
    : QWidget(parent), data_(data) {
    setFixedSize(data.size, data.size);
    move(data.x, data.y);
    setFocusPolicy(Qt::ClickFocus);
    setCursor(Qt::OpenHandCursor);
}

void KeymapOverlayWidget::setData(const KeymapWidgetData& d) {
    data_ = d;
    setFixedSize(d.size, d.size);
    move(d.x, d.y);
    update();
}

void KeymapOverlayWidget::setSelected(bool s) {
    selected_ = s;
    update();
}

QColor KeymapOverlayWidget::typeColor() const {
    static const QMap<QString, QColor> colors = {
        {"key",      QColor(76, 175, 80)},
        {"dpad",     QColor(33, 150, 243)},
        {"joystick", QColor(156, 39, 176)},
        {"tapzone",  QColor(255, 152, 0)},
        {"swipe",    QColor(0, 188, 212)},
        {"aim",      QColor(244, 67, 54)},
        {"shoot",    QColor(255, 87, 34)},
    };
    return colors.value(data_.type, QColor(128, 128, 128));
}

QString KeymapOverlayWidget::typeLabel() const {
    if (!data_.label.isEmpty()) return data_.label;
    if (!data_.bound_key.isEmpty()) return data_.bound_key;
    return data_.type.left(3).toUpper();
}

void KeymapOverlayWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QColor bg = typeColor();
    bg.setAlphaF(data_.opacity);
    p.setBrush(bg);

    QPen pen(selected_ ? Qt::white : QColor(200, 200, 200), selected_ ? 2 : 1);
    p.setPen(pen);
    p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 6, 6);

    p.setPen(Qt::white);
    p.drawText(rect(), Qt::AlignCenter, typeLabel());
}

void KeymapOverlayWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        dragging_ = true;
        drag_offset_ = event->pos();
        setCursor(Qt::ClosedHandCursor);
        emit selected(this);
    }
}

void KeymapOverlayWidget::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_) {
        QPoint newPos = mapToParent(event->pos() - drag_offset_);
        move(newPos);
        data_.x = newPos.x();
        data_.y = newPos.y();
        emit moved(this, newPos);
    }
}

void KeymapOverlayWidget::mouseReleaseEvent(QMouseEvent*) {
    dragging_ = false;
    setCursor(Qt::OpenHandCursor);
}

void KeymapOverlayWidget::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        emit deleteRequested(this);
    }
}

// ===========================================================================
// KeymapOverlay
// ===========================================================================

KeymapOverlay::KeymapOverlay(QWidget* parent) : QWidget(parent) {
    setAcceptDrops(true);
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_TranslucentBackground);
}

void KeymapOverlay::addWidget(const KeymapWidgetData& data) {
    auto* w = new KeymapOverlayWidget(data, this);
    items_.append(w);
    w->show();

    connect(w, &KeymapOverlayWidget::selected, this, &KeymapOverlay::widgetSelected);
    connect(w, &KeymapOverlayWidget::moved, this, [this]() { emit widgetMoved(); });
}

void KeymapOverlay::removeWidget(KeymapOverlayWidget* w) {
    items_.removeAll(w);
    w->deleteLater();
}

void KeymapOverlay::clearWidgets() {
    for (auto* w : items_) w->deleteLater();
    items_.clear();
}

void KeymapOverlay::deselectAll() {
    for (auto* w : items_) w->setSelected(false);
}

QVector<KeymapWidgetData> KeymapOverlay::collectWidgetData() const {
    QVector<KeymapWidgetData> result;
    for (const auto* w : items_)
        result.append(w->data());
    return result;
}

void KeymapOverlay::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasText())
        event->acceptProposedAction();
}

void KeymapOverlay::dropEvent(QDropEvent* event) {
    QString type = event->mimeData()->text();
    KeymapWidgetData data;
    data.type = type;
    data.x = static_cast<int>(event->position().x());
    data.y = static_cast<int>(event->position().y());
    data.size = (type == "joystick" || type == "dpad") ? 120 : 50;
    addWidget(data);
    event->acceptProposedAction();
}

void KeymapOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0, 30));
    p.setPen(QColor(255, 255, 255, 80));
    p.drawText(rect(), Qt::AlignTop | Qt::AlignHCenter,
               "Drag widgets from toolbox below");
}

// ===========================================================================
// KeymapPropertyPopover
// ===========================================================================

KeymapPropertyPopover::KeymapPropertyPopover(QWidget* parent) : QWidget(parent) {
    setFixedSize(240, 200);
    setWindowFlags(Qt::Popup);
    buildUi();
    hide();
}

void KeymapPropertyPopover::buildUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);

    key_edit_ = new QLineEdit(this);
    key_edit_->setPlaceholderText("Bound key (e.g. W, Space)");
    layout->addWidget(new QLabel("Key Binding:", this));
    layout->addWidget(key_edit_);

    label_edit_ = new QLineEdit(this);
    label_edit_->setPlaceholderText("Label");
    layout->addWidget(new QLabel("Label:", this));
    layout->addWidget(label_edit_);

    size_spin_ = new QSpinBox(this);
    size_spin_->setRange(20, 200);
    layout->addWidget(new QLabel("Size:", this));
    layout->addWidget(size_spin_);

    opacity_slider_ = new QSlider(Qt::Horizontal, this);
    opacity_slider_->setRange(10, 100);
    layout->addWidget(new QLabel("Opacity:", this));
    layout->addWidget(opacity_slider_);

    auto* btn_row = new QHBoxLayout();
    delete_btn_ = new QPushButton("Delete", this);
    close_btn_ = new QPushButton("Done", this);
    btn_row->addWidget(delete_btn_);
    btn_row->addWidget(close_btn_);
    layout->addLayout(btn_row);

    connect(close_btn_, &QPushButton::clicked, this, [this]() {
        commit();
        hide();
    });
    connect(delete_btn_, &QPushButton::clicked, this, [this]() {
        if (target_) emit deleteRequested(target_);
        hide();
    });
}

void KeymapPropertyPopover::showForWidget(KeymapOverlayWidget* w) {
    target_ = w;
    populate(w);
    QPoint pos = w->mapToGlobal(QPoint(w->width() + 5, 0));
    move(pos);
    show();
    raise();
}

void KeymapPropertyPopover::hide_() {
    hide();
}

void KeymapPropertyPopover::populate(KeymapOverlayWidget* w) {
    auto d = w->data();
    key_edit_->setText(d.bound_key);
    label_edit_->setText(d.label);
    size_spin_->setValue(d.size);
    opacity_slider_->setValue(static_cast<int>(d.opacity * 100));
}

void KeymapPropertyPopover::commit() {
    if (!target_) return;
    auto d = target_->data();
    d.bound_key = key_edit_->text();
    d.label = label_edit_->text();
    d.size = size_spin_->value();
    d.opacity = opacity_slider_->value() / 100.0f;
    emit dataChanged(target_, d);
}

// ===========================================================================
// KeymapEditor
// ===========================================================================

KeymapEditor::KeymapEditor(QWidget* parent) : QWidget(parent) {
    buildUi();
    installDefaultProfiles();
}

void KeymapEditor::buildUi() {
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(0, 0, 0, 0);

    // Overlay (will be reparented when display widget is set)
    overlay_ = new KeymapOverlay(this);
    overlay_->hide();

    // Popover
    popover_ = new KeymapPropertyPopover(this);

    connect(overlay_, &KeymapOverlay::widgetSelected,
            this, &KeymapEditor::onWidgetSelected);
    connect(popover_, &KeymapPropertyPopover::dataChanged,
            this, &KeymapEditor::onPropertyChanged);
    connect(popover_, &KeymapPropertyPopover::deleteRequested,
            this, &KeymapEditor::onDeleteRequested);

    // Toolbox + profile bar
    toolbox_panel_ = new QWidget(this);
    auto* toolbox_layout = new QHBoxLayout(toolbox_panel_);
    toolbox_layout->setContentsMargins(4, 4, 4, 4);

    profile_combo_ = new QComboBox(toolbox_panel_);
    connect(profile_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &KeymapEditor::onProfileComboChanged);
    toolbox_layout->addWidget(new QLabel("Profile:", toolbox_panel_));
    toolbox_layout->addWidget(profile_combo_);

    auto* save_btn = new QPushButton("Save", toolbox_panel_);
    connect(save_btn, &QPushButton::clicked, this, &KeymapEditor::onSaveProfile);
    toolbox_layout->addWidget(save_btn);

    toolbox_layout->addSpacing(16);

    createToolbox();

    toolbox_layout->addStretch();

    auto* import_btn = new QPushButton("Import", toolbox_panel_);
    connect(import_btn, &QPushButton::clicked, this, &KeymapEditor::onLoadFromFile);
    toolbox_layout->addWidget(import_btn);

    auto* export_btn = new QPushButton("Export", toolbox_panel_);
    connect(export_btn, &QPushButton::clicked, this, &KeymapEditor::onSaveToFile);
    toolbox_layout->addWidget(export_btn);

    main_layout->addWidget(toolbox_panel_);
    toolbox_panel_->hide();
}

void KeymapEditor::createToolbox() {
    auto* layout = qobject_cast<QHBoxLayout*>(toolbox_panel_->layout());
    if (!layout) return;

    QStringList types = {"key", "dpad", "joystick", "tapzone", "swipe", "aim", "shoot"};
    for (const auto& type : types) {
        auto* btn = new QPushButton(type.left(3).toUpper(), toolbox_panel_);
        btn->setFixedSize(48, 32);
        btn->setToolTip("Drag to add " + type + " widget");
        connect(btn, &QPushButton::pressed, [btn, type]() {
            auto* drag = new QDrag(btn);
            auto* mime = new QMimeData();
            mime->setText(type);
            drag->setMimeData(mime);
            drag->exec(Qt::CopyAction);
        });
        layout->addWidget(btn);
    }
}

void KeymapEditor::setEditMode(bool enabled) {
    edit_mode_ = enabled;
    overlay_->setVisible(enabled);
    toolbox_panel_->setVisible(enabled);
    if (!enabled) popover_->hide();
    emit editModeChanged(enabled);
}

void KeymapEditor::setDisplayWidget(QWidget* display) {
    display_ref_ = display;
    if (display) {
        overlay_->setParent(display);
        overlay_->setGeometry(display->rect());
    }
}

void KeymapEditor::loadProfile(const QString& name) {
    if (profiles_.contains(name)) {
        current_profile_name_ = name;
        applyProfile(profiles_[name]);
    }
}

void KeymapEditor::saveProfile(const QString& name) {
    auto profile = currentProfile(name);
    profiles_[name] = profile;

    QDir dir(profilesDir());
    if (!dir.exists()) dir.mkpath(".");
    QFile file(profilePath(name));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(profile.toJson()).toJson());
    }
}

QStringList KeymapEditor::profileNames() const {
    return profiles_.keys();
}

void KeymapEditor::installDefaultProfiles() {
    // Default profile
    KeymapProfile def;
    def.name = "Default";
    profiles_["Default"] = def;

    // FPS profile
    KeymapProfile fps;
    fps.name = "FPS";
    fps.widgets.append({"dpad", 80, 350, 120, 0.7f, "W", "Move", {{"keys_up", "W"}, {"keys_down", "S"}, {"keys_left", "A"}, {"keys_right", "D"}}});
    fps.widgets.append({"key", 250, 450, 50, 0.7f, "Space", "Jump", {}});
    fps.widgets.append({"key", 250, 380, 50, 0.7f, "R", "Reload", {}});
    fps.widgets.append({"aim", 400, 300, 100, 0.5f, "", "Aim", {}});
    fps.widgets.append({"shoot", 450, 400, 50, 0.7f, "", "Shoot", {}});
    profiles_["FPS"] = fps;

    // MOBA profile
    KeymapProfile moba;
    moba.name = "MOBA";
    moba.widgets.append({"joystick", 80, 400, 140, 0.7f, "W", "Move", {}});
    moba.widgets.append({"key", 350, 450, 50, 0.7f, "Q", "Skill 1", {}});
    moba.widgets.append({"key", 410, 420, 50, 0.7f, "W", "Skill 2", {}});
    moba.widgets.append({"key", 460, 380, 50, 0.7f, "E", "Skill 3", {}});
    moba.widgets.append({"key", 500, 330, 50, 0.7f, "R", "Ultimate", {}});
    profiles_["MOBA"] = moba;

    // Populate combo
    profile_combo_->blockSignals(true);
    profile_combo_->clear();
    for (const auto& name : profiles_.keys())
        profile_combo_->addItem(name);
    profile_combo_->blockSignals(false);

    current_profile_name_ = "Default";
}

void KeymapEditor::applyProfile(const KeymapProfile& profile) {
    overlay_->clearWidgets();
    for (const auto& w : profile.widgets)
        overlay_->addWidget(w);
}

KeymapProfile KeymapEditor::currentProfile(const QString& name) const {
    KeymapProfile p;
    p.name = name;
    p.widgets = overlay_->collectWidgetData();
    return p;
}

QString KeymapEditor::profilesDir() const {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/keymaps";
}

QString KeymapEditor::profilePath(const QString& name) const {
    return profilesDir() + "/" + name + ".json";
}

void KeymapEditor::onWidgetSelected(KeymapOverlayWidget* w) {
    overlay_->deselectAll();
    w->setSelected(true);
    popover_->showForWidget(w);
}

void KeymapEditor::onPropertyChanged(KeymapOverlayWidget* w, const KeymapWidgetData& data) {
    w->setData(data);
}

void KeymapEditor::onDeleteRequested(KeymapOverlayWidget* w) {
    overlay_->removeWidget(w);
    popover_->hide();
}

void KeymapEditor::onProfileComboChanged(int index) {
    QString name = profile_combo_->itemText(index);
    if (!name.isEmpty()) loadProfile(name);
}

void KeymapEditor::onSaveProfile() {
    if (!current_profile_name_.isEmpty())
        saveProfile(current_profile_name_);
}

void KeymapEditor::onLoadFromFile() {
    QString path = QFileDialog::getOpenFileName(this, "Import Keymap", "", "JSON (*.json)");
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;
    auto doc = QJsonDocument::fromJson(file.readAll());
    auto profile = KeymapProfile::fromJson(doc.object());
    if (!profile.name.isEmpty()) {
        profiles_[profile.name] = profile;
        profile_combo_->addItem(profile.name);
        loadProfile(profile.name);
    }
}

void KeymapEditor::onSaveToFile() {
    QString path = QFileDialog::getSaveFileName(this, "Export Keymap", "", "JSON (*.json)");
    if (path.isEmpty()) return;
    auto profile = currentProfile(current_profile_name_);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(profile.toJson()).toJson());
    }
}

} // namespace rex::gui
