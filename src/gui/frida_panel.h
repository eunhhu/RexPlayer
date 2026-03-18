#pragma once

#include <QWidget>
#include <QPlainTextEdit>
#include <QListWidget>
#include <QComboBox>
#include <QPushButton>
#include <QSplitter>
#include <QSyntaxHighlighter>
#include <QRegularExpression>
#include <QTextCharFormat>
#include <QVector>

namespace rex::gui {

class JsSyntaxHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit JsSyntaxHighlighter(QTextDocument* parent = nullptr);
protected:
    void highlightBlock(const QString& text) override;
private:
    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    QVector<Rule> rules_;
    QTextCharFormat comment_format_;
    QRegularExpression comment_start_;
    QRegularExpression comment_end_;
};

class FridaPanel : public QWidget {
    Q_OBJECT
public:
    explicit FridaPanel(QWidget* parent = nullptr);
    void setAdbPort(int port);

private slots:
    void onStart();
    void onStop();
    void onNewScript();
    void onScriptSelected(QListWidgetItem* item);

private:
    void setupUi();
    void loadScriptList();
    void appendConsole(const QString& text, const QColor& color = QColor(176, 176, 176));
    QString scriptsDir() const;

    QListWidget*   script_list_     = nullptr;
    QPushButton*   new_script_btn_  = nullptr;
    QPlainTextEdit* editor_         = nullptr;
    QPlainTextEdit* console_        = nullptr;
    QComboBox*     process_combo_   = nullptr;
    QPushButton*   start_btn_       = nullptr;
    QPushButton*   stop_btn_        = nullptr;
    QString        current_script_path_;
    int            adb_port_        = 5555;
};

} // namespace rex::gui
