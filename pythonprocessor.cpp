#include "pythonprocessor.h"
#include <QFileDialog>
#include <QDir>
#include <QDebug>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QPrinter>
#include <QPainter>
#include <QDateTime> // 添加以获取当前时间
#include <QDesktopServices>
#include <QTimer>

PythonProcessor::PythonProcessor(QObject *parent)
    : QObject(parent), m_process(new QProcess(this))
{
    // 配置进程信号连接
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &PythonProcessor::handleProcessOutput);
    connect(m_process, &QProcess::errorOccurred,
            this, &PythonProcessor::handleProcessError);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &PythonProcessor::handleProcessFinished);
}

void PythonProcessor::startProcessing(const QString& inputFilePath, const QString& nid)
{
    // 重置进程状态
    resetProcess();

    QFileInfo inputFile(inputFilePath);
    if (!inputFile.exists()) {
        emit errorOccurred("输入文件不存在");
        return;
    }

    // 自动生成输出路径
    m_outputPath = generateOutputPath(inputFile, true);

    // 获取NID（优先使用传入参数）
    QString finalNid = nid.isEmpty() ? extractNid(inputFile.fileName()) : nid;

    // 配置Python参数
    QStringList args;
    args << finalNid
         << "--input=" + QDir::toNativeSeparators(inputFilePath)
         << "--output=" + QDir::toNativeSeparators(m_outputPath);

    // 启动进程（指定Python绝对路径）
    QString pythonExe = "python";
    QString scriptPath = QCoreApplication::applicationDirPath() + "/DNH.py";

    // ========== 新增调试信息：验证Python和脚本路径 ==========
    qDebug() << "\n===== 启动单头处理 =====";
    qDebug() << "Python解释器路径：" << pythonExe;
    qDebug() << "Python是否存在：" << QFile::exists(pythonExe);
    qDebug() << "脚本路径：" << scriptPath;
    qDebug() << "脚本是否存在：" << QFile::exists(scriptPath);
    qDebug() << "输入文件路径：" << inputFilePath;
    qDebug() << "输出文件路径：" << m_outputPath;
    qDebug() << "启动命令：" << pythonExe << " " << scriptPath << " " << args.join(" ");

    emit progressChanged("正在启动Python进程...");
    m_isProcessing = true;
    m_process->start(pythonExe, QStringList() << scriptPath << args);

    // 确保进程成功启动（新增超时细节）
    if (!m_process->waitForStarted(5000)) {
        // ========== 新增调试：无法启动时的具体原因 ==========
        qDebug() << "Python进程启动失败！原因：" << m_process->errorString();
        qDebug() << "可能原因：1. Python路径错误；2. 脚本路径错误；3. 权限不足；4. 架构不匹配（32/64位）";
        emit errorOccurred("启动Python进程失败，请检查Python环境或脚本路径！错误详情：" + m_process->errorString());
        return;
    }

    // 启动超时定时器
    if (m_processingTimeout > 0) {
        QTimer::singleShot(m_processingTimeout, this, [this]() {
            if (m_process->state() == QProcess::Running) {
                qDebug() << "Python处理超时，强制终止";
                m_process->terminate();
                emit errorOccurred("处理超时，已强制终止");
            }
        });
    }
}

QString PythonProcessor::generateOutputPath(const QFileInfo& inputFile, bool isMultiHead) const
{
    QString suffix = isMultiHead ? "结果.xlsx" : "_拟合结果.xlsx";
    return inputFile.path() + "/" + inputFile.baseName() + suffix;
}

QString PythonProcessor::extractNid(const QString& fileName) const
{
    // 示例提取逻辑：文件名前3位为NID
    return fileName.left(3);
}

void PythonProcessor::handleProcessOutput()
{
    QString rawOutput = QString::fromLocal8Bit(m_process->readAllStandardOutput());
    QStringList outputLines = rawOutput.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts);

    static const QRegularExpression re(R"(\[PROGRESS\s+(\d+)%\](.*))");

    foreach (const QString& line, outputLines) {
        QString cleanLine = line.trimmed();  // 直接使用 trimmed() 清理空白字符

        QRegularExpressionMatch match = re.match(cleanLine);
        if (match.hasMatch()) {
            bool ok;
            int progress = match.captured(1).toInt(&ok);
            if (ok) {
                QString message = match.captured(2).trimmed();
                qDebug() << "[进度更新]" << progress << "%" << message;
                emit progressUpdated(progress, message);
            }
        } else {
            qDebug() << "[普通输出]" << cleanLine;
            emit progressChanged(cleanLine);
        }
    }
}

void PythonProcessor::handleProcessError(QProcess::ProcessError error) {
    m_isProcessing = false;
    QString errorMsg;
    switch (error) {
    case QProcess::FailedToStart:
        errorMsg = "Python进程启动失败，请检查Python环境";
        break;
    case QProcess::Crashed:
        errorMsg = "Python进程崩溃，可能是脚本错误或内存不足";
        break;
    case QProcess::Timedout:
        errorMsg = "Python进程执行超时";
        break;
    default:
        errorMsg = "Python进程发生错误: " + QString::number(error);
    }
    qDebug() << "Python错误:" << errorMsg;
    emit errorOccurred(errorMsg);
}

void PythonProcessor::handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    m_isProcessing = false;
    qDebug() << "Python处理完成，exitCode:" << exitCode << "exitStatus:" << exitStatus;

    QString errorOutput = QString::fromLocal8Bit(m_process->readAllStandardError());
    QString standardOutput = QString::fromLocal8Bit(m_process->readAllStandardOutput());
    qDebug() << "Standard Output:\n" << standardOutput;
    qDebug() << "Error Output:\n" << errorOutput;

    // 解析输出路径
    QRegularExpression pathRegex(R"(\[OUTPUT_PATH\]\s*(.+\.xlsx))");
    QRegularExpressionMatch match = pathRegex.match(standardOutput);
    QString resultPath = match.hasMatch() ? match.captured(1).trimmed() : m_outputPath;

    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        QList<QList<QList<double>>> data = extractDataFromExcel(resultPath); // 三维列表
        if (!data.isEmpty()) {
            QFileInfo excelInfo(resultPath);
            QString deviceNumber = excelInfo.baseName().split('-').first().replace("结果", "");
            qDebug() << "设备名：" << deviceNumber;

            QString calibrationTime = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
            generatePDFCertificate(data, resultPath, deviceNumber, calibrationTime); // 参数类型匹配

            // 处理系数数据（从三维列表data[0]获取）
            if (data.size() >= 1 && !data[0].isEmpty()) {
                QList<QList<double>> coefficients = data[0]; // 系数是二维列表
                if (coefficients.size() == 3) { // 确保有3组系数
                    QString command = generateCalibrationCommand(coefficients, deviceNumber);
                    saveCommandToFile(command, deviceNumber);
                } else {
                    qDebug() << "系数数据格式错误，需要3组，实际" << coefficients.size() << "组";
                }
            } else {
                qDebug() << "未提取到系数数据，无法生成命令文件";
            }
        } else {
            emit errorOccurred("无法从结果Excel文件中提取数据！");
        }
        emit processingFinished(true, resultPath);
    } else {
        // 错误处理逻辑（保持不变）
        QString errorMessage = QString("Python 进程异常退出（代码 %1）\n错误信息:\n%2")
                                   .arg(exitCode)
                                   .arg(errorOutput.isEmpty() ? "无错误输出" : errorOutput);
        emit errorOccurred(errorMessage);
        emit processingFinished(false, "");
    }

    m_isProcessing = false;
    emit processingFinished(true, resultPath);
}

void PythonProcessor::startMultiProcessing(const QString& inputFilePath, const QString& nid)
{
    QFileInfo inputFile(inputFilePath);
    m_outputPath = generateOutputPath(inputFile, true); // 生成多头专用路径
    if (!inputFile.exists()) {
        emit errorOccurred("输入文件不存在");
        return;
    }

    // 提取目录路径
    QString filesPath = inputFile.absolutePath();
    filesPath = QDir::toNativeSeparators(filesPath) + QDir::separator();

    // NID格式验证
    if (!nid.startsWith("多") || nid.length() < 2) {
        emit errorOccurred("无效的NID格式，示例：多8");
        return;
    }

    // 脚本路径验证
    QString scriptPath = QCoreApplication::applicationDirPath() + "/run.py";
    // ========== 新增调试：验证多头处理的脚本和Python路径 ==========
    QString pythonExe = "python";
    qDebug() << "\n===== 启动多头处理 =====";
    qDebug() << "Python解释器路径：" << pythonExe;
    qDebug() << "Python是否存在：" << QFile::exists(pythonExe);
    qDebug() << "脚本路径（run.py）：" << scriptPath;
    qDebug() << "脚本是否存在：" << QFile::exists(scriptPath);
    qDebug() << "启动命令：" << pythonExe << " " << scriptPath << " " << filesPath << " " << nid;

    // 构建参数
    QStringList args;
    args << filesPath << nid;

    // 启动进程
    m_process->setWorkingDirectory(QCoreApplication::applicationDirPath());
    m_process->start(pythonExe, QStringList() << scriptPath << args); // 这里也改用绝对路径，之前可能漏改

    // 超时处理（新增调试）
    if (!m_process->waitForStarted(5000)) {
        qDebug() << "多头处理Python启动失败！原因：" << m_process->errorString();
        emit errorOccurred("启动Python进程超时（多头处理）：" + m_process->errorString());
    }

    // 连接自定义槽函数处理多头数据结束后的逻辑
    disconnect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, nullptr);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
                handleMultiProcessFinished(exitCode, exitStatus);
            });
}

void PythonProcessor::handleMultiProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    QString errorOutput = QString::fromLocal8Bit(m_process->readAllStandardError());
    QString standardOutput = QString::fromLocal8Bit(m_process->readAllStandardOutput());

    qDebug() << "Standard Output:\n" << standardOutput;
    qDebug() << "Error Output:\n" << errorOutput;

    // 解析输出路径
    QRegularExpression pathRegex(R"(\[OUTPUT_PATH\]\s*(.+\.xlsx))");
    QRegularExpressionMatch match = pathRegex.match(standardOutput);

    QString resultPath;
    if (match.hasMatch()) {
        resultPath = match.captured(1).trimmed();
        qDebug() << "Parsed Result Path:" << resultPath;
    } else {
        qDebug() << "Using default output path:" << m_outputPath;
        resultPath = m_outputPath;
    }

    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        // 提取多头设备的 Excel 文件路径和设备编号
        QPair<QString, QStringList> result = extractMultiExcelPaths(resultPath);
        QString deviceNumber = result.first;
        this->excelPaths = result.second;

        if (!this->excelPaths.isEmpty()) {
            // 从多个 Excel 文件中提取数据
            QList<QList<QList<double>>> multiData = extractMultiDataFromExcel(excelPaths);

            QDateTime latestDate = extractLatestDateFromMergedFile();
            QString calibrationDate = latestDate.toString("yyyy-MM-dd");

            if (!multiData.isEmpty()) {
                // 合并数据并生成 PDF 校准证书
                generateMultiPDFCertificate(multiData, excelPaths.first(), deviceNumber, calibrationDate);

                // 新增：检查 AR 系数是否存在
                if (m_arCoefficients.size() == 3) { // 确保三个文件的系数都被提取
                    generateEnergyConfigCommand(deviceNumber);
                } else {
                    qDebug() << "AR系数不完整，未生成命令文件";
                }

            } else {
                emit errorOccurred("无法从多头设备的 Excel 文件中提取数据！");
            }
        } else {
            emit errorOccurred("未找到多头设备的 Excel 文件！");
        }

        emit processingFinished(true, resultPath);
    } else {
        qDebug() << "Python 进程退出异常！";
        qDebug() << "Exit Code:" << exitCode;
        qDebug() << "Exit Status:" << (exitStatus == QProcess::CrashExit ? "Crashed" : "Normal");
        qDebug() << "Standard Output:\n" << standardOutput;
        qDebug() << "Error Output:\n" << errorOutput;

        // 生成错误提示信息
        QString errorMessage = QString("Python 进程异常退出（代码 %1）\n"
                                       "错误信息:\n%2")
                                   .arg(exitCode)
                                   .arg(errorOutput.isEmpty() ? "无错误输出" : errorOutput);
        emit errorOccurred(errorMessage);
        emit processingFinished(false, "");
    }
}

QList<QList<QList<double>>> PythonProcessor::extractDataFromExcel(const QString& filePath)
{
    QList<QList<QList<double>>> data; // 三维列表：[系数数据, 温度数据]
    QXlsx::Document xlsx(filePath);
    if (!xlsx.load()) {
        emit errorOccurred("Excel文件加载失败");
        return data;
    }

    QList<QString> sheetNames = xlsx.sheetNames();
    if (sheetNames.size() < 2) {
        emit errorOccurred("结果Excel文件必须包含至少两个工作表");
        return data;
    }

    // ========== 系数提取（二维列表：每组3个系数） ==========
    QList<QList<double>> coefficients; // 二维列表：[[a1,b1,c1], [a2,b2,c2], [a3,b3,c3]]
    QString firstSheetName = sheetNames[0];
    QXlsx::Worksheet* firstSheet = dynamic_cast<QXlsx::Worksheet*>(xlsx.sheet(firstSheetName));
    if (firstSheet) {
        int lastRow = firstSheet->dimension().rowCount();
        if (lastRow >= 3) {
            QRegularExpression coefRegex(R"(\s*=\s*([\d.-]+)\*\S+\s*\+\s*([\d.-]+)\*\S+\s*\+\s*([\d.-]+))");
            for (int row = lastRow - 2; row <= lastRow; ++row) {
                auto cellO = firstSheet->cellAt(row, 15); // O列（第15列）
                if (cellO) {
                    QString formula = cellO->value().toString();
                    auto match = coefRegex.match(formula);
                    if (match.hasMatch()) {
                        QList<double> coefRow; // 单组3个系数
                        for (int i = 1; i <= 3; ++i) {
                            coefRow.append(match.captured(i).toDouble());
                        }
                        coefficients.append(coefRow); // 存入二维列表
                        qDebug() << "成功提取第" << coefficients.size() << "组系数:" << coefRow;
                    }
                }
            }
        }
    }

    // ========== 温度数据提取（二维列表：每个温度点4个值） ==========
    const QList<double> targetTemps = {-20, 0, 30, 60};
    QMap<double, QList<QList<double>>> tempDataMap; // 温度 -> [E列表, L列表, M列表]
    for (double temp : targetTemps) {
        tempDataMap[temp] = {QList<double>(), QList<double>(), QList<double>()};
    }

    int rowCount = firstSheet->dimension().rowCount();
    for (int row = 2; row <= rowCount; ++row) {
        auto cellB = firstSheet->cellAt(row, 2); // B列（第2列）
        if (!cellB) continue;

        double temp = cellB->value().toString().remove("℃").toDouble();
        if (!targetTemps.contains(temp)) continue;

        auto cellE = firstSheet->cellAt(row, 5); // E列（第5列）
        auto cellL = firstSheet->cellAt(row, 12); // L列（第12列）
        auto cellM = firstSheet->cellAt(row, 13); // M列（第13列）

        if (cellE) tempDataMap[temp][0].append(cellE->value().toDouble());
        if (cellL) tempDataMap[temp][1].append(cellL->value().toDouble());
        if (cellM) tempDataMap[temp][2].append(cellM->value().toDouble());
    }

    // 计算温度平均值（二维列表：[温度, E平均, L平均, M平均]）
    QList<QList<double>> averagedData;
    for (double temp : targetTemps) {
        auto& values = tempDataMap[temp];
        if (values[0].isEmpty() || values[1].isEmpty() || values[2].isEmpty()) {
            qDebug() << "温度" << temp << "℃数据不完整，跳过";
            continue;
        }

        double avgE = std::accumulate(values[0].begin(), values[0].end(), 0.0) / values[0].size();
        double avgL = std::accumulate(values[1].begin(), values[1].end(), 0.0) / values[1].size();
        double avgM = std::accumulate(values[2].begin(), values[2].end(), 0.0) / values[2].size();

        averagedData.append({temp, avgE, avgL, avgM}); // 单组4个值
    }

    // 最终数据：[系数（二维）, 温度（二维）]
    data.append(coefficients);
    data.append(averagedData);
    return data;
}

void PythonProcessor::generatePDFCertificate(const QList<QList<QList<double>>>& data, const QString& excelPath, const QString& deviceNumber, const QString& calibrationTime)
{
    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::PdfFormat);

    QString pdfPath = excelPath;
    pdfPath.replace("结果.xlsx",  "-测试报告.pdf");
    printer.setOutputFileName(pdfPath);
    printer.setPageSize(QPageSize(QPageSize::A4));
    printer.setPageOrientation(QPageLayout::Portrait);

    QPainter painter(&printer);
    if (!painter.isActive()) {
        emit errorOccurred("无法生成 PDF: QPainter 无法启动");
        return;
    }

    // 从合并文件中提取最新日期
    QDateTime latestDate = extractLatestDateFromMergedFile();
    QString reportDate = latestDate.toString("yyyy-MM-dd");

    // ========== 标题与基础信息 ==========
    QFont titleFont;
    titleFont.setPointSize(30);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.drawText(QRect(0, 100, printer.width(), 600), Qt::AlignCenter, "测试报告");

    QFont infoFont;
    infoFont.setPointSize(15);
    painter.setFont(infoFont);
    int infoY = 800;
    QString processedDeviceNumber = deviceNumber.mid(1);
    QString formattedDeviceNumber = QString("IR27E800%1").arg(processedDeviceNumber);
    painter.drawText(QRect(0, infoY, printer.width(), 400), Qt::AlignCenter, QString("设备编号: %1").arg(formattedDeviceNumber));
    painter.drawText(QRect(0, infoY + 400, printer.width(), 400), Qt::AlignCenter, QString("测试日期: %1").arg(reportDate));

    // ========== 表格绘制（按表头字符数分配列宽） ==========
    QFont dataFont;
    dataFont.setPointSize(15);
    painter.setFont(dataFont);

    int startY = 2100;
    int rowHeight = printer.height() / 25;

    // 表头内容及字符数计算
    QStringList headers = {"序号", "测试点(℃)", "标准器温度(℃)", "红外温度计示值(℃)", "示值误差(℃)"};
    QList<int> charCounts; // 计算每个表头的字符数
    for (const QString& header : headers) {
        charCounts.append(header.length());
    }

    // 总字符数
    int totalChars = 0;
    for (int count : charCounts) {
        totalChars += count;
    }

    // 计算宽度比例
    QList<double> widthRatios;
    for (int count : charCounts) {
        // 实际比例 = 字符数比例
        double ratio = (double)count / totalChars ;
        widthRatios.append(ratio);
    }

    // 计算各列宽度 (预留100px边距)
    double totalWidth = printer.width() - 200;
    QList<int> colWidths;
    for (double ratio : widthRatios) {
        colWidths.append(totalWidth * ratio);
    }

    // 确保总宽度一致 (处理浮点误差)
    int actualTotalWidth = 0;
    for (int width : colWidths) {
        actualTotalWidth += width;
    }
    int diff = totalWidth - actualTotalWidth;
    if (diff > 0) {
        colWidths[colWidths.size() - 1] += diff; // 误差累加到最后一列
    }

    // 计算表格起始X坐标，确保居中
    int startX = (printer.width() - actualTotalWidth) / 2;

    // 绘制表头
    QFont boldFont;
    boldFont.setBold(true);
    boldFont.setPointSize(15);
    painter.setFont(boldFont);

    for (int i = 0; i < headers.size(); ++i) {
        int x = startX;
        if (i > 0) {
            for (int j = 0; j < i; ++j) {
                x += colWidths[j]; // 累加前几列宽度
            }
        }

        painter.drawRect(x, startY, colWidths[i], rowHeight);
        QTextOption textOption(Qt::AlignCenter);
        painter.drawText(QRect(x, startY, colWidths[i], rowHeight), headers[i], textOption);
    }

    // 绘制数据行
    painter.setFont(dataFont);
    QList<QList<double>> averagedData = data[1];
    for (int row = 0; row < averagedData.size(); ++row) {
        startY += rowHeight;

        QList<double> rowData = averagedData[row];
        double temp = rowData[0];
        double avgE = rowData[1];
        double avgL = rowData[2];

        // 序号列
        painter.drawRect(startX, startY, colWidths[0], rowHeight);
        painter.drawText(QRect(startX, startY, colWidths[0], rowHeight),
                         QString::number(row+1), QTextOption(Qt::AlignCenter));

        // 其他数据列（前四列）
        QStringList cells = {
            QString::number(temp, 'f', 2),
            QString::number(avgE, 'f', 2),
            QString::number(avgL, 'f', 2),
        };

        int x = startX + colWidths[0]; // 跳过序号列
        for (int i = 0; i < cells.size(); ++i) {
            painter.drawRect(x, startY, colWidths[i+1], rowHeight);
            painter.drawText(QRect(x, startY, colWidths[i+1], rowHeight),
                             cells[i], QTextOption(Qt::AlignCenter));
            x += colWidths[i+1]; // 移动到下一列
        }

        // 计算并绘制误差列（第五列）
        // 提取原始数值
        double rawStandard = avgE;   // 标准器温度（第三列原始值）
        double rawMeasured = avgL;   // 测量温度（第四列原始值）

        // 格式化为两位小数的字符串（模拟显示值）
        QString stdStr = QString::number(rawStandard, 'f', 2);
        QString measStr = QString::number(rawMeasured, 'f', 2);

        // 转换回double类型（此时值为四舍五入后的显示值）
        double displayedStandard = stdStr.toDouble();
        double displayedMeasured = measStr.toDouble();

        // 计算误差（基于显示值）
        double calculatedError = displayedMeasured - displayedStandard;

        // 绘制误差列
        painter.drawRect(x, startY, colWidths[4], rowHeight);
        painter.drawText(QRect(x, startY, colWidths[4], rowHeight),
                         QString::number(calculatedError, 'f', 2),
                         QTextOption(Qt::AlignCenter));
    }

    if (startY + rowHeight > printer.height()) {
        qDebug() << "警告：表格内容超出页面高度";
    }

    // ========== 在PDF底部添加测试员和审核员信息 ==========
    QFont footerFont;
    footerFont.setPointSize(15);
    painter.setFont(footerFont);

    // 计算底部Y坐标（页面底部留出5000px边距）
    int footerY = printer.height() - 5000;

    // 绘制测试员信息（左对齐）
    painter.drawText(QRect(200, footerY, printer.width()/2 - 150, 300),
                     QString("测试员：%1").arg(m_testerName.isEmpty() ? "未填写" : m_testerName),
                     QTextOption(Qt::AlignLeft | Qt::AlignVCenter));

    // 绘制审核员信息（右对齐）
    painter.drawText(QRect(printer.width()/2 + 100, footerY, printer.width()/2 - 150, 300),
                     QString("复核员：%1").arg(m_reviewerName.isEmpty() ? "未填写" : m_reviewerName),
                     QTextOption(Qt::AlignRight | Qt::AlignVCenter));

    painter.end();
    emit progressChanged("测试报告PDF已生成：" + pdfPath);
}

QPair<QString, QStringList> PythonProcessor::extractMultiExcelPaths(const QString& baseResultPath)
{
    QStringList excelPaths;
    QString deviceNumber;

    QFileInfo baseInfo(baseResultPath);
    QString baseName = baseInfo.baseName();

    // 去掉“结果”两个字
    baseName = baseName.replace("结果", "");
    qDebug() << "Modified Base Name:" << baseName;

    // 提取设备编号（假设设备编号是“多”后面跟着的数字）
    QRegularExpression deviceNumberRegex(R"(多\d+)"); // 直接匹配“多”后面跟着的数字
    QRegularExpressionMatch match = deviceNumberRegex.match(baseName);
    if (match.hasMatch()) {
        deviceNumber = match.captured(0); // 捕获完整的设备编号（如“多8”）
        qDebug() << "Extracted Device Number:" << deviceNumber;
    } else {
        qDebug() << "Device number not found in base name:" << baseName;
    }

    // 提取多头设备的三个 Excel 文件路径
    for (int i = 1; i <= 3; ++i) {
        QString filePath = baseInfo.path() + "/" + deviceNumber + "-" + QString::number(i) + "结果.xlsx";
        qDebug() << "Checking file path:" << filePath;

        if (QFile::exists(filePath)) {
            excelPaths.append(filePath);
            qDebug() << "Found file:" << filePath;
        } else {
            qDebug() << "File not found:" << filePath;
        }
    }

    qDebug() << "Extracted Excel Paths:" << excelPaths;
    return QPair<QString, QStringList>(deviceNumber, excelPaths);
}

QList<QList<QList<double>>> PythonProcessor::extractMultiDataFromExcel(const QStringList& excelPaths)
{
    QList<QList<QList<double>>> multiData;
    QList<QList<double>> combinedData;

    // 固定校准温度点
    const QList<double> targetTemperatures = {-25.0, -10.0, 0.0, 30.0, 50.0, 70.0};

    if (excelPaths.size() < 3) {
        emit errorOccurred(QString("需要至少三个 Excel 文件来生成多头设备校准证书！实际找到: %1").arg(excelPaths.size()));
        return multiData;
    }

    qDebug() << "Processing Excel files:" << excelPaths;

    // 存储所有文件的温度数据，用于后续计算平均值
    QList<QList<QList<double>>> allFilesData;

    for (const QString& filePath : excelPaths) {
        QXlsx::Document xlsx(filePath);
        if (!xlsx.load()) {
            emit errorOccurred(QString("无法加载 Excel 文件: %1").arg(filePath));
            return multiData;
        }

        qDebug() << "Processing file:" << filePath;

        // 获取第一个工作表
        QXlsx::Worksheet* sheet = dynamic_cast<QXlsx::Worksheet*>(xlsx.sheet(xlsx.sheetNames().first()));
        if (!sheet) {
            emit errorOccurred(QString("无法获取工作表: %1").arg(filePath));
            return multiData;
        }

        // 存储当前文件的四个温度点数据
        QList<QList<double>> fileTemperatures;

        const int MAX_SCAN_ROWS = 100; // 假设数据最多在前1000行，可根据实际调整

        for (double targetTemp : targetTemperatures) {
            QList<std::tuple<double, double, int, bool>> candidateRows; // 绝对值, 温度值, 行号, 是否来自A列

            // 扫描A列寻找目标温度点（对应AO列）
            for (int row = 2; row <= MAX_SCAN_ROWS; ++row) {
                std::shared_ptr<QXlsx::Cell> cellA = sheet->cellAt(row, 1); // A列
                if (!cellA || cellA->value().toString().isEmpty()) {
                    continue; // 遇到空行不停止，继续扫描
                }

                double calibTemp = cellA->value().toDouble();

                if (calibTemp != targetTemp) {
                    continue;
                }

                // 获取AO列值并计算绝对值
                std::shared_ptr<QXlsx::Cell> cellAO = sheet->cellAt(row, 41); // AO列
                double aoValue = cellAO ? cellAO->value().toDouble() : std::numeric_limits<double>::max();
                double absValue = qAbs(aoValue);

                candidateRows.append({absValue, calibTemp, row, true});
            }

            // 扫描I列寻找目标温度点（对应AP列）
            for (int row = 2; row <= MAX_SCAN_ROWS; ++row) {
                std::shared_ptr<QXlsx::Cell> cellI = sheet->cellAt(row, 9); // I列
                if (!cellI || cellI->value().toString().isEmpty()) {
                    continue; // 遇到空行不停止，继续扫描
                }

                double calibTemp = cellI->value().toDouble();

                if (calibTemp != targetTemp) {
                    continue;
                }

                // 获取AP列值并计算绝对值
                std::shared_ptr<QXlsx::Cell> cellAP = sheet->cellAt(row, 42); // AP列
                double apValue = cellAP ? cellAP->value().toDouble() : std::numeric_limits<double>::max();
                double absValue = qAbs(apValue);

                candidateRows.append({absValue, calibTemp, row, false});
            }

            // 后续代码保持不变...
            // 选择绝对值最小的行
            if (candidateRows.isEmpty()) {
                emit errorOccurred(QString("文件 %1 未找到温度点 %2").arg(filePath).arg(targetTemp));
                return multiData;
            }

            // 按绝对值排序并取第一个
            std::sort(candidateRows.begin(), candidateRows.end(),
                      [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
            auto bestRow = candidateRows.first();

            double calibTemp = std::get<1>(bestRow);
            bool fromAColumn = std::get<3>(bestRow);
            int rowNum = std::get<2>(bestRow);

            qDebug() << "找到最佳行，温度点:" << targetTemp << ", 行号:" << rowNum
                     << ", 校准温度:" << calibTemp << ", 来自A列:" << fromAColumn
                     << ", AO/AP列绝对值:" << std::get<0>(bestRow);

            // 提取对应列的数据
            double standardTemp = 0.0;
            double measuredTemp = 0.0;
            double error = 0.0;

            if (fromAColumn) {
                // 从A列提取
                std::shared_ptr<QXlsx::Cell> cellH = sheet->cellAt(rowNum, 8); // H列
                if (cellH) standardTemp = cellH->value().toDouble();

                std::shared_ptr<QXlsx::Cell> cellAM = sheet->cellAt(rowNum, 39); // AM列(39)
                if (cellAM) measuredTemp = cellAM->value().toDouble();

                std::shared_ptr<QXlsx::Cell> cellAO = sheet->cellAt(rowNum, 41); // AO列(41)
                if (cellAO) error = cellAO->value().toDouble();
            } else {
                // 从I列提取
                std::shared_ptr<QXlsx::Cell> cellP = sheet->cellAt(rowNum, 16); // P列(16)
                if (cellP) standardTemp = cellP->value().toDouble();

                std::shared_ptr<QXlsx::Cell> cellAN = sheet->cellAt(rowNum, 40); // AN列(40)
                if (cellAN) measuredTemp = cellAN->value().toDouble();

                std::shared_ptr<QXlsx::Cell> cellAP = sheet->cellAt(rowNum, 42); // AP列(42)
                if (cellAP) error = cellAP->value().toDouble();
            }

            // 存储当前温度点的四个值: 校准温度, 标准温度, 测量温度, 误差
            fileTemperatures.append({calibTemp, standardTemp, measuredTemp, error});

            // 新增：打印提取的数据
            qDebug() << "提取的数据 - 温度点:" << targetTemp
                     << ", 校准温度:" << calibTemp
                     << ", 标准温度:" << standardTemp
                     << ", 测量温度:" << measuredTemp
                     << ", 误差:" << error;
        }

        allFilesData.append(fileTemperatures);

        // 新增：打印单个文件的完整数据
        qDebug() << "文件" << filePath << "的完整数据:";
        for (int i = 0; i < targetTemperatures.size(); ++i) {
            qDebug() << "  温度点" << targetTemperatures[i]
                     << ": [" << fileTemperatures[i][0] << ", "
                     << fileTemperatures[i][1] << ", "
                     << fileTemperatures[i][2] << ", "
                     << fileTemperatures[i][3] << "]";
        }
    }

    // 新增：打印所有文件的数据
    qDebug() << "所有文件的数据汇总:";
    for (int fileIdx = 0; fileIdx < allFilesData.size(); ++fileIdx) {
        qDebug() << "文件" << fileIdx + 1 << "(" << excelPaths[fileIdx] << "):";
        for (int tempIdx = 0; tempIdx < targetTemperatures.size(); ++tempIdx) {
            qDebug() << "  温度点" << targetTemperatures[tempIdx]
                     << ": [" << allFilesData[fileIdx][tempIdx][0] << ", "
                     << allFilesData[fileIdx][tempIdx][1] << ", "
                     << allFilesData[fileIdx][tempIdx][2] << ", "
                     << allFilesData[fileIdx][tempIdx][3] << "]";
        }
    }

    // 计算三个文件的平均值
    QList<QList<double>> averagedData;
    for (int tempIndex = 0; tempIndex < targetTemperatures.size(); ++tempIndex) {

        QList<double> avgValues;
        for(int i = 0; i < 4; ++i) {
            avgValues.append(0.0);
        }

        qDebug() << "计算温度点" << targetTemperatures[tempIndex] << "的平均值:";
        for (int fileIndex = 0; fileIndex < excelPaths.size() && fileIndex < 3; ++fileIndex) {
            qDebug() << "  文件" << fileIndex + 1 << "数据:"
                     << allFilesData[fileIndex][tempIndex][0] << ", "
                     << allFilesData[fileIndex][tempIndex][1] << ", "
                     << allFilesData[fileIndex][tempIndex][2] << ", "
                     << allFilesData[fileIndex][tempIndex][3];

            for (int valueIndex = 0; valueIndex < 4; ++valueIndex) {
                avgValues[valueIndex] += allFilesData[fileIndex][tempIndex][valueIndex];
            }
        }

        // 计算平均值
        for (int valueIndex = 0; valueIndex < 4; ++valueIndex) {
            avgValues[valueIndex] /= qMin(3, excelPaths.size());
        }

        averagedData.append(avgValues);

        // 新增：打印计算的平均值
        qDebug() << "  温度点" << targetTemperatures[tempIndex] << "的平均值:"
                 << avgValues[0] << ", " << avgValues[1] << ", "
                 << avgValues[2] << ", " << avgValues[3];
    }

    // 整理合并数据
    QList<double> calibrationTemperatures;
    QList<double> standardTemperatures;
    QList<double> measuredTemperatures;
    QList<double> errors;

    for (const auto& tempData : averagedData) {
        calibrationTemperatures.append(tempData[0]);
        standardTemperatures.append(tempData[1]);
        measuredTemperatures.append(tempData[2]);
        errors.append(tempData[3]);
    }

    combinedData.append(calibrationTemperatures);
    combinedData.append(standardTemperatures);
    combinedData.append(measuredTemperatures);
    combinedData.append(errors);
    multiData.append(combinedData);

    // 新增：打印最终合并的数据
    qDebug() << "最终合并的数据:";
    qDebug() << "校准温度:" << calibrationTemperatures;
    qDebug() << "标准温度:" << standardTemperatures;
    qDebug() << "测量温度:" << measuredTemperatures;
    qDebug() << "误差:" << errors;

    qDebug() << "合并后的数据大小:" << combinedData.size();

    // 新增：提取AR系数
    QRegularExpression arRegex(R"(E标准\s*=\s*([+-]?\d+\.?\d*)\s*\*\s*EETO\d+\s*\+\s*([+-]?\d+\.?\d*)\s*\*\s*ETA\d+\s*\+\s*([+-]?\d+\.?\d*))");
    QList<QList<QList<double>>> arCoefficients;

    for (const QString& filePath : excelPaths) {
        QXlsx::Document xlsx(filePath);
        if (!xlsx.load()) {
            emit errorOccurred(QString("无法加载 Excel 文件: %1").arg(filePath));
            return multiData;
        }

        QXlsx::Worksheet* sheet = dynamic_cast<QXlsx::Worksheet*>(xlsx.sheet(xlsx.sheetNames().first()));
        if (!sheet) {
            emit errorOccurred(QString("无法获取工作表: %1").arg(filePath));
            return multiData;
        }

        int rowCount = sheet->dimension().rowCount();
        if (rowCount < 3) {
            emit errorOccurred(QString("文件 %1 行数不足，至少需要3行").arg(filePath));
            return multiData;
        }

        QList<QList<double>> fileArCoeffs;
        for (int row = rowCount - 2; row <= rowCount; ++row) { // 最后三行
            std::shared_ptr<QXlsx::Cell> arCell = sheet->cellAt(row, 44); // AR列（第44列）
            if (!arCell || arCell->value().toString().isEmpty()) {
                emit errorOccurred(QString("文件 %1 第%2行AR列无数据").arg(filePath).arg(row));
                return multiData;
            }

            QString arLine = arCell->value().toString().trimmed();
            qDebug() << "AR行内容: " << arLine;

            QRegularExpressionMatch match = arRegex.match(arLine);
            if (!match.hasMatch()) {
                qDebug() << "AR行内容匹配失败: " ;
                emit errorOccurred(QString("文件 %1 第%2行AR列格式错误: %3").arg(filePath).arg(row).arg(arLine));
                return multiData;
            }

            double a = match.captured(1).toDouble();
            double b = match.captured(2).toDouble();
            double c = match.captured(3).toDouble();
            fileArCoeffs.append({a, b, c});
        }

        arCoefficients.append(fileArCoeffs);
    }

    m_arCoefficients = arCoefficients; // 保存AR系数到成员变量
    return multiData;

}



void PythonProcessor::generateEnergyConfigCommand(const QString& deviceNumber)
{
    if (m_arCoefficients.size() != 3) {
        emit errorOccurred("AR系数缺失，需3个Excel文件的AR数据");
        return;
    }

    QStringList commandParts;
    for (const auto& fileCoeffs : m_arCoefficients) {
        QStringList rowParts;
        for (const auto& row : fileCoeffs) {
            // 关键修改：将 'f' 格式改为 'g'，并调整有效数字位数（如10位）
            rowParts.append(QString("%1,%2,%3")
                                .arg(row[0], 0, 'g', 10)  // 'g' 格式自动去除末尾零
                                .arg(row[1], 0, 'g', 10)
                                .arg(row[2], 0, 'g', 10));
        }
        commandParts.append(rowParts.join(","));
    }

    QString command = QString("SETBLKCALCE,YALL,Nnn,3;0,30,99;%1").arg(commandParts.join(";"));
    QString savePath = QFileInfo(excelPaths.first()).path() + "/" + deviceNumber + "-能量配置命令.txt";

    QFile file(savePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit errorOccurred(QString("无法保存命令文件: %1").arg(savePath));
        return;
    }

    QTextStream stream(&file);
    stream << command;
    file.close();

    emit progressChanged(QString("能量配置命令已生成: %1").arg(savePath));
}

void PythonProcessor::generateMultiPDFCertificate(const QList<QList<QList<double>>>& multiData,
                                                  const QString& excelPath,
                                                  const QString& deviceNumber,
                                                  const QString& calibrationTime)
{
    qDebug() << "Generating PDF Certificate for Multi-Head Device";
    qDebug() << "Excel Path:" << excelPath;
    qDebug() << "Device Number:" << deviceNumber;
    qDebug() << "Calibration Time:" << calibrationTime;

    if (multiData.isEmpty() || multiData[0].size() < 4) {
        emit errorOccurred("无效的校准数据格式，缺少必要数据列");
        return;
    }

    // 准备数据
    const QList<double>& calibrationTemperatures = multiData[0][0];
    const QList<double>& standardTemperatures = multiData[0][1];
    const QList<double>& measuredTemperatures = multiData[0][2];
    // const QList<double>& errors = multiData[0][3];

    // 创建打印机对象
    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setPageSize(QPageSize(QPageSize::A4));
    printer.setPageOrientation(QPageLayout::Portrait);
    printer.setPageMargins(QMarginsF(15, 15, 15, 15), QPageLayout::Millimeter);

    // 设置输出路径
    QString pdfPath = excelPath;
    pdfPath.replace("-1结果.xlsx", "-测试报告.pdf");
    printer.setOutputFileName(pdfPath);

    QPainter painter(&printer);
    if (!painter.isActive()) {
        emit errorOccurred("无法启动PDF绘制引擎");
        return;
    }

    // 样式初始化
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const int dpi = printer.resolution();
    const double mmToPixel = dpi / 25.4;

    // 页面布局参数（增加序号列）
    const int headerHeight = 15 * mmToPixel;
    const int footerHeight = 10 * mmToPixel;
    const int rowHeight = 8 * mmToPixel;
    const QVector<int> colWidths = { 20, 35, 35, 35, 35 }; // 新增序号列，宽度20mm

    // 计算表格参数
    const int tableWidth = std::accumulate(colWidths.begin(), colWidths.end(), 0) * mmToPixel;
    const int tableX = (printer.width() - tableWidth) / 2;
    int currentY = 0;
    bool isFirstPage = true;

    QString newDeviceNumber = deviceNumber;
    if (deviceNumber.startsWith("多")) {
        QString numberPart = deviceNumber.mid(1); // 提取"多"后面的数字部分
        newDeviceNumber = QString("IR37E800%1").arg(numberPart);
    }

    // 分页绘制
    int dataIndex = 0;
    while (dataIndex < calibrationTemperatures.size()) {
        if (!isFirstPage) {
            printer.newPage();
            currentY = 0;
        }

        // 绘制页眉（仅第一页）
        if (isFirstPage) {
            // 绘制标题
            QFont titleFont("SimHei", 20, QFont::Bold);
            painter.setFont(titleFont);

            int titleHeight = 24 * mmToPixel;

            painter.drawText(QRect(0, currentY, printer.width(), titleHeight),
                             Qt::AlignCenter | Qt::TextWordWrap,
                             "测试报告");
            currentY += headerHeight + 10 * mmToPixel;

            // 绘制设备信息
            QFont infoFont("SimSun", 10);
            painter.setFont(infoFont);
            painter.drawText(QRect(0, currentY, printer.width(), rowHeight),
                             Qt::AlignCenter,
                             QString("设备编号: %1").arg(newDeviceNumber));
            currentY += rowHeight + 5 * mmToPixel;

            painter.drawText(QRect(0, currentY, printer.width(), rowHeight),
                             Qt::AlignCenter,
                             QString("测试日期: %1").arg(calibrationTime));
            currentY += rowHeight + 5 * mmToPixel;
        }

        // 计算可用高度
        const int pageHeight = printer.pageRect(QPrinter::DevicePixel).height();
        int remainingHeight = pageHeight - currentY - footerHeight;

        // 绘制表头（添加序号列）
        QFont headerFont("SimSun", 10, QFont::Bold);
        painter.setFont(headerFont);

        // 表头背景
        painter.fillRect(tableX, currentY, tableWidth, headerHeight, QColor(240, 240, 240));

        // 表头文字
        int xPos = tableX;
        const QStringList headers = {"序号", "测试点(℃)", "标准器温度(℃)", "红外温度计示值(℃)", "示值误差(℃)"};
        for (int i = 0; i < headers.size(); ++i) {
            QRect headerRect(xPos, currentY, colWidths[i] * mmToPixel, headerHeight);
            painter.drawText(headerRect, Qt::AlignCenter, headers[i]);
            xPos += colWidths[i] * mmToPixel;
        }
        currentY += headerHeight;
        remainingHeight -= headerHeight;

        // 计算本页可显示行数
        int maxRows = qMin(remainingHeight / rowHeight,
                           calibrationTemperatures.size() - dataIndex);

        // 绘制数据行
        QFont dataFont("SimSun", 9);
        painter.setFont(dataFont);

        for (int i = 0; i < maxRows; ++i) {
            const int rowIndex = dataIndex + i;
            int serialNumber = dataIndex + i + 1; // 序号从1开始

            // 交替行背景色
            if (i % 2 == 0) {
                painter.fillRect(tableX, currentY, tableWidth, rowHeight, QColor(250, 250, 250));
            }

            // 绘制单元格数据
            xPos = tableX;
            const QList<double> rowData = {
                calibrationTemperatures[rowIndex],
                standardTemperatures[rowIndex],
                measuredTemperatures[rowIndex]
                // 不再使用预计算的误差值，改为实时计算
            };

            // 绘制序号列
            QRect serialRect(xPos, currentY, colWidths[0] * mmToPixel, rowHeight);
            painter.drawText(serialRect, Qt::AlignCenter, QString::number(serialNumber));
            xPos += colWidths[0] * mmToPixel;

            // 绘制数据列（前四列）
            for (int col = 0; col < rowData.size(); ++col) {
                QRect cellRect(xPos, currentY, colWidths[col+1] * mmToPixel, rowHeight);
                painter.drawText(cellRect, Qt::AlignCenter,
                                 QString::number(rowData[col], 'f', 2));

                // 绘制列分隔线
                if (col < rowData.size() - 1) {
                    painter.setPen(QColor(200, 200, 200));
                    painter.drawLine(xPos + colWidths[col+1] * mmToPixel, currentY,
                                     xPos + colWidths[col+1] * mmToPixel, currentY + rowHeight);
                    painter.setPen(Qt::black);
                }

                xPos += colWidths[col+1] * mmToPixel;
            }

            // 计算并绘制误差列（第五列）
            // 提取原始数值
            double rawStandard = rowData[1];  // 原始标准器温度
            double rawMeasured = rowData[2];  // 原始测量温度

            // 格式化为两位小数的字符串（模拟显示值）
            QString stdStr = QString::number(rawStandard, 'f', 2);
            QString measStr = QString::number(rawMeasured, 'f', 2);

            // 转换回double类型（此时值为四舍五入后的显示值）
            double displayedStandard = stdStr.toDouble();
            double displayedMeasured = measStr.toDouble();

            double calculatedError = displayedMeasured - displayedStandard;  // 实时计算误差

            QRect errorRect(xPos, currentY, colWidths[4] * mmToPixel, rowHeight);
            painter.drawText(errorRect, Qt::AlignCenter,
                             QString::number(calculatedError, 'f', 2));

            // 绘制列分隔线（误差列后）
            painter.setPen(QColor(200, 200, 200));
            painter.drawLine(xPos + colWidths[4] * mmToPixel, currentY,
                             xPos + colWidths[4] * mmToPixel, currentY + rowHeight);
            painter.setPen(Qt::black);

            xPos += colWidths[4] * mmToPixel;

            // 绘制行分隔线
            painter.setPen(QColor(220, 220, 220));
            painter.drawLine(tableX, currentY + rowHeight,
                             tableX + tableWidth, currentY + rowHeight);
            painter.setPen(Qt::black);

            currentY += rowHeight;
        }

        dataIndex += maxRows;
        isFirstPage = false;
    }

    // 添加测试员和审核员信息
    QFont footerFont;
    footerFont.setPointSize(10);
    painter.setFont(footerFont);

    // 计算底部Y坐标
    int footerY = printer.height() - 5000;

    // 绘制测试员信息（左对齐）
    painter.drawText(QRect(200, footerY, printer.width()/2 - 150, 300),
                     QString("测试员：%1").arg(m_testerName.isEmpty() ? "未填写" : m_testerName),
                     QTextOption(Qt::AlignLeft | Qt::AlignVCenter));

    // 绘制审核员信息（右对齐）
    painter.drawText(QRect(printer.width()/2 + 100, footerY, printer.width()/2 - 150, 300),
                     QString("复核员：%1").arg(m_reviewerName.isEmpty() ? "未填写" : m_reviewerName),
                     QTextOption(Qt::AlignRight | Qt::AlignVCenter));

    painter.end();

    // 生成完成提示
    emit progressChanged(QString("测试报告已生成: %1").arg(pdfPath));
}

QString PythonProcessor::generateCalibrationCommand(const QList<QList<double>>& coefficients, const QString& deviceNumber)
{
    // 假设温度分段为0-30, 30-100, 100-130
    QStringList tempRanges = {"0", "30", "100"};

    // 构建命令开头
    QString command = "SETBLKCALCE,YALL,Nnn,3::";

    // 累加和校验系数
    QList<double> sumA1, sumA2, sumB;
    sumA1 << 0 << 0 << 0;
    sumA2 << 0 << 0 << 0;
    sumB << 0 << 0 << 0;

    // 构建每段的系数
    for (int i = 0; i < coefficients.size(); ++i) {
        if (i < tempRanges.size()) {
            double a1 = coefficients[i][0];
            double a2 = coefficients[i][1];
            double b = coefficients[i][2];

            // 累加和校验系数
            sumA1[i] += a1;
            sumA2[i] += a2;
            sumB[i] += b;

            // 添加当前段的系数到命令
            command += tempRanges[i] + ":" +
                       QString::number(a1, 'g', 15) + ";" +
                       QString::number(a2, 'g', 15) + ";" +
                       QString::number(b, 'g', 15);

            if (i < coefficients.size() - 1) {
                command += "/";
            }
        }
    }

    // 添加和校验段的系数
    command += "/130:" +
               QString::number(sumA1[0] + sumA1[1] + sumA1[2], 'g', 15) + ";" +
               QString::number(sumA2[0] + sumA2[1] + sumA2[2], 'g', 15) + ";" +
               QString::number(sumB[0] + sumB[1] + sumB[2], 'g', 15) + "/";

    return command;
}

void PythonProcessor::saveCommandToFile(const QString& command, const QString& deviceNumber)
{
    // 生成文件名：NXX-能量配置命令.txt
    QString fileName = deviceNumber + "-能量配置命令.txt";

    // 确定保存路径（假设与Excel文件同目录）
    QFileInfo excelInfo(m_outputPath);
    QString savePath = excelInfo.path() + "/" + fileName;

    // 写入文件
    QFile file(savePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        out << command;
        file.close();

        emit progressChanged("能量配置命令已保存: " + savePath);
        qDebug() << "命令已保存至:" << savePath;
    } else {
        emit errorOccurred("无法保存能量配置命令文件: " + file.errorString());
        qDebug() << "保存命令文件失败:" << file.errorString();
    }
}

bool PythonProcessor::isProcessing() const {
    return m_isProcessing || (m_process->state() == QProcess::Running);
}

// PythonProcessor.cpp 实现超时处理
void PythonProcessor::setProcessingTimeout(int milliseconds) {
    m_processingTimeout = milliseconds;
}

void PythonProcessor::resetProcess()
{
    if (m_process->state() == QProcess::Running) {
        m_process->terminate();
        if (!m_process->waitForFinished(1000)) {
            m_process->kill();
            m_process->waitForFinished(1000);
        }
    }
    m_process->close();
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    m_isProcessing = false;
}

void PythonProcessor::terminateProcess()
{
    if (m_process->state() == QProcess::Running) {
        m_process->terminate(); // 先尝试优雅终止
        if (!m_process->waitForFinished(1000)) {
            m_process->kill(); // 强制终止
            m_process->waitForFinished(1000);
        }
    }
    m_isProcessing = false;

    // 重置进程状态
    m_process->close();
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
}

void PythonProcessor::setTesterReviewerInfo(const QString& tester, const QString& reviewer) {
    m_testerName = tester;
    m_reviewerName = reviewer;
    qDebug() << "接收测试员:" << m_testerName << "审核员:" << m_reviewerName;
}

void PythonProcessor::setMergedFilePath(const QString& path) {
    m_mergedFilePath = path;
    qDebug() << "接收合并文件路径:" << m_mergedFilePath;
}

QDateTime PythonProcessor::extractLatestDateFromMergedFile()
{
    QDateTime latestDate;

    // 检查合并文件路径是否有效
    if (m_mergedFilePath.isEmpty()) {
        qDebug() << "合并文件路径为空，使用当前时间";
        return QDateTime::currentDateTime();
    }

    // 加载合并后的Excel文件
    QXlsx::Document doc(m_mergedFilePath);
    if (!doc.load()) {
        qDebug() << "合并文件加载失败:" << m_mergedFilePath;
        return QDateTime::currentDateTime();
    }

    // 遍历所有工作表（跳过"标准"工作表）
    foreach (const QString& sheetName, doc.sheetNames()) {
        if (sheetName == "标准") continue;

        doc.selectSheet(sheetName);
        int lastRow = doc.dimension().lastRow();  // 获取当前工作表最后一行

        // 从第2行开始读取B列（第2列）日期
        for (int row = 2; row <= lastRow; ++row) {
            QVariant dateVar = doc.read(row, 2);  // B列是第2列（列号从1开始）
            if (dateVar.isNull()) continue;

            // 解析日期（支持多种格式）
            QDateTime currentDate;
            QString dateStr = dateVar.toString().trimmed();
            QList<QString> formats = {
                "yyyy-MM-dd", "yyyy/MM/dd", "yyyyMMdd",
                "yyyy-MM-dd hh:mm:ss", "yyyy/MM/dd hh:mm:ss", "yyyyMMddhhmmss"
            };

            foreach (const QString& format, formats) {
                currentDate = QDateTime::fromString(dateStr, format);
                if (currentDate.isValid()) break;
            }

            // 更新最新日期
            if (currentDate.isValid() && (currentDate > latestDate || !latestDate.isValid())) {
                latestDate = currentDate;
            }
        }
    }

    // 未找到有效日期时使用当前时间
    if (!latestDate.isValid()) {
        latestDate = QDateTime::currentDateTime();
        qDebug() << "合并文件中未找到有效日期，使用当前时间";
    }

    return latestDate;
}

