#include "frida_panel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFont>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDateTime>
#include <QInputDialog>
#include <QMessageBox>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QColor>
#include <QStandardPaths>
#include <QDialog>
#include <QListWidget>
#include <QDialogButtonBox>

namespace rex::gui {

// ---------------------------------------------------------------------------
// JsSyntaxHighlighter
// ---------------------------------------------------------------------------

JsSyntaxHighlighter::JsSyntaxHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent)
{
    // Keywords — blue
    QTextCharFormat keyword_fmt;
    keyword_fmt.setForeground(QColor(86, 156, 214));
    keyword_fmt.setFontWeight(QFont::Bold);
    const QStringList keywords = {
        "function", "var", "let", "const", "if", "else", "return",
        "for", "while", "do", "break", "continue", "switch", "case",
        "default", "new", "delete", "typeof", "instanceof", "in", "of",
        "try", "catch", "finally", "throw", "class", "extends", "import",
        "export", "this", "super", "null", "undefined", "true", "false",
        "void", "async", "await", "yield"
    };
    for (const auto& kw : keywords) {
        Rule r;
        r.pattern = QRegularExpression(QString("\\b%1\\b").arg(kw));
        r.format  = keyword_fmt;
        rules_.append(r);
    }

    // Frida API — purple
    QTextCharFormat frida_fmt;
    frida_fmt.setForeground(QColor(197, 134, 192));
    frida_fmt.setFontWeight(QFont::Bold);
    const QStringList frida_api = {
        "Java\\.perform", "Java\\.use", "Java\\.choose",
        "Interceptor\\.attach", "Interceptor\\.replace",
        "Module\\.findBaseAddress", "Module\\.enumerateExports",
        "Memory\\.readByteArray", "Memory\\.writeByteArray",
        "Process\\.enumerateModules", "Process\\.getCurrentThreadId",
        "hexdump", "send", "recv", "console\\.log",
        "console\\.warn", "console\\.error"
    };
    for (const auto& api : frida_api) {
        Rule r;
        r.pattern = QRegularExpression(api);
        r.format  = frida_fmt;
        rules_.append(r);
    }

    // Strings — green  (single and double quoted, non-greedy)
    QTextCharFormat string_fmt;
    string_fmt.setForeground(QColor(206, 145, 120));
    {
        Rule r;
        r.pattern = QRegularExpression(R"("(?:[^"\\]|\\.)*")");
        r.format  = string_fmt;
        rules_.append(r);
    }
    {
        Rule r;
        r.pattern = QRegularExpression(R"('(?:[^'\\]|\\.)*')");
        r.format  = string_fmt;
        rules_.append(r);
    }
    {
        Rule r;
        r.pattern = QRegularExpression(R"(`(?:[^`\\]|\\.)*`)");
        r.format  = string_fmt;
        rules_.append(r);
    }

    // Numbers — light green
    QTextCharFormat number_fmt;
    number_fmt.setForeground(QColor(181, 206, 168));
    {
        Rule r;
        r.pattern = QRegularExpression(R"(\b0x[0-9A-Fa-f]+|\b\d+\.?\d*([eE][+-]?\d+)?\b)");
        r.format  = number_fmt;
        rules_.append(r);
    }

    // Single-line comments — gray/green
    comment_format_.setForeground(QColor(106, 153, 85));
    comment_format_.setFontItalic(true);
    {
        Rule r;
        r.pattern = QRegularExpression(R"(//[^\n]*)");
        r.format  = comment_format_;
        rules_.append(r);
    }

    // Multi-line comment delimiters
    comment_start_ = QRegularExpression(R"(/\*)");
    comment_end_   = QRegularExpression(R"(\*/)");
}

void JsSyntaxHighlighter::highlightBlock(const QString& text)
{
    // Apply single-line rules
    for (const auto& rule : rules_) {
        auto it = rule.pattern.globalMatch(text);
        while (it.hasNext()) {
            auto m = it.next();
            setFormat(static_cast<int>(m.capturedStart()),
                      static_cast<int>(m.capturedLength()),
                      rule.format);
        }
    }

    // Multi-line block comments
    setCurrentBlockState(0);
    int start = 0;
    if (previousBlockState() != 1)
        start = static_cast<int>(comment_start_.match(text).capturedStart());

    while (start >= 0) {
        auto end_match = comment_end_.match(text, start);
        int end_idx    = static_cast<int>(end_match.capturedStart());
        int len;
        if (end_idx == -1) {
            setCurrentBlockState(1);
            len = text.length() - start;
        } else {
            len = end_idx - start + static_cast<int>(end_match.capturedLength());
        }
        setFormat(start, len, comment_format_);
        start = static_cast<int>(
            comment_start_.match(text, start + len).capturedStart());
    }
}

// ---------------------------------------------------------------------------
// Script templates
// ---------------------------------------------------------------------------

static const char* k_template_hook =
"Java.perform(function() {\n"
"    var cls = Java.use('CLASS_NAME');\n"
"    cls.METHOD_NAME.implementation = function() {\n"
"        console.log('METHOD_NAME called');\n"
"        return this.METHOD_NAME.apply(this, arguments);\n"
"    };\n"
"});\n";

static const char* k_template_trace =
"Java.perform(function() {\n"
"    var cls = Java.use('CLASS_NAME');\n"
"    var methods = cls.class.getDeclaredMethods();\n"
"    methods.forEach(function(method) {\n"
"        console.log('Method: ' + method.getName());\n"
"    });\n"
"});\n";

static const char* k_template_dump =
"var base = Module.findBaseAddress('MODULE_NAME');\n"
"if (base) {\n"
"    console.log(hexdump(base, { length: 256 }));\n"
"}\n";

static const char* k_template_custom =
"// Custom Frida script\n"
"Java.perform(function() {\n"
"    // Your code here\n"
"});\n";

// ---------------------------------------------------------------------------
// FridaPanel
// ---------------------------------------------------------------------------

FridaPanel::FridaPanel(QWidget* parent) : QWidget(parent) {
    setupUi();
    loadScriptList();
}

void FridaPanel::setAdbPort(int port) {
    adb_port_ = port;
}

QString FridaPanel::scriptsDir() const {
    QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    return home + "/.rexplayer/scripts";
}

void FridaPanel::setupUi() {
    setMinimumHeight(300);

    // --- Toolbar row ---
    auto* toolbar = new QWidget(this);
    auto* tb_layout = new QHBoxLayout(toolbar);
    tb_layout->setContentsMargins(4, 4, 4, 4);
    tb_layout->setSpacing(6);

    auto* frida_label = new QLabel("Frida", toolbar);
    frida_label->setStyleSheet("font-weight: bold; color: #d4a017;");
    tb_layout->addWidget(frida_label);

    tb_layout->addStretch();

    auto* proc_label = new QLabel("Process:", toolbar);
    tb_layout->addWidget(proc_label);

    process_combo_ = new QComboBox(toolbar);
    process_combo_->setMinimumWidth(160);
    process_combo_->setEditable(true);
    process_combo_->addItem("com.android.settings");
    process_combo_->setToolTip("Target process name or PID");
    tb_layout->addWidget(process_combo_);

    start_btn_ = new QPushButton("Start", toolbar);
    start_btn_->setToolTip("Inject Frida gadget and run script");
    start_btn_->setStyleSheet(
        "QPushButton { background: #2d7a2d; color: white; border-radius: 4px; padding: 4px 10px; }"
        "QPushButton:hover { background: #3a9a3a; }");
    tb_layout->addWidget(start_btn_);

    stop_btn_ = new QPushButton("Stop", toolbar);
    stop_btn_->setToolTip("Detach Frida session");
    stop_btn_->setEnabled(false);
    stop_btn_->setStyleSheet(
        "QPushButton { background: #7a2d2d; color: white; border-radius: 4px; padding: 4px 10px; }"
        "QPushButton:hover { background: #9a3a3a; }"
        "QPushButton:disabled { background: #444; color: #888; }");
    tb_layout->addWidget(stop_btn_);

    connect(start_btn_, &QPushButton::clicked, this, &FridaPanel::onStart);
    connect(stop_btn_,  &QPushButton::clicked, this, &FridaPanel::onStop);

    // --- Left panel: script list ---
    auto* left_widget = new QWidget(this);
    auto* left_layout = new QVBoxLayout(left_widget);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(2);

    auto* scripts_label = new QLabel("Scripts", left_widget);
    scripts_label->setStyleSheet("color: #888; font-size: 11px; padding: 2px 4px;");
    left_layout->addWidget(scripts_label);

    script_list_ = new QListWidget(left_widget);
    script_list_->setStyleSheet(
        "QListWidget { background: #1e1e1e; border: none; color: #d4d4d4; }"
        "QListWidget::item { padding: 4px 6px; }"
        "QListWidget::item:selected { background: #264f78; }"
        "QListWidget::item:hover { background: #2a2d2e; }");
    left_layout->addWidget(script_list_, 1);

    new_script_btn_ = new QPushButton("+ New", left_widget);
    new_script_btn_->setStyleSheet(
        "QPushButton { background: #3c3c3c; color: #d4d4d4; border: none; padding: 5px; border-radius: 3px; }"
        "QPushButton:hover { background: #4a4a4a; }");
    left_layout->addWidget(new_script_btn_);

    connect(script_list_, &QListWidget::itemClicked, this, &FridaPanel::onScriptSelected);
    connect(new_script_btn_, &QPushButton::clicked, this, &FridaPanel::onNewScript);

    // --- Center panel: JS editor ---
    editor_ = new QPlainTextEdit(this);
    editor_->setStyleSheet(
        "QPlainTextEdit { background: #1e1e1e; color: #d4d4d4; border: none; }");
    QFont mono("Monospace", 12);
    mono.setStyleHint(QFont::Monospace);
    editor_->setFont(mono);
    editor_->setTabStopDistance(28.0);
    editor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    editor_->setPlaceholderText("// Select or create a script on the left");
    new JsSyntaxHighlighter(editor_->document());

    // Horizontal splitter: script list | editor
    auto* h_splitter = new QSplitter(Qt::Horizontal, this);
    h_splitter->addWidget(left_widget);
    h_splitter->addWidget(editor_);
    h_splitter->setStretchFactor(0, 0);
    h_splitter->setStretchFactor(1, 1);
    h_splitter->setSizes({160, 600});
    h_splitter->setStyleSheet("QSplitter::handle { background: #3c3c3c; width: 1px; }");

    // --- Bottom panel: console ---
    auto* console_header = new QWidget(this);
    auto* ch_layout = new QHBoxLayout(console_header);
    ch_layout->setContentsMargins(4, 2, 4, 2);

    auto* console_label = new QLabel("Console Output", console_header);
    console_label->setStyleSheet("color: #888; font-size: 11px;");
    ch_layout->addWidget(console_label);
    ch_layout->addStretch();

    auto* clear_btn = new QPushButton("Clear", console_header);
    clear_btn->setFixedWidth(50);
    clear_btn->setStyleSheet(
        "QPushButton { background: #3c3c3c; color: #d4d4d4; border: none; padding: 2px 6px;"
        "  border-radius: 3px; font-size: 11px; }"
        "QPushButton:hover { background: #4a4a4a; }");
    ch_layout->addWidget(clear_btn);

    console_ = new QPlainTextEdit(this);
    console_->setReadOnly(true);
    console_->setMaximumBlockCount(2000);
    console_->setStyleSheet(
        "QPlainTextEdit { background: #141414; color: #b0b0b0; border: none; }");
    console_->setFont(mono);
    console_->setPlaceholderText("Console output will appear here...");

    connect(clear_btn, &QPushButton::clicked, console_, &QPlainTextEdit::clear);

    auto* console_widget = new QWidget(this);
    auto* cw_layout = new QVBoxLayout(console_widget);
    cw_layout->setContentsMargins(0, 0, 0, 0);
    cw_layout->setSpacing(0);
    cw_layout->addWidget(console_header);
    cw_layout->addWidget(console_);

    // Vertical splitter: (script list | editor) | console
    auto* v_splitter = new QSplitter(Qt::Vertical, this);
    v_splitter->addWidget(h_splitter);
    v_splitter->addWidget(console_widget);
    v_splitter->setStretchFactor(0, 1);
    v_splitter->setStretchFactor(1, 0);
    v_splitter->setSizes({400, 150});
    v_splitter->setStyleSheet("QSplitter::handle { background: #3c3c3c; height: 1px; }");

    // --- Root layout ---
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(toolbar);
    root->addWidget(v_splitter, 1);

    appendConsole("[Frida panel ready] Select a script and press Start.");
}

void FridaPanel::loadScriptList() {
    script_list_->clear();

    QDir dir(scriptsDir());
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    const QStringList files = dir.entryList({"*.js"}, QDir::Files, QDir::Name);
    for (const auto& f : files) {
        auto* item = new QListWidgetItem(f, script_list_);
        item->setData(Qt::UserRole, dir.absoluteFilePath(f));
    }

    if (files.isEmpty()) {
        appendConsole(QString("[info] No scripts found in %1").arg(dir.absolutePath()),
                      QColor(120, 120, 120));
    }
}

void FridaPanel::appendConsole(const QString& text, const QColor& color) {
    QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
    QTextCursor cursor = console_->textCursor();
    cursor.movePosition(QTextCursor::End);

    QTextCharFormat fmt;
    fmt.setForeground(color);

    cursor.insertText(QString("[%1] %2\n").arg(ts, text), fmt);
    console_->setTextCursor(cursor);
    console_->ensureCursorVisible();
}

void FridaPanel::onStart() {
    if (current_script_path_.isEmpty()) {
        appendConsole("[error] No script selected.", QColor(244, 71, 71));
        return;
    }

    // Save current editor content before running
    QFile f(current_script_path_);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&f);
        out << editor_->toPlainText();
    }

    QString process = process_combo_->currentText().trimmed();
    if (process.isEmpty()) {
        appendConsole("[error] No target process specified.", QColor(244, 71, 71));
        return;
    }

    appendConsole(QString("[info] Attaching to '%1' via ADB port %2...")
                      .arg(process).arg(adb_port_));
    appendConsole(QString("[info] Script: %1")
                      .arg(QFileInfo(current_script_path_).fileName()));
    appendConsole("[warn] Frida execution backend not yet connected — UI only.",
                  QColor(220, 180, 0));

    start_btn_->setEnabled(false);
    stop_btn_->setEnabled(true);
}

void FridaPanel::onStop() {
    appendConsole("[info] Detaching Frida session.");
    start_btn_->setEnabled(true);
    stop_btn_->setEnabled(false);
}

void FridaPanel::onNewScript() {
    // Template chooser dialog
    QDialog dlg(this);
    dlg.setWindowTitle("New Script");
    dlg.resize(300, 200);

    auto* layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel("Choose a template:"));

    auto* list = new QListWidget(&dlg);
    list->addItem("Hook Method");
    list->addItem("Trace Class");
    list->addItem("Dump Memory");
    list->addItem("Custom");
    list->setCurrentRow(0);
    layout->addWidget(list);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    int row = list->currentRow();
    const char* tmpl = nullptr;
    QString default_name;
    switch (row) {
        case 0: tmpl = k_template_hook;    default_name = "hook.js";   break;
        case 1: tmpl = k_template_trace;   default_name = "trace.js";  break;
        case 2: tmpl = k_template_dump;    default_name = "dump.js";   break;
        default: tmpl = k_template_custom; default_name = "script.js"; break;
    }

    bool ok;
    QString name = QInputDialog::getText(this, "Script Name", "File name:",
                                         QLineEdit::Normal, default_name, &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    if (!name.endsWith(".js")) name += ".js";

    QDir dir(scriptsDir());
    dir.mkpath(".");
    QString path = dir.absoluteFilePath(name);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Error", "Could not create file: " + path);
        return;
    }
    QTextStream out(&f);
    out << tmpl;
    f.close();

    loadScriptList();

    // Select and open the newly created script
    for (int i = 0; i < script_list_->count(); ++i) {
        if (script_list_->item(i)->data(Qt::UserRole).toString() == path) {
            script_list_->setCurrentRow(i);
            onScriptSelected(script_list_->item(i));
            break;
        }
    }

    appendConsole(QString("[info] Created: %1").arg(name));
}

void FridaPanel::onScriptSelected(QListWidgetItem* item) {
    if (!item) return;

    // Auto-save previous script
    if (!current_script_path_.isEmpty()) {
        QFile f(current_script_path_);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&f);
            out << editor_->toPlainText();
        }
    }

    current_script_path_ = item->data(Qt::UserRole).toString();

    QFile f(current_script_path_);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&f);
        editor_->setPlainText(in.readAll());
    }
}

} // namespace rex::gui
