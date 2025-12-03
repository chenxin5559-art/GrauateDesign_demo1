#ifndef DUALTEMPERATURECHART_H
#define DUALTEMPERATURECHART_H

#include <QWidget>
#include <QtCharts>
#include <QDateTime>
#include <QComboBox> // 新增

QT_CHARTS_USE_NAMESPACE

    class DualTemperatureChart : public QWidget
{
    Q_OBJECT
public:
    explicit DualTemperatureChart(QWidget *parent = nullptr);

    // 更新数据接口
    void updateBlackbodyData(QDateTime time, float temp);
    void updateHumidityBoxData(QDateTime time, float temp);
    void updateIrData(QDateTime time, float to, float ta); // 红外数据接口

    void clearIrData(); // 清除红外数据
    void setIrDataVisible(bool visible); // 设置红外曲线可见性

    // 新增：时间范围枚举
    enum TimeRange {
        Last30Minutes,
        Last1Hour,
        Last2Hours,
        Last6Hours,
        Last12Hours,
        AllData
    };

private slots:
    // 新增：处理时间范围变更
    void onTimeRangeChanged(int index);

private:
    QChartView *m_chartView;
    QChart *m_chart;

    // 曲线系列
    QLineSeries *m_seriesBlackbody; // 黑体炉
    QLineSeries *m_seriesHumidityBox; // 恒温箱
    QLineSeries *m_seriesIrTO; // 红外TO
    QLineSeries *m_seriesIrTA; // 红外TA

    // 坐标轴
    QDateTimeAxis *m_axisX;
    QValueAxis *m_axisY;

    // 新增：下拉框控件
    QComboBox *m_rangeComboBox;

    // 新增：存储所有历史数据（用于重新筛选显示）
    struct DataPoint {
        QDateTime time;
        float value;
    };
    QVector<DataPoint> m_allBlackbodyData;
    QVector<DataPoint> m_allHumidityBoxData;
    QVector<DataPoint> m_allIrTOData;
    QVector<DataPoint> m_allIrTAData;

    // 新增：当前选择的时间范围
    TimeRange m_currentTimeRange = AllData;

    // 辅助函数：根据时间范围筛选并更新Series
    void refreshChartDisplay();
    void updateSeries(QLineSeries *series, const QVector<DataPoint> &allData, const QDateTime &startTime);
};

#endif // DUALTEMPERATURECHART_H
