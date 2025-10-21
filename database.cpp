#include "database.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QCryptographicHash>
#include <QUuid>

Database::Database(QObject *parent) : QObject(parent) {
    m_db = QSqlDatabase::addDatabase("QSQLITE");
    m_db.setDatabaseName("users.db");  // 数据库文件路径
}

Database::~Database() {
    if (m_db.isOpen()) {
        m_db.close();
    }
}

bool Database::initialize() {

    if (!m_db.open()) {
        qWarning() << "Database open error:" << m_db.lastError().text();
        return false;
    }

    QSqlQuery query;
    query.prepare(
        "CREATE TABLE IF NOT EXISTS users ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "username TEXT UNIQUE NOT NULL,"
        "password_hash TEXT NOT NULL,"
        "salt TEXT NOT NULL"
        ")"
        );

    if (!query.exec()) {
        qWarning() << "Create table error:" << query.lastError().text();
        return false;
    }

    // 检查并创建管理员（参数化查询）
    QSqlQuery checkAdmin;
    checkAdmin.prepare("SELECT username FROM users WHERE username = ?");
    checkAdmin.addBindValue("admin");
    if (!checkAdmin.exec()) {
        qWarning() << "Check admin error:" << checkAdmin.lastError().text();
        return false;
    }

    if (!checkAdmin.next()) {
        if (!createUser("admin", "12345")) {
            qWarning() << "Failed to create admin user";
            return false;
        }
    }

    return true;

}

bool Database::createUser(const QString &username, const QString &password) {
    QSqlQuery query;
    QString salt = QUuid::createUuid().toString();
    QString hashedPassword = hashPassword(password, salt);

    query.prepare("INSERT INTO users (username, password_hash, salt) VALUES (?, ?, ?)");
    query.addBindValue(username);
    query.addBindValue(hashedPassword);
    query.addBindValue(salt);

    if (!query.exec()) {
        qWarning() << "Create user error:" << query.lastError().text();
        return false;
    }
    return true;
}

bool Database::validateUser(const QString &username, const QString &password) {
    QSqlQuery query;
    query.prepare("SELECT password_hash, salt FROM users WHERE username = ?");
    query.addBindValue(username);

    if (!query.exec() || !query.next()) {
        return false; // 用户不存在
    }

    QString storedHash = query.value(0).toString();
    QString salt = query.value(1).toString();
    QString inputHash = hashPassword(password, salt);

    return (storedHash == inputHash);
}

QString Database::hashPassword(const QString &password, const QString &salt) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(password.toUtf8());
    hash.addData(salt.toUtf8());
    return QString(hash.result().toHex());
}
