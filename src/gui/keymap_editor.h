#pragma once

#include <QWidget>
#include <QComboBox>
#include <QPushButton>
#include <QLineEdit>
#include <QSlider>
#include <QSpinBox>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QVector>
#include <QMap>
#include <QPoint>
#include <QSize>

namespace rex::gui {

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

struct KeymapWidgetData {
    QString type;        // "key", "dpad", "joystick", "tapzone", "swipe", "aim", "shoot"
    int     x    = 0;
    int     y    = 0;
    int     size = 50;
    float   opacity   = 0.7f;
    QString bound_key;   // primary key binding (Qt key name, e.g. "W", "Space")
    QString label;
    QJsonObject extra;   // type-specific properties (e.g. keys for dpad)

    QJsonObject toJson() const;
    static KeymapWidgetData fromJson(const QJsonObject& obj);
};

struct KeymapProfile {
    QString name;
    QVector<KeymapWidgetData> widgets;

    QJsonObject toJson() const;
    static KeymapProfile fromJson(const QJsonObject& obj);
};

// ---------------------------------------------------------------------------
// Individual overlay widget (draggable / selectable)
// ---------------------------------------------------------------------------

class KeymapOverlayWidget : public QWidget {
    Q_OBJECT
public:
    explicit KeymapOverlayWidget(const KeymapWidgetData& data, QWidget* parent = nullptr);

    KeymapWidgetData data() const { return data_; }
    void setData(const KeymapWidgetData& d);
    void setSelected(bool s);
    bool isSelected() const { return selected_; }

signals:
    void moved(KeymapOverlayWidget* self, QPoint newPos);
    void selected(KeymapOverlayWidget* self);
    void deleteRequested(KeymapOverlayWidget* self);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    QColor typeColor() const;
    QString typeLabel() const;

    KeymapWidgetData data_;
    bool  selected_    = false;
    bool  dragging_    = false;
    QPoint drag_offset_;
};

// ---------------------------------------------------------------------------
// Overlay panel (transparent, sits on top of display)
// ---------------------------------------------------------------------------

class KeymapOverlay : public QWidget {
    Q_OBJECT
public:
    explicit KeymapOverlay(QWidget* parent = nullptr);

    void addWidget(const KeymapWidgetData& data);
    void removeWidget(KeymapOverlayWidget* w);
    void clearWidgets();
    void deselectAll();

    QVector<KeymapWidgetData> collectWidgetData() const;

signals:
    void widgetSelected(KeymapOverlayWidget* w);
    void widgetMoved();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QVector<KeymapOverlayWidget*> items_;
};

// ---------------------------------------------------------------------------
// Property popover shown when a widget is selected
// ---------------------------------------------------------------------------

class KeymapPropertyPopover : public QWidget {
    Q_OBJECT
public:
    explicit KeymapPropertyPopover(QWidget* parent = nullptr);
    void showForWidget(KeymapOverlayWidget* w);
    void hide_();

signals:
    void dataChanged(KeymapOverlayWidget* w, const KeymapWidgetData& newData);
    void deleteRequested(KeymapOverlayWidget* w);

private:
    void buildUi();
    void populate(KeymapOverlayWidget* w);
    void commit();

    KeymapOverlayWidget* target_ = nullptr;

    QLineEdit*  key_edit_     = nullptr;
    QLineEdit*  label_edit_   = nullptr;
    QSlider*    opacity_slider_ = nullptr;
    QSpinBox*   size_spin_    = nullptr;
    QPushButton* delete_btn_  = nullptr;
    QPushButton* close_btn_   = nullptr;
};

// ---------------------------------------------------------------------------
// Main KeymapEditor widget
// ---------------------------------------------------------------------------

class KeymapEditor : public QWidget {
    Q_OBJECT
public:
    explicit KeymapEditor(QWidget* parent = nullptr);

    void setEditMode(bool enabled);
    bool isEditMode() const { return edit_mode_; }

    void loadProfile(const QString& name);
    void saveProfile(const QString& name);
    QStringList profileNames() const;

    // Set the display widget that the overlay should cover
    void setDisplayWidget(QWidget* display);

signals:
    void editModeChanged(bool enabled);

private slots:
    void onWidgetSelected(KeymapOverlayWidget* w);
    void onPropertyChanged(KeymapOverlayWidget* w, const KeymapWidgetData& data);
    void onDeleteRequested(KeymapOverlayWidget* w);
    void onProfileComboChanged(int index);
    void onSaveProfile();
    void onLoadFromFile();
    void onSaveToFile();

private:
    void buildUi();
    void createToolbox();
    void installDefaultProfiles();
    void applyProfile(const KeymapProfile& profile);
    KeymapProfile currentProfile(const QString& name) const;

    QString profilesDir() const;
    QString profilePath(const QString& name) const;

    bool edit_mode_ = false;

    QWidget*            display_ref_  = nullptr;
    KeymapOverlay*      overlay_      = nullptr;
    KeymapPropertyPopover* popover_   = nullptr;

    QWidget*    toolbox_panel_  = nullptr;
    QComboBox*  profile_combo_  = nullptr;

    QMap<QString, KeymapProfile> profiles_;
    QString current_profile_name_;
};

} // namespace rex::gui
