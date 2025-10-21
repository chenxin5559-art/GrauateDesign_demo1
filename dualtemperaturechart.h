// DualTemperatureChart.h
#ifndef DUALTEMPERATURECHART_H
#define DUALTEMPERATURECHART_H

#include <QWidget>
#include <QtCharts>
#include <QDateTime>

    class DualTemperatureChart : public QWidget {
    Q_OBJECT
public:
    explicit DualTemperatureChart(QWidget *parent = nullptr);
    // 更新黑体炉温度数据
    void updateBlackbodyData(const QDateTime &timestamp, float temperature);
    // 更新恒温箱温度数据
    void updateHumidityBoxData(const QDateTime &timestamp, float temperature);
    // 更新红外测温仪数据
    void updateIrData(const QDateTime &timestamp, float toTemp, float taTemp);
    // 清除红外测温仪数据
    void clearIrData();
    // 设置红外数据可见性
    void setIrDataVisible(bool visible);

private:
    QChart *m_chart;               // 主图表
    QLineSeries *m_blackbodySeries; // 黑体炉曲线
    QLineSeries *m_humiditySeries;  // 恒温箱曲线
    QLineSeries *m_irToSeries;      // 红外TO曲线
    QLineSeries *m_irTaSeries;      // 红外TA曲线
    QDateTimeAxis *m_axisX;         // 共享X轴（时间）
    QValueAxis *m_axisY;            // 共享Y轴（温度）

    // 存储所有曲线的历史数据（近30分钟）
    QList<QPair<QDateTime, double>> m_blackbodyData;
    QList<QPair<QDateTime, double>> m_humidityData;
    QList<QPair<QDateTime, double>> m_irToData;    // 红外TO数据
    QList<QPair<QDateTime, double>> m_irTaData;    // 红外TA数据

    void trimOldData(); // 清理30分钟前的旧数据
    void updateAxisRanges(); // 更新坐标轴范围
};

#endif // DUALTEMPERATURECHART_H
