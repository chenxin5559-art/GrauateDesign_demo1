#include "dataexcelprocessor.h"
#include <QFileDialog>
#include <QDebug>

DataExcelProcessor::DataExcelProcessor(QObject* parent)
    : QObject(parent)
{}

void DataExcelProcessor::startProcessing(ProcessType type, const QString& sourcePath,
                                         const QString& outputPath,
                                         const QString& templatePath)
{
    clearError(); // 清空之前的错误

    if (!QFile::exists(sourcePath)) {
        m_lastError = "源文件不存在：" + sourcePath;
        emit errorOccurred(m_lastError);
        return;
    }

    QFuture<void> future = QtConcurrent::run([=]() {
        try {
            switch(type) {
            case StandardData:
                processStandard(sourcePath);
                break;
            case SingleHead:
                processSingleHead(sourcePath);
                break;
            case MultiHead:
                processMultiHead(sourcePath);
                break;
            case MergeFiles:
                if (templatePath.isEmpty()) {
                    m_lastError = "合并文件需要提供模板路径！";
                    QMetaObject::invokeMethod(this, [this] {
                        emit errorOccurred(m_lastError);
                    }, Qt::QueuedConnection);
                    return;
                }
                mergeFiles(sourcePath, outputPath, templatePath);
                break;
            }
        } catch (const std::exception& e) {
            m_lastError = "处理过程中发生异常：" + QString(e.what());
            QMetaObject::invokeMethod(this, [this] {
                emit errorOccurred(m_lastError);
            }, Qt::QueuedConnection);
        } catch (...) {
            m_lastError = "处理过程中发生未知异常";
            QMetaObject::invokeMethod(this, [this] {
                emit errorOccurred(m_lastError);
            }, Qt::QueuedConnection);
        }
    });
}

// 标准数据处理实现
void DataExcelProcessor::processStandard(const QString& excelPath)
{
    QXlsx::Document xlsx(excelPath);

    // 读取端口号
    QString portNumber = xlsx.read(1, 5).toString();

    // 建立时间-行号映射
    QMap<QDateTime, int> dateTimeRowMap;
    int row = 3;
    const int maxEmptyRows = 5;
    int emptyCount = 0;

    while (emptyCount < maxEmptyRows) {
        // 读取A列（用于判断空行）
        QVariant aCell = xlsx.read(row, 1);
        bool aIsEmpty = aCell.isNull() || aCell.toString().trimmed().isEmpty();

        // 读取日期（B列）
        QVariant dateCell = xlsx.read(row, 2);
        QDate date = dateCell.toDate();

        // 读取时间（C列）
        QVariant timeCell = xlsx.read(row, 3);
        QTime time;

        // 类型优先级：1.直接获取QTime 2.数值格式 3.文本格式
        bool timeIsEmpty = false;
        if (timeCell.isNull() || timeCell.toString().trimmed().isEmpty()) {
            timeIsEmpty = true;
        } else if (timeCell.userType() == QMetaType::QTime) {
            time = timeCell.toTime();
        } else if (timeCell.canConvert<double>()) {
            double excelTime = timeCell.toDouble();
            if (excelTime == 0.0) {
                timeIsEmpty = true;
            } else {
                int totalSeconds = static_cast<int>(excelTime * 86400);
                time = QTime(0, 0).addSecs(totalSeconds);
            }
        } else {
            QString timeStr = timeCell.toString().trimmed();
            time = QTime::fromString(timeStr, "HH:mm:ss");
            if (!time.isValid()) {
                time = QTime::fromString(timeStr, "HH:mm");
            }
            if (!time.isValid()) {
                timeIsEmpty = true;
            }
        }

        // 检测空行（日期或时间有一个无效 且 A列也是空的）
        bool isEmptyRow = (date.isValid() || timeIsEmpty) ? false : aIsEmpty;
        // 等价于：(date无效 || timeIsEmpty) 并且 aIsEmpty

        // 构建日期时间
        QDateTime dt;
        if (date.isValid() && time.isValid()) {
            dt = QDateTime(date, time);
            if (dt.isValid()) {
                dateTimeRowMap[dt] = row;
                emptyCount = 0;
            } else {
                ++emptyCount;
            }
        } else {
            ++emptyCount;
        }

        // 对空行进行特殊处理
        if (isEmptyRow) {
            xlsx.write(row, 2, QVariant()); // 清空B列
            xlsx.write(row, 3, QVariant()); // 清空C列
        }
        ++row;
    }

    // 处理温度数据
    QMap<QDateTime, QVector<double>> tempDataMap;
    for (auto it = dateTimeRowMap.begin(); it != dateTimeRowMap.end(); ++it) {
        const QDateTime& dt = it.key();
        QString txtFile = findMatchingTxtFile(excelPath, portNumber, dt.date());
        if (!txtFile.isEmpty()) {
            QString fullPath = QFileInfo(excelPath).path() + "/" + txtFile;
            tempDataMap[dt] = processTxtFile(fullPath, dt);
        }
    }

    // 写入Excel
    writeTemperatures(xlsx, tempDataMap, dateTimeRowMap);

    // 保存结果
    QString outputPath = excelPath;
    outputPath.replace(".xlsx", "_processed.xlsx");
    if (!xlsx.saveAs(outputPath)) {
        emit errorOccurred("文件保存失败");
        return;
    }

    emit operationCompleted(true, outputPath);
}

// 单头数据处理实现
void DataExcelProcessor::processSingleHead(const QString& excelPath)
{
    qDebug() << "开始处理 Excel：" << excelPath;

    QXlsx::Document xlsx(excelPath);

    // 遍历所有工作表，筛选出包含 "单头" 的工作表
    QStringList targetSheets;
    foreach (const QString &sheetName, xlsx.sheetNames()) {
        if (sheetName.contains("单头")) {
            targetSheets.append(sheetName);
        }
    }

    qDebug() << "筛选出的单头工作表：" << targetSheets;

    foreach (const QString &sheetName, targetSheets) {
        xlsx.selectSheet(sheetName);
        qDebug() << "当前处理工作表：" << sheetName;

        // 处理COM端口列
        const QVector<int> comCols = {5, 9, 13, 17};
        foreach (int col, comCols) {
            QString comInfo = xlsx.read(2, col).toString();
            if (!comInfo.contains("COM")) {
                qDebug() << "列 " << col << " 端口信息无效：" << comInfo;
                continue;
            }

            // 解析端口号
            QString port = comInfo.split("-").last().trimmed();
            qDebug() << "解析出的端口号：" << port;

            // 建立时间-行号映射
            QMap<QDateTime, int> dateTimeRowMap;
            int row = 3;
            const int maxEmptyRows = 5;
            int emptyCount = 0;

            while (emptyCount < maxEmptyRows) {
                // 读取日期（B列）
                QDate date = xlsx.read(row, 2).toDate();

                // 读取时间（C列）
                QVariant timeCell = xlsx.read(row, 3);
                QTime time;

                // 类型优先级：1.直接获取QTime 2.数值格式 3.文本格式
                if (timeCell.userType() == QMetaType::QTime) {
                    // 情况1：QXlsx已直接解析为QTime
                    time = timeCell.toTime();
                } else if (timeCell.canConvert<double>()) {
                    // 情况2：数值格式（Excel内部时间）
                    double excelTime = timeCell.toDouble();
                    int totalSeconds = static_cast<int>(excelTime * 86400);
                    time = QTime(0, 0).addSecs(totalSeconds);
                } else {
                    // 情况3：文本格式（兼容带秒的情况）
                    QString timeStr = timeCell.toString().trimmed();
                    time = QTime::fromString(timeStr, "HH:mm:ss");
                    if (!time.isValid()) {
                        time = QTime::fromString(timeStr, "HH:mm");
                    }
                }

                // 构建日期时间
                QDateTime dt;
                if (date.isValid() && time.isValid()) {
                    dt = QDateTime(date, time);
                    if (dt.isValid()) {
                        dateTimeRowMap[dt] = row;
                        emptyCount = 0;
                    } else {
                        ++emptyCount;
                    }
                } else {
                    ++emptyCount;
                }
                ++row;

            }

            // 处理温度数据
            QMap<QDateTime, QVector<double>> tempDataMap;
            foreach (const QDateTime &dt, dateTimeRowMap.keys()) {
                QString txtFile = findMatchingTxtFile(excelPath, port, dt.date());
                if (!txtFile.isEmpty()) {
                    QString fullPath = QFileInfo(excelPath).path() + "/" + txtFile;
                    qDebug() << "找到匹配的 TXT 文件：" << fullPath << " 对应时间：" << dt.toString("yyyy-MM-dd HH:mm");

                    QVector<double> temps = processSingleHeadTxtFile(fullPath, dt);
                    qDebug() << "提取到的温度数据：" << temps;

                    tempDataMap[dt] = temps;
                } else {
                    qDebug() << "未找到匹配的 TXT 文件：" << dt.toString("yyyy-MM-dd");
                }
            }

            // 写入数据
            foreach (const QDateTime &dt, tempDataMap.keys()) {
                int row = dateTimeRowMap[dt];
                QVector<double> temps = tempDataMap[dt];
                qDebug() << "写入 Excel：" << sheetName << " 行：" << row << " 列起始：" << col
                         << " 温度数据：" << temps;

                for (int i = 0; i < 3; ++i) {
                    xlsx.write(row, col + i, temps.value(i, 65535));
                }
            }
        }
    }

    QString outputPath = excelPath;
    bool saveSuccess = xlsx.saveAs(outputPath);

    if (saveSuccess) {
        qDebug() << "单头数据处理完成，保存至：" << outputPath;
        emit operationCompleted(true, outputPath);
    } else {
        qDebug() << "单头数据保存失败！路径：" << outputPath;
        emit operationCompleted(false, outputPath);
    }
}

// 多头数据处理主函数
void DataExcelProcessor::processMultiHead(const QString& excelPath)
{
    qDebug() << "开始处理多头数据文件:" << excelPath;

    QXlsx::Document xlsx(excelPath);

    // 筛选所有含 "多" 的工作表
    QStringList multiHeadSheets;
    foreach (const QString &sheetName, xlsx.sheetNames()) {
        if (sheetName.contains("多")) {
            multiHeadSheets.append(sheetName);
        }
    }

    qDebug() << "找到" << multiHeadSheets.size() << "个包含'多'的工作表";

    foreach (const QString &sheetName, multiHeadSheets) {
        xlsx.selectSheet(sheetName);
        emit progressUpdated(20);
        qDebug() << "正在处理工作表:" << sheetName;

        // 从工作表名称提取端口号 (假设格式："COM9-多14")
        static const QRegularExpression regex(R"(COM(\d+)-多(\d+))");
        QRegularExpressionMatch match = regex.match(sheetName);
        if (!match.hasMatch()) {
            emit errorOccurred("无法解析工作表端口号：" + sheetName);
            qDebug() << "警告: 无法从工作表名称解析端口号 -" << sheetName;
            continue;
        }

        QString portNumber = "COM" + match.captured(1);
        qDebug() << "从工作表名称解析出端口号:" << portNumber;

        // 建立时间索引（B列日期，C列时间）
        QMap<QDateTime, int> dateTimeRowMap;
        int row = 4;
        int emptyCount = 0;
        const int maxEmptyRows = 10;

        qDebug() << "开始建立时间-行号映射...";
        while (emptyCount < maxEmptyRows) {
            // 读取日期
            QDate date = xlsx.read(row, 2).toDate();

            // 读取时间
            QVariant timeCell = xlsx.read(row, 3);
            QTime time;

            if (timeCell.userType() == QMetaType::QTime) {
                time = timeCell.toTime();
            } else if (timeCell.canConvert<double>()) {
                double excelTime = timeCell.toDouble();
                int totalSeconds = static_cast<int>(excelTime * 86400);
                time = QTime(0, 0).addSecs(totalSeconds);
            } else {
                QString timeStr = timeCell.toString().trimmed();
                time = QTime::fromString(timeStr, "HH:mm:ss");
                if (!time.isValid()) {
                    time = QTime::fromString(timeStr, "HH:mm");
                }
            }

            // 构建日期时间
            QDateTime dt;
            if (date.isValid() && time.isValid()) {
                dt = QDateTime(date, time);
                dateTimeRowMap[dt] = row;
                emptyCount = 0;
                qDebug() << "成功解析时间-行号映射:" << dt.toString("yyyy-MM-dd HH:mm") << "-> 行" << row;
            } else {
                ++emptyCount;
                qDebug() << "第" << row << "行时间解析失败 - 日期:" << date << "时间:" << time;

                // 当时间解析失败时，清空B列和C列的内容
                xlsx.write(row, 2, QVariant()); // 清空B列
                xlsx.write(row, 3, QVariant()); // 清空C列
            }
            ++row;
        }

        qDebug() << "时间-行号映射建立完成，共找到" << dateTimeRowMap.size() << "个有效时间点";

        // 处理温度数据
        int totalSteps = dateTimeRowMap.size();
        int currentStep = 0;
        qDebug() << "开始处理温度数据，总行数:" << totalSteps;

        foreach (const QDateTime &dt, dateTimeRowMap.keys()) {
            QString txtFile = findMatchingTxtFile(excelPath, portNumber, dt.date());
            if (!txtFile.isEmpty()) {
                QString fullPath = QDir(QFileInfo(excelPath).path()).filePath(txtFile);
                qDebug() << "找到匹配的TXT文件:" << fullPath << "对应日期:" << dt.date().toString("yyyy-MM-dd");

                QVector<double> temps = processMultiHeadTxtFile(fullPath, dt);

                // 写入Excel（E-M列共9个温度值）
                int targetRow = dateTimeRowMap[dt];
                qDebug() << "准备写入第" << targetRow << "行的温度数据";

                for (int i = 0; i < 9; ++i) {
                    QVariant value = (i < temps.size() && temps[i] != 65535) ? QVariant(temps[i]) : QVariant("///");
                    xlsx.write(targetRow, 5 + i, value);
                    qDebug() << "写入第" << targetRow << "行，第" << (5 + i) << "列的数据:" << value.toString();
                }
            } else {
                qDebug() << "未找到日期为" << dt.date().toString("yyyy-MM-dd") << "的TXT文件";
            }

            emit progressUpdated(20 + (++currentStep * 60 / totalSteps));
            if (currentStep % 10 == 0 || currentStep == totalSteps) {
                qDebug() << "进度更新:" << (20 + (currentStep * 60 / totalSteps)) << "%，已处理" << currentStep << "/" << totalSteps;
            }
        }

        qDebug() << "工作表" << sheetName << "处理完成";
    }

    // 保存处理结果
    QString baseName = QFileInfo(excelPath).completeBaseName();
    // 修复保存路径，添加_processed后缀
    QString outputPath = QFileInfo(excelPath).path() + "/" + baseName + ".xlsx";

    if (!xlsx.saveAs(outputPath)) {
        emit errorOccurred("多头文件保存失败");
        qDebug() << "错误: 保存文件失败 -" << outputPath;
        return;
    }

    qDebug() << "多头数据处理完成，结果保存至:" << outputPath;
    emit operationCompleted(true, outputPath);
}


// 单头TXT文件处理
QVector<double> DataExcelProcessor::processSingleHeadTxtFile(const QString& txtFilePath,
                                                             const QDateTime& dateTime)
{
    QFile file(txtFilePath);
    QVector<double> result(3, 65535); // 初始化为无效值
    QVector<QVector<double>> minuteData(3); // 存储每分钟各通道的有效数据
    int validCount = 0;

    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        // 目标时间格式改为只精确到分钟（忽略秒）
        QString targetMinute = dateTime.toString("yyyy-MM-dd HH:mm");

        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (!line.startsWith("[R:") || !line.contains(" ST,")) continue;

            // 提取时间部分
            int startIndex = line.indexOf("[R:") + 3;
            int endIndex = line.indexOf("] ST,");
            if (startIndex == -1 || endIndex == -1 || endIndex <= startIndex) continue;
            QString timePart = line.mid(startIndex, endIndex - startIndex);

            // 匹配到分钟（忽略秒）
            if (!timePart.startsWith(targetMinute)) continue;

            // 分割数据并清理空格
            QStringList parts = line.split(',');
            for (auto& part : parts) part = part.trimmed();

            // 温度提取逻辑（与原代码一致）
            bool ok1 = false, ok2 = false, ok3 = false;
            double temp1 = 65535, temp2 = 65535, temp3 = 65535;

            if (parts.size() >= 4) {
                temp1 = parts[2].toDouble(&ok1);
            }

            if (parts.size() >= 5) {
                QString temp2_raw = parts[3];
                if (temp2_raw.contains('|')) {
                    QStringList temp2_split = temp2_raw.split('|');
                    if (temp2_split.size() >= 2) {
                        temp2 = temp2_split[0].toDouble(&ok2);
                        temp3 = temp2_split[1].toDouble(&ok3);
                    }
                } else {
                    if (parts.size() >= 6) {
                        temp2 = parts[3].toDouble(&ok2);
                        temp3 = parts[4].toDouble(&ok3);
                    }
                }
            }

            // 有效性验证并收集数据（修改点）
            if (ok1 && temp1 >= -40.0 && temp1 <= 150.0) {
                minuteData[0].append(temp1);
                validCount++;
            }
            if (ok2 && temp2 >= -40.0 && temp2 <= 150.0) {
                minuteData[1].append(temp2);
                validCount++;
            }
            if (ok3 && temp3 >= -40.0 && temp3 <= 150.0) {
                minuteData[2].append(temp3);
                validCount++;
            }
        }
        file.close();

        // 计算平均值（新增逻辑）
        for (int i = 0; i < 3; i++) {
            if (!minuteData[i].isEmpty()) {
                double sum = 0;
                for (double temp : minuteData[i]) {
                    sum += temp;
                }
                result[i] = sum / minuteData[i].size(); // 计算平均值
            } else {
                result[i] = 65535; // 无数据时保持无效值
            }
        }

        // 无有效数据时标记为无效
        if (validCount == 0) {
            result.fill(65535);
        }

        // ===== 新增：温度范围检查 =====
        if (validCount > 0) {
            double temp1Avg = result[0];
            double temp2Avg = result[1];

            // 检查第一和第二通道温度是否在-40~90℃范围内
            bool temp1OutOfRange = (temp1Avg < -40.0 || temp1Avg > 90.0);
            bool temp2OutOfRange = (temp2Avg < -40.0 || temp2Avg > 90.0);

            if (temp1OutOfRange || temp2OutOfRange) {
                QString errorMsg = QString("温度超出正常范围\n"
                                           "目标时间: %1\n"
                                           "通道1平均温度: %.2f℃ (正常范围: -40~90℃)\n"
                                           "通道2平均温度: %.2f℃ (正常范围: -40~90℃)")
                                       .arg(dateTime.toString("yyyy-MM-dd HH:mm:ss"))
                                       .arg(temp1Avg)
                                       .arg(temp2Avg);

                m_lastError = errorMsg;
                emit errorOccurred(errorMsg);
            }

        }

    } else {
        emit errorOccurred("无法打开单头文件：" + txtFilePath);
    }
    return result;
}


// 多头TXT文件处理（优化版）
QVector<double> DataExcelProcessor::processMultiHeadTxtFile(const QString& txtFilePath,
                                                            const QDateTime& targetDateTime)
{
    QFile file(txtFilePath);
    QVector<double> result(9, 65535); // 9个温度通道，默认无效值65535
    QVector<QVector<double>> channelData(9);
    const QString targetTimePrefix = targetDateTime.toString("yyyy-MM-dd HH:mm");

    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();

            // 有效性检查
            if (!line.startsWith("[R:") || !line.contains(" ST,")) continue;
            if (!line.contains(targetTimePrefix)) continue;

            // 解析数据部分
            QStringList parts = line.split(',', Qt::SkipEmptyParts);
            if (parts.size() < 19) continue; // 确保数据行长度足够

            // 提取9个温度值
            QVector<int> indices = {6, 7, 8, 11, 12, 13, 16, 17, 18};
            QVector<double> temps;

            for (int idx : indices) {
                QString tempStr = parts.value(idx).trimmed();
                bool ok;
                double temp = tempStr.toDouble(&ok) / 100.0; // 温度值需要除以100
                if (ok && temp >= -40.0 && temp <= 150.0) {
                    temps.append(temp);
                } else {
                    temps.append(65535); // 无效数据填充65535
                }
            }

            // 只有当至少7个通道有效时，才记录该行数据
            int validCount = std::count_if(temps.begin(), temps.end(),
                                           [](double v){ return v != 65535; });
            if (validCount >= 7) {
                for (int i = 0; i < 9; ++i) {
                    if (temps[i] != 65535) {
                        channelData[i].append(temps[i]);
                    }
                }
            }
        }
        file.close();

        // 计算每个通道的分钟平均值
        for (int i = 0; i < 9; ++i) {
            if (!channelData[i].isEmpty()) {
                result[i] = std::accumulate(channelData[i].begin(),
                                            channelData[i].end(), 0.0) / channelData[i].size();
            }
        }

        // ===== 新增：温度范围检查 =====
        for (int i = 0; i < 9; ++i) {
            double tempAvg = result[i];

            if (tempAvg < -40.0 || tempAvg > 90.0) {
                QString errorMsg = QString("温度超出正常范围\n"
                                           "文件: %1\n"
                                           "目标时间: %2\n"
                                           "通道%3平均温度: %.2f℃ (正常范围: -40~90℃)")
                                       .arg(txtFilePath)
                                       .arg(targetDateTime.toString("yyyy-MM-dd HH:mm:ss"))
                                       .arg(i + 1)
                                       .arg(tempAvg);

                m_lastError = errorMsg;
                emit errorOccurred(errorMsg);
            }
        }
    } else {
        emit errorOccurred("无法打开多头文件：" + txtFilePath);
    }
    return result;
}


QVector<double> DataExcelProcessor::processTxtFile(const QString& filePath,
                                                   const QDateTime& targetDateTime)
{
    QFile file(filePath);
    QVector<double> result(16, 65535);
    QVector<QVector<double>> channelData(16);

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_lastError = "无法打开文件：" + filePath;
        emit errorOccurred(m_lastError);
        return result;
    }

    QTextStream in(&file);
    QString targetTime = targetDateTime.toString("yyyy-MM-dd HH:mm");
    qDebug() << "目标日期时间：" << targetTime;
    int validLines = 0;

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (!line.contains(targetTime)) continue;

        QStringList parts = line.split(',');
        if (parts.size() < 19) continue;

        // 解析温度值
        for (int i = 3; i <= 18; ++i) {
            QString tempStr = parts[i].split(':').last().trimmed();
            bool ok;
            double temp = tempStr.toDouble(&ok);
            if (ok && temp >= -40.0 && temp <= 150.0) {
                channelData[i-3].append(temp);
            }
        }
        validLines++;
    }

    file.close();

    if (validLines == 0) {
        qDebug() << "警告：TXT文件中无匹配时间的记录，文件：" << filePath;

        m_lastError = QString("TXT文件中无匹配时间的记录\n文件: %1\n目标时间: %2")
                          .arg(filePath).arg(targetTime);
        emit errorOccurred(m_lastError);
    }

    // 计算平均值
    for (int i = 0; i < 16; ++i) {
        if (!channelData[i].isEmpty()) {
            result[i] = std::accumulate(channelData[i].begin(), channelData[i].end(), 0.0) / channelData[i].size();
        }
    }

    return result;
}

// 合并Excel文件实现
void DataExcelProcessor::mergeFiles(const QString& file1, const QString& file2,
                                    const QString& templatePath) {
    // 加载模板文件
    QXlsx::Document templateXlsx(templatePath);
    if (!QFile::exists(templatePath) || !templateXlsx.load()) {
        emit errorOccurred("模板文件加载失败");
        return;
    }

    // 加载数据文件（箱内、箱外）
    QXlsx::Document xlsx1(file1);
    QXlsx::Document xlsx2(file2);
    if (!QFile::exists(file1) || !xlsx1.load() || !QFile::exists(file2) || !xlsx2.load()) {
        emit errorOccurred("数据文件加载失败");
        return;
    }

    // **从箱内 & 箱外 Excel 读取标准温度数据**
    QMap<QString, double> standardTempData;
    loadStandardTemperature(xlsx1, standardTempData);
    loadStandardTemperature(xlsx2, standardTempData);

    // 遍历所有工作表，跳过“标准”工作表
    for (const QString& sheetName : templateXlsx.sheetNames()) {
        if (sheetName == "标准") continue;
        if (!xlsx1.sheetNames().contains(sheetName) || !xlsx2.sheetNames().contains(sheetName)) {
            continue;
        }

        // 选择当前工作表
        templateXlsx.selectSheet(sheetName);
        xlsx1.selectSheet(sheetName);
        xlsx2.selectSheet(sheetName);

        // 复制箱内数据
        int lastRow = 4;
        lastRow = copySheetData(xlsx1, templateXlsx, 4, lastRow, "箱内", standardTempData);

        // 复制箱外数据
        lastRow = copySheetData(xlsx2, templateXlsx, 4, lastRow, "箱外", standardTempData);

        // 按 D 列排序（降序）
        sortSheetData(templateXlsx, 4, lastRow, 4);
    }

    // 生成输出文件
    QString outputPath = QFileInfo(file1).path() + "/多头箱内箱外合并结果_" +
                         QDateTime::currentDateTime().toString("yyyyMMddHHmmss") + ".xlsx";

    // QString outputPath = "E:/研究生学习资料/研二/高低温实验数据/多头2025.5新/多头箱内箱外合并结果_20250325160303.xlsx";

    if (!templateXlsx.saveAs(outputPath)) {
        emit errorOccurred("合并文件保存失败");
        return;
    }

    emit operationCompleted(true, outputPath);
}

void DataExcelProcessor::generateTemplateExcelforMulitiHead(const QString& file1, const QString& file2, QString& outputTemplatePath) {
    // 加载箱内和箱外的 Excel 文件
    QXlsx::Document xlsx1(file1);
    QXlsx::Document xlsx2(file2);

    if (!xlsx1.load() || !xlsx2.load()) {
        emit errorOccurred("箱内或箱外数据文件加载失败");
        return;
    }

    // 获取所有非“标准”工作表的名称
    QStringList sheetNames;
    for (const QString& sheet : xlsx1.sheetNames()) {
        if (sheet != "标准") sheetNames.append(sheet);
    }
    for (const QString& sheet : xlsx2.sheetNames()) {
        if (sheet != "标准" && !sheetNames.contains(sheet)) {
            sheetNames.append(sheet);
        }
    }

    // 生成新模板文件
    QXlsx::Document newTemplate;

    // 创建格式，设置水平和垂直居中
    QXlsx::Format centerFormat;
    centerFormat.setHorizontalAlignment(QXlsx::Format::AlignHCenter);
    centerFormat.setVerticalAlignment(QXlsx::Format::AlignVCenter);
    centerFormat.setFontBold(true);  // 可选，是否加粗字体

    for (const QString& sheetName : sheetNames) {
        newTemplate.addSheet(sheetName);
        newTemplate.selectSheet(sheetName);

        // **第一行表头（B1:M2）合并单元格，并设置居中格式**
        newTemplate.mergeCells("B1:M2", centerFormat);
        newTemplate.write(1, 2, sheetName, centerFormat);

        // **字段标题（A3:P3）**
        QStringList headers = {
            "序号", "日期", "时间", "温度", "TO1_1", "TO1_2", "TO1_3",
            "TA1-1", "TA1-2", "TA1-3", "TO修1_1", "TO修1_2", "TO修1_3",
            "标准平均值", "拟合筛选", "测试环境"
        };
        for (int col = 0; col < headers.size(); ++col) {
            newTemplate.write(3, col + 1, headers[col]);  // A3 到 P3
        }
    }

    // **生成模板文件路径**
    outputTemplatePath = QFileInfo(file1).path() + "/自动生成模板_" +
                         QDateTime::currentDateTime().toString("yyyyMMddHHmmss") + ".xlsx";

    // **保存模板文件**
    if (!newTemplate.saveAs(outputTemplatePath)) {
        emit errorOccurred("模板文件生成失败");
        return;
    }

     emit operationCompleted(true, outputTemplatePath);
}



// 查找匹配的TXT文件
QString DataExcelProcessor::findMatchingTxtFile(const QString& excelFilePath,
                                                const QString& portNumber,
                                                const QDate& date)
{
    QDir dir(QFileInfo(excelFilePath).absolutePath());
    QStringList txtFiles = dir.entryList(QStringList() << "*.txt", QDir::Files);
    QString dateString = date.toString("yyyyMMdd");

    foreach (const QString &fileName, txtFiles) {
        QFileInfo fileInfo(fileName);
        QString baseName = fileInfo.completeBaseName();
        QStringList parts = baseName.split('_');

        if (parts.size() >= 3 &&
            parts[1].trimmed() == portNumber.trimmed() &&
            parts[2] == dateString) {
            return fileName;
        }
    }

    // 优化：记录详细的搜索条件
    qDebug() << "搜索条件：端口=" << portNumber << "，日期=" << dateString;
    emit errorOccurred(QString("未找到端口 %1 在 %2 的TXT文件")
                           .arg(portNumber, date.toString("yyyy-MM-dd")));
    return QString();
}

// 写入温度数据的通用方法
bool DataExcelProcessor::writeTemperatures(QXlsx::Document& xlsx,
                                           const QMap<QDateTime, QVector<double>>& tempDataMap,
                                           const QMap<QDateTime, int>& dateTimeRowMap)
{
    qDebug() << "开始写入温度数据...";

    try {
        for (auto it = tempDataMap.begin(); it != tempDataMap.end(); ++it) {
            QDateTime dt = it.key();
            int row = dateTimeRowMap.value(dt, -1);
            if (row == -1) continue; // 跳过无效行

            const QVector<double>& temps = it.value();
            if (temps.isEmpty()) continue; // 没有有效温度数据，跳过

            // 写入温度数据
            for (int i = 0; i < temps.size(); ++i) {
                if (temps[i] == 65535) continue;
                xlsx.write(row, 5 + i, temps[i]);
            }
        }

        qDebug() << "温度数据写入完成";

        // 填充U列平均值
        qDebug() << "开始填充 U 列的平均值...";
        int validRows = 0;
        for (auto it = dateTimeRowMap.begin(); it != dateTimeRowMap.end(); ++it) {
            int row = it.value();

            QVariant gVar = xlsx.read(row, 7);
            QVariant hVar = xlsx.read(row, 8);

            if (!gVar.isValid() || !hVar.isValid()) continue;

            bool gOk, hOk;
            double gValue = gVar.toDouble(&gOk);
            double hValue = hVar.toDouble(&hOk);

            if (!gOk || !hOk) continue;

            double average = (gValue + hValue) / 2.0;
            xlsx.write(row, 21, average);
            validRows++;
        }

        qDebug() << "U 列平均值写入完成，共处理" << validRows << "行";
        return true;
    } catch (const std::exception& e) {
        qDebug() << "写入Excel时发生异常：" << e.what();
        m_lastError = "写入Excel时发生异常：" + QString(e.what());
        return false;
    } catch (...) {
        qDebug() << "写入Excel时发生未知异常";
        m_lastError = "写入Excel时发生未知异常";
        return false;
    }
}




int DataExcelProcessor::findRowByDateTime(QXlsx::Document& xlsx, const QDateTime& dt) {
    xlsx.selectSheet(xlsx.sheetNames().first());
    for (int row = 2; row <= xlsx.dimension().lastRow(); ++row) {
        if (xlsx.read(row, 2).toDateTime() == dt) {
            return row;
        }
    }
    return -1;
}

int DataExcelProcessor::copySheetData(QXlsx::Document& srcXlsx, QXlsx::Document& destXlsx,
                                      int srcStartRow, int destStartRow, const QString& envType,
                                      const QMap<QString, double>& standardTempData) {
    int colCount = srcXlsx.dimension().lastColumn();
    int rowCount = srcXlsx.dimension().lastRow();
    int rowOffset = 0;

    qDebug() << "开始复制数据: " << envType;

    for (int row = srcStartRow; row <= rowCount; ++row) {
        QDate date = srcXlsx.read(row, 2).toDate();
        QVariant timeCell = srcXlsx.read(row, 3);
        QTime time;

        if (timeCell.userType() == QMetaType::QTime) {
            time = timeCell.toTime();
        } else if (timeCell.canConvert<double>()) {
            double excelTime = timeCell.toDouble();
            int totalSeconds = static_cast<int>(excelTime * 86400);
            time = QTime(0, 0).addSecs(totalSeconds);
        } else {
            QString timeStr = timeCell.toString().trimmed();
            time = QTime::fromString(timeStr, "HH:mm:ss");
            if (!time.isValid()) {
                time = QTime::fromString(timeStr, "HH:mm");
            }
        }

        // **构建日期时间键**
        QString key = date.toString("yyyy-MM-dd") + " " + time.toString("HH:mm");

        // **读取温度数据**
        QVariant tempVar = srcXlsx.read(row, 4);
        if (time.isNull() || tempVar.isNull()) continue;

        // **A列（序号）**
        destXlsx.write(destStartRow + rowOffset, 1, destStartRow + rowOffset - 3);

        // **B列 日期 & C列 时间**
        destXlsx.write(destStartRow + rowOffset, 2, date.toString("yyyy-MM-dd"));
        destXlsx.write(destStartRow + rowOffset, 3, time.toString("HH:mm"));

        // **复制所有列数据**
        for (int col = 4; col <= colCount; ++col) {
            QVariant value = srcXlsx.read(row, col);
            destXlsx.write(destStartRow + rowOffset, col, value);
        }

        // **P列（16列）：填充箱内/箱外**
        destXlsx.write(destStartRow + rowOffset, 16, envType);

        // **N列（14列）：写入标准温度**
        if (standardTempData.contains(key)) {
            destXlsx.write(destStartRow + rowOffset, 14, standardTempData[key]);
            qDebug() << "[匹配成功] 标准温度填充:" << key << "->" << standardTempData[key];
        } else {
            qDebug() << "[匹配失败] 没找到标准温度:" << key;
        }

        rowOffset++;
    }

    qDebug() << "数据复制完成: " << envType;
    return destStartRow + rowOffset;
}



int DataExcelProcessor::getLastRow(QXlsx::Document& xlsx, int col) {
    int row = 1;
    while (!xlsx.read(row, col).toString().isEmpty()) {
        ++row;
    }
    return row - 1;
}

void DataExcelProcessor::sortSheetData(QXlsx::Document& xlsx, int startRow, int endRow, int col) {
    struct RowData {
        int rowNum;
        double value;
        QVector<QVariant> rowValues;
    };

    QVector<RowData> data;

    for (int row = startRow; row <= endRow; ++row) {
        QVariant tempVar = xlsx.read(row, col);
        if (tempVar.isNull()) continue;  // 仅跳过空值

        double temp = tempVar.toDouble();  // 允许 0 作为有效温度

        QVector<QVariant> rowValues;
        for (int col = 1; col <= xlsx.dimension().lastColumn(); ++col) {
            rowValues.append(xlsx.read(row, col));
        }

        data.append({row, temp, rowValues});
    }

    // 按D列降序排序
    std::sort(data.begin(), data.end(), [](const RowData& a, const RowData& b) {
        return a.value > b.value;  // 温度降序（保留 0）
    });

    // 重新写入排序后数据
    for (int i = 0; i < data.size(); ++i) {
        for (int col = 1; col <= data[i].rowValues.size(); ++col) {
            xlsx.write(startRow + i, col, data[i].rowValues[col - 1]);
        }

        // A列（序号）：按新的行顺序填充
        xlsx.write(startRow + i, 1, i + 1);
    }
}

void DataExcelProcessor::loadStandardTemperature(QXlsx::Document& srcXlsx,
                                                 QMap<QString, double>& standardTempData) {
    if (!srcXlsx.sheetNames().contains("标准")) {
        qDebug() << "未找到标准工作表，跳过";
        return;
    }
    srcXlsx.selectSheet("标准");

    qDebug() << "开始读取标准工作表...";

    int emptyCount = 0; // 记录连续无效数据行的计数

    for (int row = 3; ; ++row) {
        QVariant dateVar = srcXlsx.read(row, 2);
        QVariant timeVar = srcXlsx.read(row, 3);
        QVariant tempVar = srcXlsx.read(row, 21); // U列

        QDate date = dateVar.toDate();
        QTime time = timeVar.toTime(); // 直接转换时间

        if (!time.isValid()) {
            time = QTime::fromString(timeVar.toString(), "HH:mm:ss");
        }
        if (!time.isValid()) {
            time = QTime::fromString(timeVar.toString(), "HH:mm");
        }

        // **调试信息**
        qDebug() << "Row:" << row;
        qDebug() << "Date:" << dateVar << "-> QDate:" << date;
        qDebug() << "Time:" << timeVar << "-> QTime:" << time << " (" << timeVar.typeName() << ")";
        qDebug() << "Temp:" << tempVar;

        // **检测是否是空行或无效数据**
        if (!date.isValid() || !time.isValid() || tempVar.isNull()) {
            emptyCount++;
            qDebug() << "读取到空行或无效数据，连续无效行：" << emptyCount;
            if (emptyCount >= 5) {
                qDebug() << "连续超过 5 行无效数据，结束标准温度读取";
                break; // 终止读取
            }
            continue;
        }

        // **重置无效数据计数**
        emptyCount = 0;

        // **跳过 Excel 公式**
        QString tempStr = tempVar.toString().trimmed();
        if (tempStr.startsWith("=")) {
            qDebug() << "跳过 Excel 公式：" << tempStr;
            continue;
        }

        bool ok;
        double temperature = tempStr.toDouble(&ok);
        if (!ok) {
            qDebug() << "警告：无法解析温度值" << tempVar << "，位于行" << row;
            continue;
        }

        QString key = date.toString("yyyy-MM-dd") + " " + time.toString("HH:mm");
        standardTempData[key] = temperature;

        qDebug() << "读取标准温度:" << key << "->" << temperature;
    }

    qDebug() << "标准温度读取完成，共" << standardTempData.size() << "条数据";
}

// DataExcelProcessor.cpp 单头合并实现
QString DataExcelProcessor::mergeSingleHeadFiles(const QString& inFile, const QString& outFile)
{
    try {
        QXlsx::Document inDoc(inFile);
        QXlsx::Document outDoc(outFile);

        if(inDoc.sheetNames().isEmpty() || outDoc.sheetNames().isEmpty()){
            emit errorOccurred("文件缺少工作表");
            return "";
        }

        // 验证第一个工作表是标准
        if(inDoc.sheetNames().first() != "标准" || outDoc.sheetNames().first() != "标准"){
            emit errorOccurred("第一个工作表必须命名为'标准'");
            return "";
        }

        // 调试输出工作表结构
        qDebug() << "箱内文件工作表:" << inDoc.sheetNames();
        qDebug() << "箱外文件工作表:" << outDoc.sheetNames();

        // 合并设备列表（自动跳过标准工作表）
        auto devices = detectDevices(inDoc);
        auto outDevices = detectDevices(outDoc);
        qDebug() << "箱内检测到设备:" << devices;
        qDebug() << "箱外检测到设备:" << outDevices;
        QVector<QPair<int, QString>> allDevices;
        std::set_union(devices.begin(), devices.end(),
                       outDevices.begin(), outDevices.end(),
                       std::back_inserter(allDevices));

        // 加载标准温度数据
        QMap<QString, double> stdTempMap;
        loadStandardTemperature(inDoc, stdTempMap);
        loadStandardTemperature(outDoc, stdTempMap);

        QXlsx::Document templateDoc;
        if(templateDoc.sheetNames().isEmpty()){
            templateDoc.addSheet("Default");
        }

        // 删除临时工作表
        templateDoc.deleteSheet("Template");

        // 为每个设备创建工作表
        foreach(const auto& device, allDevices) {
            QString sheetName = device.second;
            templateDoc.addSheet(sheetName);
            templateDoc.selectSheet(sheetName);

            // 写入统一表头
            QStringList headers = {"序号", "日期", "时间", "环境",
                                   "测试点温度", "目标温度", "腔体温度", "标准温度"};
            for(int col = 1; col <= headers.size(); ++col) {
                templateDoc.write(1, col, headers[col-1]);
            }

            // 处理数据
            processDeviceData(inDoc, templateDoc, "箱内", device, stdTempMap);
            processDeviceData(outDoc, templateDoc, "箱外", device, stdTempMap);

            // 排序数据（按目标温度降序）
            int lastRow = getLastRow(templateDoc, 1);
            if(lastRow > 1) {
                sortSheetData(templateDoc, 2, lastRow, 5);
            }
        }

        if (templateDoc.sheetNames().contains("Default")) {
            templateDoc.deleteSheet("Default");
        }

        // 保存结果并返回路径
        QString outputPath = QFileInfo(inFile).path() + "/单头箱内箱外合并结果_"
                             + QDateTime::currentDateTime().toString("yyyyMMddHHmm") + ".xlsx";
        if (!templateDoc.saveAs(outputPath)) {
            emit errorOccurred("文件保存失败");
            return "";
        }
        emit operationCompleted(true, outputPath);
        return outputPath;
    } catch (...) {
        emit errorOccurred("合并过程中发生未知错误");
        return "";
    }
}


// 检测设备列
// 修改后的detectDevices函数应包含更健壮的解析逻辑
QVector<QPair<int, QString>> DataExcelProcessor::detectDevices(QXlsx::Document& doc)
{
    QVector<QPair<int, QString>> devices;

    // 跳过"标准"工作表
    QStringList sheets = doc.sheetNames();
    sheets.removeAll("标准");

    foreach(const QString& sheetName, sheets) {
        doc.selectSheet(sheetName);
        qDebug() << "正在检测工作表:" << sheetName;

        // 原设备检测逻辑
        const int startCol = 5;
        const int step = 4;
        for(int col = startCol; col <= 30; col += step) {
            std::shared_ptr<QXlsx::Cell> cell = doc.cellAt(2, col); // 第2行是设备信息
            if (!cell) break;

            QString info = cell->value().toString().trimmed();
            qDebug() << "检测设备列" << col << "信息:" << info;

            QString deviceNum;
            if (info.contains("-COM", Qt::CaseInsensitive)) {
                // 处理格式：43-COM1，提取前面的设备编号
                deviceNum = info.section("-COM", 0, 0).trimmed();
            } else {
                qDebug() << "非设备列，跳过";
                continue;
            }

            QString port = "N" + deviceNum; // 修改端口格式为"N43"
            qDebug() << "解析出端口:" << port;
            devices.append(qMakePair(col, port));
        }
    }
    return devices;
}



// 处理单个设备数据
void DataExcelProcessor::processDeviceData(QXlsx::Document& srcXlsx,
                                           QXlsx::Document& destXlsx,
                                           const QString& envType,
                                           const QPair<int, QString>& device,
                                           const QMap<QString, double>& stdTemp)
{
    QStringList sheetNames = srcXlsx.sheetNames();
    if (sheetNames.size() <= 1) { // 只有 "标准" 一个表
        emit errorOccurred("没有足够的工作表，无法读取数据");
        return;
    }

    // **解析设备号**（如 "N34" -> "34"）
    QRegularExpression re("(\\d+)$");  // 匹配字符串末尾的数字
    QRegularExpressionMatch match = re.match(device.second);
    if (!match.hasMatch()) {
        emit errorOccurred(QString("无法解析设备号: %1").arg(device.second));
        return;
    }
    int deviceNumber = match.captured(1).toInt(); // 设备号，如 34

    // **匹配正确的工作表**
    QString selectedSheet;
    for (const QString& sheet : sheetNames) {
        QRegularExpression rangeRe("-(\\d+)-(\\d+)"); // 匹配形如 "-34-37" 的范围
        QRegularExpressionMatch rangeMatch = rangeRe.match(sheet);

        if (rangeMatch.hasMatch()) {
            int rangeStart = rangeMatch.captured(1).toInt(); // 34
            int rangeEnd   = rangeMatch.captured(2).toInt(); // 37

            if (deviceNumber >= rangeStart && deviceNumber <= rangeEnd) {
                selectedSheet = sheet;
                break;
            }
        }
    }

    if (selectedSheet.isEmpty()) {
        emit errorOccurred(QString("未找到匹配的工作表: 设备号 %1").arg(deviceNumber));
        return;
    }

    // **选择匹配到的工作表**
    srcXlsx.selectSheet(selectedSheet);
    qDebug() << "Processing Device:" << device.second << "in Sheet:" << selectedSheet;

    const int srcCol = device.first;

    // 获取目标工作表最后行号
    int destRow = getLastRow(destXlsx, 1) + 1;
    destRow = qMax(destRow, 2); // 确保从第2行开始

    qDebug() << "目标工作表:" << device.second << "，初始目标行:" << destRow;

    // **源数据起始行**
    int srcRow = 4;
    const int maxEmptyRows = 10;
    int emptyCount = 0;

    while(emptyCount < maxEmptyRows) {
        // 读取日期时间
        QDate date = srcXlsx.read(srcRow, 2).toDate();
        QTime time = srcXlsx.read(srcRow, 3).toTime();

        // 读取测试点温度（D列）
        QVariant testPointTemp = srcXlsx.read(srcRow, 4); // D列是第4列

        if(date.isNull() || time.isNull()) {
            emptyCount++;
            srcRow++;
            continue;
        }

        // 读取设备数据
        QVariant targetTemp = srcXlsx.read(srcRow, srcCol);
        QVariant chamberTemp = srcXlsx.read(srcRow, srcCol + 1);

        qDebug() << "处理行:" << srcRow
                 << "，目标行:" << destRow
                 << "，目标温度:" << targetTemp
                 << "，腔体温度:" << chamberTemp;

        // **写入目标工作表**
        if(targetTemp.isValid() && chamberTemp.isValid()) {
            destXlsx.write(destRow, 1, destRow-1); // 序号
            destXlsx.write(destRow, 2, date);
            destXlsx.write(destRow, 3, time);
            destXlsx.write(destRow, 4, envType);
            destXlsx.write(destRow, 5, testPointTemp); // 新增测试点温度列
            destXlsx.write(destRow, 6, targetTemp);
            destXlsx.write(destRow, 7, chamberTemp);

            // **写入标准温度**
            QString timeKey = QString("%1 %2")
                                  .arg(date.toString("yyyy-MM-dd"))
                                  .arg(time.toString("HH:mm"));
            destXlsx.write(destRow, 8, stdTemp.value(timeKey, 65535));

            destRow++;
            emptyCount = 0;
        } else {
            emptyCount++;
        }
        srcRow++;
    }
}




