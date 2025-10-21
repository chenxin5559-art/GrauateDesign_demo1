#include "modelingpointdialog.h"
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QHBoxLayout>

ModelingPointDialog::ModelingPointDialog(const QVector<double>& temperatures,
                                         const QVector<QString>& conditions,
                                         const QString& deviceName,
                                         const QVector<bool>& defaultSelections,  // 新增参数
                                         QWidget* parent)
    : QDialog(parent),
    m_selections(temperatures.size(), false)
{
    setWindowTitle(QString("建模温度点选择—%1").arg(deviceName));
    resize(600, 400);

    m_selectAllCheckBox = new QCheckBox("全选");
    connect(m_selectAllCheckBox, &QCheckBox::toggled, this, &ModelingPointDialog::onSelectAllToggled);

    m_table = new QTableWidget(temperatures.size(), 3);
    m_table->setHorizontalHeaderLabels({"温度值", "测试条件", "选择"});
    m_table->horizontalHeader()->setStretchLastSection(true);

    // 遍历行并根据默认选择初始化复选框
    for (int i = 0; i < temperatures.size(); ++i) {
        // 温度值
        QTableWidgetItem* tempItem = new QTableWidgetItem(
            QString::number(temperatures[i], 'f', 2));
        m_table->setItem(i, 0, tempItem);

        // 测试条件
        m_table->setItem(i, 1, new QTableWidgetItem(conditions[i]));

        // 复选框（根据默认选择初始化）
        QWidget* widget = new QWidget();
        QCheckBox* checkbox = new QCheckBox(widget);
        // 使用 defaultSelections 的对应值（若存在），否则默认不选
        bool isChecked = (i < defaultSelections.size()) ? defaultSelections[i] : false;
        checkbox->setChecked(isChecked);

        QHBoxLayout* layout = new QHBoxLayout(widget);
        layout->addWidget(checkbox);
        layout->setAlignment(Qt::AlignCenter);
        m_table->setCellWidget(i, 2, widget);
    }

    // 初始化全选按钮状态（若所有行都被选中则全选框勾选）
    bool allChecked = true;
    for (int i = 0; i < m_table->rowCount(); ++i) {
        QWidget* widget = m_table->cellWidget(i, 2);
        QCheckBox* checkbox = widget->findChild<QCheckBox*>();
        if (!checkbox || !checkbox->isChecked()) {
            allChecked = false;
            break;
        }
    }
    m_selectAllCheckBox->setChecked(allChecked);

    QPushButton* btnOK = new QPushButton("确定");
    QPushButton* btnCancel = new QPushButton("取消");

    connect(btnOK, &QPushButton::clicked, this, &QDialog::accept);
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);

    QVBoxLayout* mainLayout = new QVBoxLayout;
    mainLayout->addWidget(m_selectAllCheckBox);
    mainLayout->addWidget(m_table);

    QHBoxLayout* btnLayout = new QHBoxLayout;
    btnLayout->addStretch();
    btnLayout->addWidget(btnOK);
    btnLayout->addWidget(btnCancel);

    mainLayout->addLayout(btnLayout);
    setLayout(mainLayout);
}

// 全选状态变更时同步所有复选框
void ModelingPointDialog::onSelectAllToggled(bool checked)
{
    for (int i = 0; i < m_table->rowCount(); ++i) {
        QWidget* widget = m_table->cellWidget(i, 2);
        QCheckBox* checkbox = widget->findChild<QCheckBox*>();
        if (checkbox) {
            checkbox->setChecked(checked);
        }
    }
}

QVector<bool> ModelingPointDialog::getSelections() const
{
    QVector<bool> result;
    if (m_table) {
        for (int i = 0; i < m_table->rowCount(); ++i) {
            QWidget* widget = m_table->cellWidget(i, 2);
            QCheckBox* checkbox = widget->findChild<QCheckBox*>();
            result.append(checkbox ? checkbox->isChecked() : false);
        }
    }
    return result;
}


