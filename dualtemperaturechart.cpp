#include "dualtemperaturechart.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QDebug>

DualTemperatureChart::DualTemperatureChart(QWidget *parent) : QWidget(parent)
{
    // 1. 创建图表对象
    m_chart = new QChart();
    m_chart->setTitle("实时温度曲线");
    m_chart->setAnimationOptions(QChart::NoAnimation); // 实时数据建议关闭动画，避免卡顿

    // 2. 创建曲线系列
    m_seriesBlackbody = new QLineSeries();
    m_seriesBlackbody->setName("黑体炉温度");
    m_seriesBlackbody->setColor(Qt::red);

    m_seriesHumidityBox = new QLineSeries();
    m_seriesHumidityBox->setName("恒温箱温度");
    m_seriesHumidityBox->setColor(Qt::blue);

    m_seriesIrTO = new QLineSeries();
    m_seriesIrTO->setName("红外目标温度(TO)");
    m_seriesIrTO->setColor(Qt::darkGreen); // 深绿色

    m_seriesIrTA = new QLineSeries();
    m_seriesIrTA->setName("红外环境温度(TA)");
    m_seriesIrTA->setColor(Qt::darkYellow); // 暗黄色

    // 3. 添加系列到图表
    m_chart->addSeries(m_seriesBlackbody);
    m_chart->addSeries(m_seriesHumidityBox);
    m_chart->addSeries(m_seriesIrTO);
    m_chart->addSeries(m_seriesIrTA);

    // 初始隐藏红外曲线
    m_seriesIrTO->setVisible(false);
    m_seriesIrTA->setVisible(false);

    // 4. 创建坐标轴
    m_axisX = new QDateTimeAxis();
    m_axisX->setTickCount(10);
    m_axisX->setFormat("HH:mm:ss");
    m_axisX->setTitleText("时间");
    m_chart->addAxis(m_axisX, Qt::AlignBottom);

    m_axisY = new QValueAxis();
    m_axisY->setRange(-50, 100); // 初始范围，后续自动调整
    m_axisY->setTitleText("温度 (℃)");
    m_chart->addAxis(m_axisY, Qt::AlignLeft);

    // 关联系列和坐标轴
    m_seriesBlackbody->attachAxis(m_axisX);
    m_seriesBlackbody->attachAxis(m_axisY);
    m_seriesHumidityBox->attachAxis(m_axisX);
    m_seriesHumidityBox->attachAxis(m_axisY);
    m_seriesIrTO->attachAxis(m_axisX);
    m_seriesIrTO->attachAxis(m_axisY);
    m_seriesIrTA->attachAxis(m_axisX);
    m_seriesIrTA->attachAxis(m_axisY);

    // 5. 创建ChartView
    m_chartView = new QChartView(m_chart);
    m_chartView->setRenderHint(QPainter::Antialiasing);

    // ================== 新增：时间范围选择控件 ==================
    QWidget *controlPanel = new QWidget(this);
    QHBoxLayout *controlLayout = new QHBoxLayout(controlPanel);
    controlLayout->setContentsMargins(0, 0, 0, 0);

    QLabel *rangeLabel = new QLabel("显示时间范围:", this);
    rangeLabel->setFont(QFont("PingFang SC Regular", 10));

    m_rangeComboBox = new QComboBox(this);
    m_rangeComboBox->addItem("全部数据", AllData);
    m_rangeComboBox->addItem("近30分钟", Last30Minutes);
    m_rangeComboBox->addItem("近1小时", Last1Hour);
    m_rangeComboBox->addItem("近2小时", Last2Hours);
    m_rangeComboBox->addItem("近6小时", Last6Hours);
    m_rangeComboBox->addItem("近12小时", Last12Hours);

    // 默认选中"全部数据"
    m_rangeComboBox->setCurrentIndex(0);

    // 样式美化
    m_rangeComboBox->setStyleSheet(
        "QComboBox { border: 1px solid #ccc; border-radius: 3px; padding: 2px 5px; min-width: 100px; }"
        "QComboBox::drop-down { border-left: 1px solid #ccc; }"
        );

    controlLayout->addStretch(); // 弹簧，让控件靠右
    controlLayout->addWidget(rangeLabel);
    controlLayout->addWidget(m_rangeComboBox);

    // 连接信号
    connect(m_rangeComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DualTemperatureChart::onTimeRangeChanged);

    // ================== 布局设置 ==================
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(controlPanel); // 先放控制栏
    mainLayout->addWidget(m_chartView);  // 再放图表
    setLayout(mainLayout);
}

void DualTemperatureChart::updateBlackbodyData(QDateTime time, float temp)
{
    m_allBlackbodyData.append({time, temp});
    refreshChartDisplay();
}

void DualTemperatureChart::updateHumidityBoxData(QDateTime time, float temp)
{
    m_allHumidityBoxData.append({time, temp});
    refreshChartDisplay();
}

void DualTemperatureChart::updateIrData(QDateTime time, float to, float ta)
{
    m_allIrTOData.append({time, to});
    m_allIrTAData.append({time, ta});
    refreshChartDisplay();
}

void DualTemperatureChart::clearIrData()
{
    m_seriesIrTO->clear();
    m_seriesIrTA->clear();
    m_allIrTOData.clear();
    m_allIrTAData.clear();
    // 注意：这里不清空黑体和恒温箱的历史数据，只清红外
    refreshChartDisplay();
}

void DualTemperatureChart::setIrDataVisible(bool visible)
{
    m_seriesIrTO->setVisible(visible);
    m_seriesIrTA->setVisible(visible);
    // 重新缩放坐标轴以适应可见性变化
    refreshChartDisplay();
}

void DualTemperatureChart::onTimeRangeChanged(int index)
{
    m_currentTimeRange = static_cast<TimeRange>(m_rangeComboBox->itemData(index).toInt());
    refreshChartDisplay();
}

void DualTemperatureChart::refreshChartDisplay()
{
    QDateTime now = QDateTime::currentDateTime();
    QDateTime startTime;

    // 1. 计算起始时间
    switch (m_currentTimeRange) {
    case Last30Minutes: startTime = now.addSecs(-30 * 60); break;
    case Last1Hour:     startTime = now.addSecs(-60 * 60); break;
    case Last2Hours:    startTime = now.addSecs(-2 * 60 * 60); break;
    case Last6Hours:    startTime = now.addSecs(-6 * 60 * 60); break;
    case Last12Hours:   startTime = now.addSecs(-12 * 60 * 60); break;
    case AllData: default: startTime = QDateTime::fromMSecsSinceEpoch(0); break; // 极早时间
    }

    // 2. 更新各条曲线
    updateSeries(m_seriesBlackbody, m_allBlackbodyData, startTime);
    updateSeries(m_seriesHumidityBox, m_allHumidityBoxData, startTime);

    if (m_seriesIrTO->isVisible()) {
        updateSeries(m_seriesIrTO, m_allIrTOData, startTime);
        updateSeries(m_seriesIrTA, m_allIrTAData, startTime);
    }

    // 3. 动态调整坐标轴范围
    // X轴范围
    if (m_currentTimeRange == AllData) {
        // 如果是全部数据，且没有数据，默认显示当前时间前后
        if (m_allBlackbodyData.isEmpty() && m_allHumidityBoxData.isEmpty()) {
            m_axisX->setRange(now.addSecs(-60), now.addSecs(10));
        } else {
            // 找最早的时间点
            QDateTime minTime = now;
            if(!m_allBlackbodyData.isEmpty()) minTime = qMin(minTime, m_allBlackbodyData.first().time);
            if(!m_allHumidityBoxData.isEmpty()) minTime = qMin(minTime, m_allHumidityBoxData.first().time);
            m_axisX->setRange(minTime, now.addSecs(5)); // 留点余量
        }
    } else {
        // 固定时间窗口
        m_axisX->setRange(startTime, now.addSecs(5));
    }

    // Y轴范围自动适应（仅计算可见范围内的数据）
    float minTemp = 1000.0f;
    float maxTemp = -1000.0f;
    bool hasData = false;

    auto checkRange = [&](const QVector<DataPoint>& data, bool visible) {
        if (!visible) return;
        for (const auto& pt : data) {
            if (pt.time >= startTime) {
                if (pt.value < minTemp) minTemp = pt.value;
                if (pt.value > maxTemp) maxTemp = pt.value;
                hasData = true;
            }
        }
    };

    checkRange(m_allBlackbodyData, m_seriesBlackbody->isVisible());
    checkRange(m_allHumidityBoxData, m_seriesHumidityBox->isVisible());
    checkRange(m_allIrTOData, m_seriesIrTO->isVisible());
    checkRange(m_allIrTAData, m_seriesIrTA->isVisible());

    if (hasData) {
        float margin = (maxTemp - minTemp) * 0.1f; // 上下留10%余量
        if (margin < 1.0f) margin = 1.0f; // 最小余量
        m_axisY->setRange(minTemp - margin, maxTemp + margin);
    } else {
        m_axisY->setRange(0, 50); // 默认范围
    }
}

void DualTemperatureChart::updateSeries(QLineSeries *series, const QVector<DataPoint> &allData, const QDateTime &startTime)
{
    QVector<QPointF> points;
    for (const auto &pt : allData) {
        if (pt.time >= startTime) {
            points.append(QPointF(pt.time.toMSecsSinceEpoch(), pt.value));
        }
    }
    series->replace(points);
}
