#include "servomotorcontroller.h"
#include <QThread>
#include <QCoreApplication>

ServoMotorController::ServoMotorController(QObject *parent)
    : QObject(parent), m_serial(new QSerialPort(this)), m_pollTimer(new QTimer(this))
{
    // 配置轮询定时器，用于检查电机是否到位
    m_pollTimer->setInterval(200);
    connect(m_pollTimer, &QTimer::timeout, this, &ServoMotorController::checkPositionStatus);
    connect(m_serial, &QSerialPort::readyRead, this, &ServoMotorController::onDataReceived);

    // 【新增】超时定时器：防止电机到位了但软件没识别到
    m_timeoutTimer = new QTimer(this);
    m_timeoutTimer->setSingleShot(true);
    connect(m_timeoutTimer, &QTimer::timeout, this, [this](){
        if(m_isMoving) {
            m_isMoving = false;
            m_pollTimer->stop();
            // 超时强制认为到位，保证流程不卡死
            emit logMessage("警告：电机运动等待超时，强制跳过等待");
            emit positionReached();
        }
    });
}

ServoMotorController::~ServoMotorController() {
    disconnectDevice();
}

bool ServoMotorController::connectDevice(const QString &portName, int baudRate) {
    if (m_serial->isOpen()) m_serial->close();

    m_serial->setPortName(portName);
    m_serial->setBaudRate(baudRate);
    m_serial->setDataBits(QSerialPort::Data8);
    m_serial->setParity(QSerialPort::NoParity);
    m_serial->setStopBits(QSerialPort::OneStop);
    m_serial->setFlowControl(QSerialPort::NoFlowControl);

    if (m_serial->open(QIODevice::ReadWrite)) {
        emit logMessage(QString("伺服电机连接成功: %1").arg(portName));
        return true;
    } else {
        emit errorOccurred("伺服电机串口打开失败");
        return false;
    }
}

void ServoMotorController::disconnectDevice() {
    if (m_serial->isOpen()) {
        m_serial->close();
        m_pollTimer->stop();
    }
}

bool ServoMotorController::isConnected() const {
    return m_serial && m_serial->isOpen();
}

void ServoMotorController::sendCommand(const QString &cmd) {
    if (!isConnected()) return;
    QByteArray data = (cmd + "\r\n").toLatin1();
    m_serial->write(data);
    m_serial->waitForBytesWritten(50);
    // 稍微延时防止指令粘包
    QThread::msleep(20);
}

// 【关键】发送全套初始化参数（防飞车配置）
void ServoMotorController::initDriverParameters() {
    emit logMessage("正在初始化电机参数...");

    sendCommand("s r0xa4 0xffff"); // 1. 先清除之前的错误
    //sendCommand("s r0xe3 100");    // 增益倍率

    // 【修改点1】提高电流限制 (如果您的电机额定电流允许，可以设大一点)
    // 例如设为 3A (300) 或 4A (400)，之前是 200
    //sendCommand("s r0x21 400");    // 峰值电流: 4.00A
    //sendCommand("s r0x22 200");    // 连续电流: 2.00A

    //sendCommand("s r0x30 1000");   // 位置比例增益

    // 【修改点2】降低加速度 (让起步更柔和，防止“闪了腰”)
    // 之前是 100000，建议改小，例如 20000 或 50000
    sendCommand("s r0xcc 20000");  // 加速度: 2000 counts/s^2
    sendCommand("s r0xcd 20000");  // 减速度: 2000 counts/s^2

    sendCommand("s r0xcb 1310720");// 最大速度
    sendCommand("s r0xc8 256");    // 相对运动模式
    sendCommand("s r0x24 21");     // 使能

    emit logMessage("电机参数初始化完成");
}

// 【关键】复位零点逻辑
void ServoMotorController::resetZeroPoint() {
    if (!isConnected()) return;

    m_buffer.clear(); // 【新增】清空缓存

    emit logMessage("正在执行位置清零...");

    // 1. 清除可能存在的报错 (Fault)
    // 这一步必须做，防止之前跳闸导致无法写入
    sendCommand("s r0xa4 0xffff");
    QThread::msleep(50);

    // 2. 【核心修改】强制将电机内部的“实际位置寄存器”设为 0
    // 依据：您提供的日志证明 s r0x32 0 是有效的
    sendCommand("s r0x32 0");
    QThread::msleep(100); // 给一点点时间让设置生效

    // 3. (可选) 如果不放心，可以再发一次初始化参数确保电流够大
    // 但如果您之前已经发过且没掉电，这里不发也可以
    initDriverParameters();

    // 4. 将软件内部的计数器也同步归零
    m_currentSoftwareCounts = 0;
    m_targetSoftwareCounts = 0;

    // 5. (可选调试) 查一下是不是真的变0了
    // sendCommand("g r0x32");

    emit logMessage("零点复位完成：硬件坐标已强制置 0");
}

int ServoMotorController::angleToCounts(double angle) {
    return static_cast<int>((angle / 360.0) * COUNTS_PER_REV);
}

double ServoMotorController::currentAngle() const {
    return (double)m_currentSoftwareCounts / COUNTS_PER_REV * 360.0;
}

// 纯相对运动指令
void ServoMotorController::moveRelative(double angle) {
    if (!isConnected()) return;

    int counts = angleToCounts(angle);
    if (counts == 0) {
        emit positionReached();
        return;
    }

    // 1. 设置相对位移量
    sendCommand(QString("s r0xca %1").arg(counts));

    // 2. 触发运动
    sendCommand("t 1");

    // 3. 更新目标位置
    m_currentSoftwareCounts += counts;
    m_targetSoftwareCounts = m_currentSoftwareCounts;

    m_isMoving = true;
    m_pollTimer->start();

    // 【修改点】启动超时定时器 (例如 20秒，根据电机转速调整)
    // 防止电机实际动了但串口没收到反馈导致死锁
    if(m_timeoutTimer) m_timeoutTimer->start(20000);

    emit logMessage(QString("电机相对运动: %1度 (%2 counts)").arg(angle).arg(counts));
}

// 通过相对运动模拟绝对定位
void ServoMotorController::moveToAbsolute(double targetAngle) {
    // 1. 计算当前角度
    double currAngle = currentAngle();

    // 2. 计算差值
    double delta = targetAngle - currAngle;

    // 3. 执行相对运动
    if (qAbs(delta) > 0.01) {
        moveRelative(delta);
    } else {
        emit positionReached();
    }
}

// 回零：计算反向距离并回退
void ServoMotorController::moveToZero() {
    moveToAbsolute(0.0);
}

void ServoMotorController::stop() {
    sendCommand("s r0x24 0"); // 去能/停止
    m_isMoving = false;
    m_pollTimer->stop();
    if(m_timeoutTimer) m_timeoutTimer->stop(); // 【新增】
}

void ServoMotorController::checkPositionStatus() {
    // 查询实际位置 (0x32)
    sendCommand("g r0x32");
}

void ServoMotorController::onDataReceived() {
    // 1. 将新数据追加到缓存
    m_buffer.append(m_serial->readAll());

    // 2. 按行处理数据 (以 \r 为结束符，Copley驱动器通常以 \r 结尾)
    while (m_buffer.contains('\r')) {
        int endIndex = m_buffer.indexOf('\r');
        QByteArray lineData = m_buffer.left(endIndex); // 取出一行
        m_buffer.remove(0, endIndex + 1); // 从缓存中移除已处理的数据 (包含 \r)

        // 去除可能的换行符 \n 和首尾空格
        QString response = QString::fromLatin1(lineData).trimmed();

        // 调试日志：看看实际上收到了什么（调试完成后可注释掉）
        // qDebug() << "Servo Raw Line:" << response;

        // 3. 解析位置数据
        // 驱动器回复格式通常是 "v 123456"
        if (m_isMoving && response.startsWith("v ")) {
            bool ok;
            // 提取 "v " 后面的数字
            long long currentDriverCounts = response.mid(2).toLongLong(&ok);

            if (ok) {
                // 计算误差
                long long diff = qAbs(currentDriverCounts - m_targetSoftwareCounts);

                // qDebug() << "Target:" << m_targetSoftwareCounts << " Current:" << currentDriverCounts << " Diff:" << diff;

                // 判断是否到位
                if (diff <= POSITION_TOLERANCE) {
                    m_isMoving = false;
                    m_pollTimer->stop();
                    if(m_timeoutTimer) m_timeoutTimer->stop(); // 停止超时计时

                    emit logMessage(QString("电机到位 (误差: %1)").arg(diff));
                    emit positionReached();
                }
            }
        }
    }
}
