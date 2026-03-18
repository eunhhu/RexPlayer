#pragma once

#include <QWidget>

namespace rex::gui {

class KeymapEditor : public QWidget {
    Q_OBJECT
public:
    explicit KeymapEditor(QWidget* parent = nullptr)
        : QWidget(parent) {}
};

} // namespace rex::gui
