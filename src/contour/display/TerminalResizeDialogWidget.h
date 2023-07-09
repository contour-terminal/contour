#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLabel>

class TerminalResizeDialogWidget : public QDialog
{
    Q_OBJECT

public:
    TerminalResizeDialogWidget(QMainWindow *parent);
    ~TerminalResizeDialogWidget() override = default;
    void updateSize(const QSize& size);
    void center();

private:
    QMainWindow* parent_;
    QLabel* label_;
    QVBoxLayout* layout_;
    QTimer* showTimer_;
};
