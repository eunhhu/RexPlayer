#pragma once

#include <QDialog>

namespace rex::gui {

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr)
        : QDialog(parent) {
        setWindowTitle("Settings");
        resize(600, 400);
    }
};

} // namespace rex::gui
