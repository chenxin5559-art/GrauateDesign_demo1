#ifndef PYTHONPROCESSOR_H
#define PYTHONPROCESSOR_H

#include <QObject>
#include <QProcess>
#include <QFileInfo>
#include <xlsxdocument.h>

class PythonProcessor : public QObject
{
    Q_OBJECT
public:
    explicit PythonProcessor(QObject *parent = nullptr);

    // 启动处理流程
    void startProcessing(const QString& inputFilePath,
                         const QString& nid = "");

    void startMultiProcessing(const QString& inputFilePath, const QString& nid);

    void generateEnergyConfigCommand(const QString& deviceNumber); // 新增命令生成函数

    // 新增：生成能量配置命令（带系数参数）
    void generateEnergyConfigCommand(const QString& deviceNumber, const QList<QList<double>>& coefficients);

    bool isProcessing() const;

    void setProcessingTimeout(int milliseconds); // 设置处理超时时间

    void resetProcess();
    void terminateProcess() ;

signals:
    // 处理进度信息
    void progressChanged(const QString& message);
    // 处理完成信号
    void processingFinished(bool success, const QString& resultPath);
    // 错误信号
    void errorOccurred(const QString& error);

    void progressUpdated(int percentage, const QString& message); // 新增进度百分比信号

public slots:

    void setTesterReviewerInfo(const QString& tester, const QString& reviewer);

    void setMergedFilePath(const QString& path);  // 设置合并文件路径


private slots:
    void handleProcessOutput();
    void handleProcessError(QProcess::ProcessError error);
    void handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void handleMultiProcessFinished(int exitCode, QProcess::ExitStatus exitStatus); // 添加新槽函数声明



    QDateTime extractLatestDateFromMergedFile();



private:
    QProcess *m_process;
    bool m_isProcessing = false;
    QString m_outputPath;  // 保存输出路径

    int m_processingTimeout;

    QString generateOutputPath(const QFileInfo& inputFile, bool isMultiHead) const;
    QString extractNid(const QString& fileName) const;
    QList<QList<QList<double>>> extractDataFromExcel(const QString& filePath);
    void generateCalibrationCertificate(const QString& resultPath);
    void generatePDFCertificate(const QList<QList<QList<double>>>& data, const QString& excelPath, const QString& deviceNumber, const QString& calibrationTime);
    QPair<QString, QStringList> extractMultiExcelPaths(const QString& baseResultPath);
    QList<QList<QList<double>>> extractMultiDataFromExcel(const QStringList& excelPaths);
    void generateMultiPDFCertificate(const QList<QList<QList<double>>>& multiData, const QString& excelPath, const QString& deviceNumber, const QString& calibrationTime);
    QList<QList<QList<double>>> m_arCoefficients;
    QList<QList<double>> coefficients;
    QStringList excelPaths;
    QString generateCalibrationCommand(const QList<QList<double>>& coefficients, const QString& deviceNumber);
    void saveCommandToFile(const QString& command, const QString& deviceNumber);

    QString m_testerName;
    QString m_reviewerName;
    QString m_mergedFilePath;
};

#endif // PYTHONPROCESSOR_H
