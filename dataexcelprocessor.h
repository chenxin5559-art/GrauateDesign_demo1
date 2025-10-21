#pragma once
#include <QObject>
#include <QtConcurrent/QtConcurrent>
#include "xlsxdocument.h"

class DataExcelProcessor : public QObject
{
    Q_OBJECT
public:
    enum ProcessType {
        StandardData,
        SingleHead,
        MultiHead,
        MergeFiles
    };

    explicit DataExcelProcessor(QObject* parent = nullptr);

    QString mergeSingleHeadFiles(const QString& inFile, const QString& outFile);

    void generateTemplateExcelforMulitiHead(const QString& file1, const QString& file2, QString& outputTemplatePath);

    QString lastError() const { return m_lastError; }
    void clearError() { m_lastError.clear(); }

signals:
    void progressUpdated(int percent);
    void operationCompleted(bool success, const QString& resultPath);
    void errorOccurred(const QString& message);

public slots:
    void startProcessing(ProcessType type, const QString& sourcePath,
                         const QString& templatePath = "",
                         const QString& outputPath = "");

private:
    // 核心处理方法
    void processStandard(const QString& excelPath);
    void processSingleHead(const QString& excelPath);
    void processMultiHead(const QString& excelPath);
    void mergeFiles(const QString& file1, const QString& file2,
                    const QString& templatePath);

    // 辅助方法
    QString findMatchingTxtFile(const QString& excelFilePath,
                                const QString& portNumber,
                                const QDate& date);

    QVector<double> processTxtFile(const QString& filePath,
                                   const QDateTime& targetDateTime);

    QVector<double> processSingleHeadTxtFile(const QString& txtFilePath,
                                             const QDateTime& dateTime);

    QVector<double> processMultiHeadTxtFile(const QString& txtFilePath,
                                            const QDateTime& dateTime);

    int findRowByDateTime(QXlsx::Document& xlsx, const QDateTime& dt);

    int getLastRow(QXlsx::Document& xlsx, int col);

    int copySheetData(QXlsx::Document& srcXlsx, QXlsx::Document& destXlsx,
                                          int srcStartRow, int destStartRow, const QString& envType,
                      const QMap<QString, double>& standardTempData);

    void sortSheetData(QXlsx::Document& xlsx, int startRow, int endRow, int col);

    void loadStandardTemperature(QXlsx::Document& srcXlsx,
                                 QMap<QString, double>& standardTempData);

    // Excel操作封装方法
    bool writeTemperatures(QXlsx::Document& xlsx,
                           const QMap<QDateTime, QVector<double>>& dataMap,
                           const QMap<QDateTime, int>& rowMap);


    QVector<QPair<int, QString>> detectDevices(QXlsx::Document& doc);
    void processDeviceData(QXlsx::Document& srcXlsx, QXlsx::Document& destXlsx,
                           const QString& envType, const QPair<int, QString>& device,
                           const QMap<QString, double>& stdTemp);

    QString m_lastError; // 存储最新错误信息


};
