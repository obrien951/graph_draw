#include "mainwindow.h"
#include "graphserializer.h"
#include <QToolBar>
#include <QAction>
#include <QActionGroup>
#include <QMenuBar>
#include <QMenu>
#include <QStatusBar>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QApplication>
#include <QPixmap>
#include <QImage>
#include <QPainter>
#include <QIcon>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Graph Draw — Dependency Visualiser"));
    resize(1280, 820);

    m_scene = new GraphScene(this);
    m_view  = new GraphView(this);
    m_view->setScene(m_scene);
    setCentralWidget(m_view);

    buildMenus();
    buildToolbar();

    // Status bar: mode indicator + legend
    m_modeLabel = new QLabel(QStringLiteral("Mode: Select"));
    statusBar()->addWidget(m_modeLabel);

    auto* legend = new QLabel(
        QStringLiteral("  ") +
        QStringLiteral("<font color='#4A90C4'>■ Class</font>  ") +
        QStringLiteral("<font color='#50AA64'>■ Module</font>  ") +
        QStringLiteral("<font color='#D48448'>● Function</font>  ") +
        QStringLiteral("  |  Scroll: zoom  |  Middle-mouse / Space+drag: pan  "
                       "|  Double-click: rename  |  Del: remove selected"));
    legend->setTextFormat(Qt::RichText);
    statusBar()->addPermanentWidget(legend);

    connect(m_scene, &GraphScene::modeChanged, this, &MainWindow::onModeChanged);
}

// ── Icon factory ──────────────────────────────────────────────────────────────

QIcon MainWindow::makeNodeIcon(const QColor& fill, const QString& shape)
{
    QPixmap pm(24, 24);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(fill);
    p.setPen(fill.darker(140));

    if (shape == QStringLiteral("rect"))
        p.drawRect(2, 5, 20, 14);
    else if (shape == QStringLiteral("round"))
        p.drawRoundedRect(2, 5, 20, 14, 4, 4);
    else if (shape == QStringLiteral("ellipse"))
        p.drawEllipse(2, 5, 20, 14);
    else if (shape == QStringLiteral("arrow")) {
        p.drawLine(2, 12, 18, 12);
        QPolygonF arr;
        arr << QPointF(22, 12) << QPointF(15, 8) << QPointF(15, 16);
        p.drawPolygon(arr);
    } else if (shape == QStringLiteral("cross")) {
        p.setPen(QPen(fill, 2.5, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(4, 4, 20, 20);
        p.drawLine(20, 4, 4, 20);
    } else if (shape == QStringLiteral("cursor")) {
        p.setPen(QPen(fill, 2));
        p.drawLine(4, 20, 12, 4);
        p.drawLine(12, 4, 20, 20);
        p.drawLine(8, 13, 16, 13);
    }
    return QIcon(pm);
}

// ── Toolbar ───────────────────────────────────────────────────────────────────

void MainWindow::buildToolbar()
{
    QToolBar* tb = addToolBar(QStringLiteral("Tools"));
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    tb->setIconSize(QSize(20, 20));

    auto* modeGroup = new QActionGroup(this);
    modeGroup->setExclusive(true);

    auto addMode = [&](const QString& text, const QIcon& icon,
                       const QString& shortcut, EditMode mode) -> QAction* {
        QAction* act = tb->addAction(icon, text);
        act->setShortcut(QKeySequence(shortcut));
        act->setCheckable(true);
        act->setToolTip(QStringLiteral("%1  [%2]").arg(text, shortcut));
        modeGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, mode]() { m_scene->setMode(mode); });
        return act;
    };

    QAction* selAct = addMode(QStringLiteral("Select"),
                              makeNodeIcon(QColor(120,120,120), QStringLiteral("cursor")),
                              QStringLiteral("S"), EditMode::Select);
    selAct->setChecked(true);

    tb->addSeparator();
    addMode(QStringLiteral("Add Class"),
            makeNodeIcon(QColor(74,144,196), QStringLiteral("rect")),
            QStringLiteral("C"), EditMode::AddClass);
    addMode(QStringLiteral("Add Module"),
            makeNodeIcon(QColor(80,170,100), QStringLiteral("round")),
            QStringLiteral("M"), EditMode::AddModule);
    addMode(QStringLiteral("Add Function"),
            makeNodeIcon(QColor(212,132,74), QStringLiteral("ellipse")),
            QStringLiteral("F"), EditMode::AddFunction);

    tb->addSeparator();
    addMode(QStringLiteral("Add Edge"),
            makeNodeIcon(QColor(80,80,80), QStringLiteral("arrow")),
            QStringLiteral("E"), EditMode::AddEdge);

    tb->addSeparator();
    addMode(QStringLiteral("Delete"),
            makeNodeIcon(QColor(192,48,48), QStringLiteral("cross")),
            QStringLiteral("D"), EditMode::Delete);

    tb->addSeparator();

    QAction* zoomIn = tb->addAction(QStringLiteral("Zoom In"));
    zoomIn->setShortcut(QKeySequence::ZoomIn);
    connect(zoomIn, &QAction::triggered, this, &MainWindow::zoomIn);

    QAction* zoomOut = tb->addAction(QStringLiteral("Zoom Out"));
    zoomOut->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOut, &QAction::triggered, this, &MainWindow::zoomOut);

    QAction* fitAct = tb->addAction(QStringLiteral("Fit View"));
    fitAct->setShortcut(QKeySequence(Qt::Key_Home));
    connect(fitAct, &QAction::triggered, this, &MainWindow::fitView);
}

// ── Menus ─────────────────────────────────────────────────────────────────────

void MainWindow::buildMenus()
{
    QMenu* file = menuBar()->addMenu(QStringLiteral("&File"));
    file->addAction(QStringLiteral("&Export as PNG…"), this, &MainWindow::exportToPng,
                    QKeySequence(QStringLiteral("Ctrl+E")));
    file->addSeparator();
    file->addAction(QStringLiteral("Clear &All"), this, [this]() {
        if (QMessageBox::question(this, QStringLiteral("Clear All"),
                QStringLiteral("Remove all nodes and edges?"),
                QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
            m_scene->clearAll();
    });
    file->addSeparator();
    file->addAction(QStringLiteral("&Quit"), qApp, &QApplication::quit,
                    QKeySequence::Quit);

    QMenu* help = menuBar()->addMenu(QStringLiteral("&Help"));
    help->addAction(QStringLiteral("&About"), this, [this]() {
        QMessageBox::about(this, QStringLiteral("Graph Draw"),
            QStringLiteral(
                "<b>Graph Draw</b> — Dependency Visualiser<br><br>"
                "<b>Node types:</b><br>"
                "&nbsp;<font color='#4A90C4'>■</font> <b>Class</b> — rectangle<br>"
                "&nbsp;<font color='#50AA64'>■</font> <b>Module</b> — rounded rectangle<br>"
                "&nbsp;<font color='#D48448'>●</font> <b>Function</b> — ellipse<br><br>"
                "<b>Shortcuts:</b><br>"
                "&nbsp;S — Select &nbsp; C — Add Class &nbsp; M — Add Module<br>"
                "&nbsp;F — Add Function &nbsp; E — Add Edge &nbsp; D — Delete mode<br>"
                "&nbsp;Del — remove selected &nbsp; Esc — cancel edge<br>"
                "&nbsp;Scroll — zoom &nbsp; Middle-mouse / Space+drag — pan<br>"
                "&nbsp;Double-click node or edge — rename / edit label<br>"
                "&nbsp;Ctrl+E — export PNG &nbsp; Home — fit view"));
    });
}

// ── Actions ───────────────────────────────────────────────────────────────────

void MainWindow::exportToPng()
{
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export PNG"),
        QStringLiteral("graph.png"),
        QStringLiteral("PNG Images (*.png);;All Files (*)"));
    if (path.isEmpty()) return;

    QRectF content = m_scene->itemsBoundingRect().adjusted(-40, -40, 40, 40);
    if (content.isEmpty())
        content = QRectF(0, 0, 800, 600);

    QImage image(content.size().toSize(), QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(248, 248, 252));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    m_scene->render(&painter, QRectF(), content);
    painter.end();

    if (image.save(path))
        statusBar()->showMessage(
            QStringLiteral("Exported to %1").arg(path), 4000);
    else
        QMessageBox::warning(this, QStringLiteral("Export Failed"),
                             QStringLiteral("Could not write to %1").arg(path));
}

void MainWindow::fitView()
{
    const QRectF content = m_scene->itemsBoundingRect().adjusted(-40, -40, 40, 40);
    if (!content.isEmpty())
        m_view->fitInView(content, Qt::KeepAspectRatio);
}

void MainWindow::zoomIn()  { m_view->scale(1.2, 1.2); }
void MainWindow::zoomOut() { m_view->scale(1.0 / 1.2, 1.0 / 1.2); }

void MainWindow::onModeChanged(EditMode mode)
{
    static const QMap<EditMode, QString> names = {
        {EditMode::Select,      QStringLiteral("Select")},
        {EditMode::AddClass,    QStringLiteral("Add Class")},
        {EditMode::AddModule,   QStringLiteral("Add Module")},
        {EditMode::AddFunction, QStringLiteral("Add Function")},
        {EditMode::AddEdge,     QStringLiteral("Add Edge")},
        {EditMode::Delete,      QStringLiteral("Delete")},
    };
    m_modeLabel->setText(QStringLiteral("Mode: ") + names.value(mode));
}
