// DualTemperatureChart.cpp
#include "DualTemperatureChart.h"
#include <algorithm>

DualTemperatureChart::DualTemperatureChart(QWidget *parent) : QWidget(parent) {
    m_chart = new QChart();
    m_chart->setTitle("温度对比（黑体炉、恒温箱、红外测温仪）");
    m_chart->legend()->setVisible(true);

    // 初始化曲线
    m_blackbodySeries = new QLineSeries();
    m_blackbodySeries->setName("黑体炉温度");
    m_blackbodySeries->setColor(QColor(255, 0, 0)); // 红色

    m_humiditySeries = new QLineSeries();
    m_humiditySeries->setName("恒温箱温度");
    m_humiditySeries->setColor(QColor(0, 0, 255)); // 蓝色

    m_irToSeries = new QLineSeries();
    m_irToSeries->setName("红外TO温度");
    m_irToSeries->setColor(QColor(0, 255, 0)); // 绿色
    m_irToSeries->setVisible(false); // 默认隐藏

    m_irTaSeries = new QLineSeries();
    m_irTaSeries->setName("红外TA温度");
    m_irTaSeries->setColor(QColor(255, 165, 0)); // 橙色
    m_irTaSeries->setVisible(false); // 默认隐藏

    // 添加曲线到图表
    m_chart->addSeries(m_blackbodySeries);
    m_chart->addSeries(m_humiditySeries);
    m_chart->addSeries(m_irToSeries);
    m_chart->addSeries(m_irTaSeries);

    // 初始化坐标轴
    m_axisX = new QDateTimeAxis();
    m_axisX->setFormat("HH:mm:ss");
    m_axisX->setTitleText("时间");
    m_chart->addAxis(m_axisX, Qt::AlignBottom);

    m_axisY = new QValueAxis();
    m_axisY->setTitleText("温度(℃)");
    m_chart->addAxis(m_axisY, Qt::AlignLeft);

    // 关联所有曲线到坐标轴
    m_blackbodySeries->attachAxis(m_axisX);
    m_blackbodySeries->attachAxis(m_axisY);
    m_humiditySeries->attachAxis(m_axisX);
    m_humiditySeries->attachAxis(m_axisY);
    m_irToSeries->attachAxis(m_axisX);
    m_irToSeries->attachAxis(m_axisY);
    m_irTaSeries->attachAxis(m_axisX);
    m_irTaSeries->attachAxis(m_axisY);

    // 显示图表
    QChartView *chartView = new QChartView(m_chart);
    chartView->setRenderHint(QPainter::Antialiasing);
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(chartView);
    setLayout(layout);
}

// 清理30分钟前的旧数据
void DualTemperatureChart::trimOldData() {
    const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-1800); // 30分钟

    // 清理黑体炉数据
    while (!m_blackbodyData.isEmpty() && m_blackbodyData.first().first < cutoff)
        m_blackbodyData.removeFirst();

    // 清理恒温箱数据
    while (!m_humidityData.isEmpty() && m_humidityData.first().first < cutoff)
        m_humidityData.removeFirst();

    // 清理红外数据
    while (!m_irToData.isEmpty() && m_irToData.first().first < cutoff)
        m_irToData.removeFirst();
    while (!m_irTaData.isEmpty() && m_irTaData.first().first < cutoff)
        m_irTaData.removeFirst();
}

// 更新坐标轴范围
void DualTemperatureChart::updateAxisRanges() {
    m_axisX->setRange(
        QDateTime::currentDateTime().addSecs(-1800),
        QDateTime::currentDateTime()
        );

    // 收集所有温度数据计算Y轴范围
    QList<double> allTemps;
    for (auto &p : m_blackbodyData) allTemps.append(p.second);
    for (auto &p : m_humidityData) allTemps.append(p.second);
    for (auto &p : m_irToData) allTemps.append(p.second);
    for (auto &p : m_irTaData) allTemps.append(p.second);

    if (allTemps.isEmpty()) {
        m_axisY->setRange(0, 50);
        return;
    }

    double min = *std::min_element(allTemps.begin(), allTemps.end());
    double max = *std::max_element(allTemps.begin(), allTemps.end());
    double margin = std::max((max - min) * 0.05, 1.0); // 5%边距或至少1℃
    m_axisY->setRange(min - margin, max + margin);
}

// 更新黑体炉数据
void DualTemperatureChart::updateBlackbodyData(const QDateTime &timestamp, float temp) {
    m_blackbodyData.append({timestamp, (double)temp});
    trimOldData();
    m_blackbodySeries->clear();
    for (auto &p : m_blackbodyData)
        m_blackbodySeries->append(p.first.toMSecsSinceEpoch(), p.second);
    updateAxisRanges();
}

// 更新恒温箱数据
void DualTemperatureChart::updateHumidityBoxData(const QDateTime &timestamp, float temp) {
    m_humidityData.append({timestamp, (double)temp});
    trimOldData();
    m_humiditySeries->clear();
    for (auto &p : m_humidityData)
        m_humiditySeries->append(p.first.toMSecsSinceEpoch(), p.second);
    updateAxisRanges();
}

// 更新红外测温仪数据
// DualTemperatureChart.cpp - 完善updateIrData函数
void DualTemperatureChart::updateIrData(const QDateTime &timestamp, float toTemp, float taTemp) {
    qDebug() << "[DualTemperatureChart] 收到红外数据 - 时间:" << timestamp.toString("HH:mm:ss")
        << " TO:" << toTemp << "℃, TA:" << taTemp << "℃";

    m_irToData.append({timestamp, (double)toTemp});
    m_irTaData.append({timestamp, (double)taTemp});
    trimOldData();

    m_irToSeries->clear();
    for (auto &p : m_irToData)
        m_irToSeries->append(p.first.toMSecsSinceEpoch(), p.second);

    m_irTaSeries->clear();
    for (auto &p : m_irTaData)
        m_irTaSeries->append(p.first.toMSecsSinceEpoch(), p.second);

    updateAxisRanges();
    qDebug() << "[DualTemperatureChart] 红外数据已更新，当前TO数据点数量:" << m_irToData.size();
}

// 清除红外数据
void DualTemperatureChart::clearIrData() {
    m_irToData.clear();
    m_irTaData.clear();
    m_irToSeries->clear();
    m_irTaSeries->clear();
    updateAxisRanges();
}

// 设置红外数据可见性
void DualTemperatureChart::setIrDataVisible(bool visible) {
    m_irToSeries->setVisible(visible);
    m_irTaSeries->setVisible(visible);
}
