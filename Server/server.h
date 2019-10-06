#ifndef SERVER_H
#define SERVER_H

#include <QTcpServer>
#include <QVector>
#include "database.h"
class QThread;
class ServerWorker;
class QJsonObject;

class Server : public QTcpServer
{
    Q_OBJECT
    Q_DISABLE_COPY(Server)

public:
    Server(QObject *parent = nullptr,Database* db=0);
    ~Server() override;
    bool tryConnectionToDatabase();

protected:
    void incomingConnection(qintptr socketDescriptor) override;

private:
    const int m_idealThreadCount;
    QVector<QThread *> m_availableThreads;
    QVector<int> m_threadsLoad;
    QVector<ServerWorker *> m_clients;
    Database* db;

private slots:
    void broadcast(const QJsonObject &message, ServerWorker *exclude);
    void jsonReceived(ServerWorker *sender, const QJsonObject &doc);
    void userDisconnected(ServerWorker *sender, int threadIdx);
//    void userError(ServerWorker *sender);

public slots:
    void stopServer();

private:
    void jsonFromLoggedOut(ServerWorker *sender, const QJsonObject &doc);
    QJsonObject signup(ServerWorker *sender,const QJsonObject &doc);
    QJsonObject login(ServerWorker *sender,const QJsonObject &doc);
    QJsonObject updateNick(const QJsonObject &doc);
    QJsonObject updatePass(const QJsonObject &doc);
    QJsonObject checkOldPass(const QJsonObject &doc);
    QJsonObject getFiles(const QJsonObject &doc);
    QJsonObject createNewFile(const QJsonObject &doc);

    void jsonFromLoggedIn(ServerWorker *sender, const QJsonObject &doc);
    void sendJson(ServerWorker *destination, const QJsonObject &message);
    void sendProfileImage(ServerWorker *destination);

signals:
    void logMessage(const QString &msg);
    void stopAllClients();
};

#endif // SERVER_H
