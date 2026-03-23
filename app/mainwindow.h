#pragma once
#include "graphscene.h"
#include "graphview.h"
#include <QMainWindow>

class QAction;
class QActionGroup;
class QLabel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void exportToPng();
    void saveGraph();
    void loadGraph();
    void fitView();
    void zoomIn();
    void zoomOut();
    void onModeChanged(EditMode mode);

private:
    void buildToolbar();
    void buildMenus();
    // Generates a small filled-shape icon in the given colour for toolbar buttons
    static QIcon makeNodeIcon(const QColor& fill, const QString& shape);

    GraphScene* m_scene = nullptr;
    GraphView*  m_view  = nullptr;
    QLabel*     m_modeLabel = nullptr;
};
