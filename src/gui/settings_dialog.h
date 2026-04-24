#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QStackedWidget>

namespace rex::gui {

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

signals:
    // Emitted when the user clicks the Apply button. Dialog stays open.
    // Callers can connect this to whatever persistence layer they use.
    void applyRequested();

private:
    void createPages();
    QWidget* createGeneralPage();
    QWidget* createDisplayPage();
    QWidget* createPerformancePage();
    QWidget* createNetworkPage();
    QWidget* createInputPage();
    QWidget* createFridaPage();
    QWidget* createAdvancedPage();
    void filterSettings(const QString& text);

    QLineEdit*      search_bar_ = nullptr;
    QListWidget*    sidebar_    = nullptr;
    QStackedWidget* pages_      = nullptr;
};

} // namespace rex::gui
