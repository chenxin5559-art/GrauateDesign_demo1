#ifndef MODELINGPOINTDIALOG_H
#define MODELINGPOINTDIALOG_H

#include <QDialog>
#include <QVector>
#include <QCheckBox>
#include <QTableWidget>
#include <QCloseEvent>

class ModelingPointDialog : public QDialog
{
    Q_OBJECT
public:
    // 新增默认选择参数（defaultSelections）
    ModelingPointDialog(const QVector<double>& temperatures,
                        const QVector<QString>& conditions,
                        const QString& deviceName,
                        const QVector<bool>& defaultSelections = QVector<bool>(),  // 新参数
                        QWidget* parent = nullptr);
    QVector<bool> getSelections() const;

private slots:
    void onSelectAllToggled(bool checked);


private:
    QVector<bool> m_selections;
    QCheckBox* m_selectAllCheckBox;
    QTableWidget* m_table;
};

#endif // MODELINGPOINTDIALOG_H
