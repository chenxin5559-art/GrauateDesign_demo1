#ifndef DATABASE_H
#define DATABASE_H

#include <QObject>
#include <QSqlDatabase>

#include <QVariant>
#include <QDebug>

class Database : public QObject {
    Q_OBJECT
public:
    explicit Database(QObject *parent = nullptr);
    ~Database();

    bool initialize();  // 初始化数据库和表
    bool createUser(const QString &username, const QString &password);
    bool validateUser(const QString &username, const QString &password);

private:
    QSqlDatabase m_db;
    QString hashPassword(const QString &password, const QString &salt);
};

#endif // DATABASE_H
