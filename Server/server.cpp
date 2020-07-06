#include <QThread>
#include <functional>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTimer>
#include <QSslSocket>
#include <QSslConfiguration>
#include <QDir>
#include <QtEndian>
#include "server.h"
#include "serverworker.h"

#include <QImage>
//#include "../Client/CRDT.h"

Server::Server(QObject *parent)
    : QTcpServer(parent),
      // Ideal number of threads based on the number of processor cores
      m_idealThreadCount(qMax(QThread::idealThreadCount(), 1))
{
    // Pool of available threads:
    // each thread handles a certain number of clients
    m_availableThreads.reserve(m_idealThreadCount);
    m_threadsLoad.reserve(m_idealThreadCount);

    mapFileWorkers=new QMap<QString,QList<ServerWorker*>*>();
    this->db.connect();

    // Create folder to store profile images
    QString profile_images_path = QDir::currentPath() + IMAGES_PATH;
    QDir dir_images(profile_images_path);
    if (!dir_images.exists()) {
        qDebug().nospace() << "Folder " << profile_images_path << " created";
        dir_images.mkpath(".");
    }

    // TODO: TO CHANGE WITH YOUR LOCAL DIRECTORY
    QFile keyFile(":/resources/certificates/server.key");
    keyFile.open(QIODevice::ReadOnly);
    key = QSslKey(keyFile.readAll(), QSsl::Rsa);
    keyFile.close();

    QFile certFile(":/resources/certificates/server.crt");
    certFile.open(QIODevice::ReadOnly);
    cert = QSslCertificate(certFile.readAll());
    certFile.close();
    //qDebug()<<'\n Common Name: '<<cert.issuerInfo(QSslCertificate::CommonName)<<" SubjectName: "<<cert.subjectInfo(QSslCertificate::CommonName);

    // Timer to periodically save the open files
    QTimer *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &Server::saveFile);
    timer->start(1000 * SAVE_INTERVAL_SEC);
}

Server::~Server()
{
    for (QThread *singleThread : m_availableThreads) {
        singleThread->quit();
        singleThread->wait();
    }
}

//Override from QTcpServer.
// This gets executed every time a client attempts a connection with the server
void Server::incomingConnection(qintptr socketDescriptor)
{
    ServerWorker *worker = new ServerWorker;
    //Sets the socket descriptor this server should use when listening
    // for incoming connections to socketDescriptor.
    // Returns true if the socket is set successfully; otherwise returns false.
    if (!worker->setSocketDescriptor(socketDescriptor,key,cert)) {
        worker->deleteLater();
        return;
    }

    int threadIdx = m_availableThreads.size();
    if (threadIdx < m_idealThreadCount) { // We can add a new thread
        m_availableThreads.append(new QThread(this));
        m_threadsLoad.append(1);
        m_availableThreads.last()->start();
    } else {
        // Find the thread with the least amount of clients and use it
        threadIdx = std::distance(m_threadsLoad.cbegin(),
                                  std::min_element(m_threadsLoad.cbegin(),
                                                   m_threadsLoad.cend()));
        ++m_threadsLoad[threadIdx];
    }
    // Assign to the chosen thread
    worker->moveToThread(m_availableThreads.at(threadIdx));

    // When the thread finished, insert in the event queue the worker deletion
    connect(m_availableThreads.at(threadIdx), &QThread::finished,
            worker, &QObject::deleteLater);
    // userDisconnected called when connection closed by the client
    connect(worker, &ServerWorker::disconnectedFromClient, this,
            std::bind(&Server::userDisconnected, this, worker, threadIdx));

    connect(worker, &ServerWorker::jsonReceived, this,
            std::bind(&Server::jsonReceived,
                      this, worker, std::placeholders::_1));
    connect(worker, &ServerWorker::signingUp_updatingImage, this,
            std::bind(&Server::signup_updateImage,
                      this, worker, std::placeholders::_1));
    connect(this, &Server::stopAllClients,
            worker, &ServerWorker::disconnectFromClient);
    m_clients.append(worker);
}

QByteArray Server::createByteArrayJsonImage(QJsonObject &message,
                                            QVector<QByteArray> &v){
    QByteArray byte_array = QJsonDocument(message).toJson();
    quint32 size_json = byte_array.size();

    // Depends on the endliness of the machine
    QByteArray ba((const char *)&size_json, sizeof(size_json));
    ba.append(byte_array);

    if(v.size()==0){
        quint32 size_img = 0;
        QByteArray p((const char *)&size_img, sizeof(size_img));
        ba.append(p);
    }

    for (QByteArray a: v){
        quint32 size_img = a.size();
        QByteArray p((const char *)&size_img, sizeof(size_img));
        if (size_img!=0)
            p.append(a);
        ba.append(p);
    }
    return ba;
}

void Server::sendJson(ServerWorker *destination, const QJsonObject &message)
{
    Q_ASSERT(destination);
    QTimer::singleShot(0, destination, std::bind(&ServerWorker::sendJson,
                                                 destination, message));
}

void Server::sendByteArray(ServerWorker *destination,const QByteArray &toSend){
    Q_ASSERT(destination);
    QTimer::singleShot(0, destination,
                       std::bind(&ServerWorker::sendByteArray,
                                 destination, toSend));
}


bool Server::tryConnectionToDatabase()
{
    return db.checkConnection();
}

bool Server::tryConnectionToMongo()
{
    return db.checkConnection();
}

void Server::broadcast(const QJsonObject &message, ServerWorker *exclude)
{
    QString filename = exclude->getFilename();
    QList<ServerWorker*>* active_clients = mapFileWorkers->value(filename);
    for (ServerWorker *worker : *active_clients) {
        Q_ASSERT(worker);
        if (worker == exclude)
            continue;
        sendJson(worker, message);
    }
}

void Server::broadcastByteArray(const QJsonObject &message,
                                const QByteArray &bArray,
                                ServerWorker *exclude){
    QByteArray byte_array = QJsonDocument(message).toJson();
    quint32 size_json = byte_array.size();

    // Depends on the endliness of the machine
    QByteArray ba((const char *)&size_json, sizeof(size_json));
    ba.append(byte_array);

    if (bArray.size()!=0) {
        quint32 size_img = bArray.size();
        QByteArray p((const char *)&size_img, sizeof(size_img));
        p.append(bArray);
        ba.append(p);
    } else {
        quint32 size_img = 0;
        QByteArray p((const char *)&size_img, sizeof(size_img));
        ba.append(p);
    }

    QString filename = exclude->getFilename();
    QList<ServerWorker*>* active_clients = mapFileWorkers->value(filename);
    for (ServerWorker *worker : *active_clients) {
        Q_ASSERT(worker);
        if (worker == exclude)
            continue;
        sendByteArray(worker, ba);
    }
}

void Server::jsonReceived(ServerWorker *sender, const QJsonObject &json)
{
//	qDebug() << json;
    if (sender->getNickname().isEmpty()) {
        return jsonFromLoggedOut(sender, json);
    } else {
        jsonFromLoggedIn(sender, json);
    }
}

// Remove disconnected client and notify
void Server::userDisconnected(ServerWorker *sender, int threadIdx)
{
    --m_threadsLoad[threadIdx];
    m_clients.removeAll(sender);

    if(!sender->getFilename().isNull() && !sender->getFilename().isEmpty())
        udpateSymbolListAndCommunicateDisconnection(sender->getFilename(), sender);
    sender->deleteLater();
}

void Server::stopServer()
{
    emit stopAllClients();
    close();
}

void Server::jsonFromLoggedOut(ServerWorker *sender, const QJsonObject &docObj)
{
    const QJsonValue typeVal = docObj.value(QLatin1String("type"));
    if (typeVal.isNull() || !typeVal.isString())
        return;
    if (typeVal.toString().compare(QLatin1String("login"),
                                   Qt::CaseInsensitive) == 0){
        QJsonObject message=this->checkCredentials(sender,docObj);
        QVector<QByteArray> tmp;
        if (message.value(QLatin1String("success"))==true){
            QString image_path = QDir::currentPath() + IMAGES_PATH + "/"
                                 + sender->getUsername() + ".png";
            QFileInfo file(image_path);
            if (file.exists()) {
                // Read Image
                QImage p(image_path);
                QByteArray bArray;
                QBuffer buffer(&bArray);
                buffer.open(QIODevice::WriteOnly);
                p.save(&buffer, "PNG");

                tmp.push_back(bArray);
            }
        }
        this->sendByteArray(sender,createByteArrayJsonImage(message,tmp));
    } else if (typeVal.toString().compare(QLatin1String("check_username"),
                                          Qt::CaseInsensitive) == 0){
        QJsonObject message = this->checkAlreadyExistingUsername(docObj);
        this->sendJson(sender,message);

    }
}

QJsonObject Server::checkAlreadyExistingUsername(
                                     const QJsonObject &doc) {
    const QJsonValue user = doc.value(QLatin1String("username"));
    QJsonObject message;
    message["type"] = QStringLiteral("check_username");

    if (user.isNull() || !user.isString()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Wrong username format");
        return message;
    }
    const QString username = user.toString().simplified();
    if (username.isEmpty()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Empty username");
        return message;
    }

    DatabaseError result = this->db.checkAlreadyExistingUsername(username);
    if (result == CONNECTION_ERROR || result == QUERY_ERROR) {
        message["success"] = false;
        message["reason"] = QStringLiteral("Database error");
        return message;
    }
    if (result == ALREADY_EXISTING_USER) {
        message["success"] = false;
        message["username"]=username;
        message["reason"] = QStringLiteral("The username already exists");
        return message;
    }
    if (result == SUCCESS) {
        message["success"] = true;
        message["username"]=username;
        return message;
    }
}

void Server::signup_updateImage(ServerWorker *sender,
                                const QByteArray &json_data){
    QJsonObject message;
    quint32 size = qFromLittleEndian<qint32>(
                reinterpret_cast<const uchar *>(json_data.left(4).data())
    );
    QByteArray json = json_data.mid(4, size);


    QJsonParseError parseError;
    // We try to create a json document with the data we received
    const QJsonDocument jsonDoc = QJsonDocument::fromJson(json, &parseError);
    QString username;
    QString typeValS;

    if (parseError.error == QJsonParseError::NoError && jsonDoc.isObject()) {
        // and is a JSON object
        // actions depend on the type of message
        QJsonObject docObj = jsonDoc.object();
        const QJsonValue typeVal = docObj.value(QLatin1String("type"));
        if (typeVal.isNull() || !typeVal.isString()) {
            message["success"] = false;
            message["reason"] = QStringLiteral("Wrong format");
            this->sendJson(sender,message);
            return;
        }
        typeValS=typeVal.toString();

        if (typeValS.compare(QLatin1String("signup"), Qt::CaseInsensitive) == 0
                || typeValS.compare(QLatin1String("update_image"),
                                    Qt::CaseInsensitive) == 0) {

            const QJsonValue user = docObj.value(QLatin1String("username"));
            username = user.toString().simplified();
            if (username.isEmpty()) {
                if (typeValS.compare(QLatin1String("signup"),
                                     Qt::CaseInsensitive) == 0) {
                    message["success"] = false;
                    message["reason"] = QStringLiteral("Empty email");
                    this->sendJson(sender,message);
                }
                return;
            }
            if (typeValS.compare(QLatin1String("signup"),
                                 Qt::CaseInsensitive) == 0) {
                message["type"] = QStringLiteral("signup");
                if (user.isNull() || !user.isString()) {
                    message["success"] = false;
                    message["reason"] = QStringLiteral("Wrong username format");
                    this->sendJson(sender,message);
                    return;
                }

                const QJsonValue pass = docObj.value(QLatin1String("password"));
                if (pass.isNull() || !pass.isString()) {
                    message["success"] = false;
                    message["reason"] = QStringLiteral("Wrong password format");
                    this->sendJson(sender,message);
                    return;
                }
                const QString password = pass.toString().simplified();
                if (password.isEmpty()) {
                    message["success"] = false;
                    message["reason"] = QStringLiteral("Empty password");
                    this->sendJson(sender,message);
                    return;
                }
                DatabaseError result = this->db.signup(username,password);
                if (result == CONNECTION_ERROR || result == QUERY_ERROR) {
                    message["success"] = false;
                    message["reason"] = QStringLiteral("Database error");
                    this->sendJson(sender,message);
                    return;
                }
                if (result == ALREADY_EXISTING_USER) {
                    message["success"] = false;
                    message["reason"] = QStringLiteral("The username "
                                                       "already exists");
                    this->sendJson(sender,message);
                    return;
                }
            }
        }
    } else {
        message["success"] = false;
        message["reason"] = QStringLiteral("JSON error");
        this->sendJson(sender,message);
        return;
    }

    QByteArray image_array = json_data.mid(4+size,-1);
    quint32 img_size = qFromLittleEndian<qint32>(
                reinterpret_cast<const uchar *>(image_array.left(4).data())
    );
    if (img_size != 0) {
            QByteArray img = image_array.mid(4, img_size);
            image_array = image_array.mid(img_size + 4);
            QImage p;
            p.loadFromData(img);

            QString image_path = QDir::currentPath() + IMAGES_PATH
                                 + "/" + username + ".png";
            qDebug()<<image_path;
            QFile file(image_path);
            if (file.exists()) { // WriteOnly doesn't seem to
                                 // override as it should be
                file.remove();	// according to the documentation,
                                // need to remove manually
            }
            if (!file.open(QIODevice::WriteOnly))
                qDebug() << "Unable to open the file specified";
            p.save(&file, "PNG");
    }

    if (typeValS.compare(QLatin1String("signup"), Qt::CaseInsensitive) == 0){
        message["success"] = true;
        this->sendJson(sender,message);
    }
}


QJsonObject Server::checkCredentials(ServerWorker *sender,
                                     const QJsonObject &doc) {
    const QJsonValue user = doc.value(QLatin1String("username"));
    QJsonObject message;
    message["type"] = QStringLiteral("login");

    if (user.isNull() || !user.isString()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Wrong username format");
        return message;
    }
    const QString username = user.toString().simplified();
    if (username.isEmpty()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Empty email");
        return message;
    }

    for (ServerWorker *client : m_clients) {
        if (client->getUsername() == username) {
            message["success"] = false;
            message["reason"] = QStringLiteral("Already connected "
                                               "from another device");
            return message;
        }
    }

    const QJsonValue pass = doc.value(QLatin1String("password"));
    if (pass.isNull() || !pass.isString()) {
        message["success"] = false;
        message["reason"] = QStringLiteral("Wrong password format");
        return message;
    }
    const QString password = pass.toString().simplified();
    if (password.isEmpty()) {
        message["success"] = false;
        message["reason"] = QStringLiteral("Empty password");
        return message;
    }
    QString nickname;
    int r = this->db.login(username,password,nickname);
    if (r == SUCCESS) {
        message["success"] = true;
        message["username"]=username;
        message["nickname"]=nickname;
        sender->setUsername(username);
        sender->setNickname(nickname);
        return message;
    } else if (r == NON_EXISTING_USER) {
        message["success"] = false;
        message["reason"] = QStringLiteral("Invalid username and/or password");
        return message;
    } else if (r == WRONG_PASSWORD) {
        message["success"] = false;
        message["reason"] = QStringLiteral("Invalid username and/or password");
        return message;
    } else {  // QUERY_ERROR or CONNECTION_ERROR
        message["success"] = false;
        message["reason"] = QStringLiteral("Database error");
        return message;
    }
}



void Server::jsonFromLoggedIn(ServerWorker *sender, const QJsonObject &docObj)
{
    const QJsonValue typeVal = docObj.value(QLatin1String("type"));
    if (typeVal.isNull() || !typeVal.isString())
        return;

    if (typeVal.toString().compare(QLatin1String("nickname"),
                                   Qt::CaseInsensitive) == 0){
        QJsonObject message=this->updateNick(sender,docObj);
        this->sendJson(sender,message);
    }
    if (typeVal.toString().compare(QLatin1String("password"),
                                   Qt::CaseInsensitive) == 0){
        QJsonObject message=this->updatePass(docObj);
        this->sendJson(sender,message);
    }
    if (typeVal.toString().compare(QLatin1String("check_old_password"),
                                   Qt::CaseInsensitive) == 0){
        QJsonObject message = this->checkOldPass(docObj);
        this->sendJson(sender,message);
    }

    if (typeVal.toString().compare(QLatin1String("list_files"),
                                   Qt::CaseInsensitive) == 0){
        QJsonObject message = this->getFiles(docObj, false);
        this->sendJson(sender,message);
    }

    if (typeVal.toString().compare(QLatin1String("list_shared_files"),
                                   Qt::CaseInsensitive) == 0){
        QJsonObject message = this->getFiles(docObj, true);
        this->sendJson(sender,message);
    }

    if (typeVal.toString().compare(QLatin1String("operation"),
                                   Qt::CaseInsensitive) == 0){

        int operation_type = docObj["operation_type"].toInt();

        // Update symbols in server memory
        // and broadcast operation to other editors
        if (operation_type == INSERT) {
            QJsonObject symbol = docObj["symbol"].toObject();
            QString position = fromJsonArraytoString(symbol["position"].toArray());
            symbols_list.value(sender->getFilename())->insert(position, symbol);
        } else if (operation_type == DELETE) {
            QJsonArray symbols = docObj["symbols"].toArray();
            for (int i = 0; i < symbols.size(); i++) {
                QJsonObject symbol = symbols[i].toObject();
                QString position = fromJsonArraytoString(symbol["position"].toArray());
                symbols_list.value(sender->getFilename())->remove(position);
            }
        } else if (operation_type == CHANGE) {
            QJsonObject symbol = docObj["symbol"].toObject();
            QString position = fromJsonArraytoString(symbol["position"].toArray());
            symbols_list.value(sender->getFilename())->insert(position, symbol);
        } else if (operation_type == ALIGN) {
            QJsonObject symbol = docObj["symbol"].toObject();
            QString position = fromJsonArraytoString(symbol["position"].toArray());
            symbols_list.value(sender->getFilename())->insert(position, symbol);
        } else if (operation_type == PASTE) {
            QJsonArray symbols = docObj["symbols"].toArray();
            for (int i = 0; i < symbols.size(); i++) {
                QJsonObject symbol = symbols[i].toObject();
                QString position = fromJsonArraytoString(symbol["position"].toArray());
                symbols_list.value(sender->getFilename())->insert(position, symbol);
            }
        }

        changed.insert(sender->getFilename(), true);
        broadcast(docObj, sender);
    }

    if (typeVal.toString().compare(QLatin1String("new_file"),
                                   Qt::CaseInsensitive) == 0) {
        QJsonObject message = this->createNewFile(docObj,sender);
        this->sendJson(sender,message);
    }
    if (typeVal.toString().compare(QLatin1String("file_to_open"),
                                   Qt::CaseInsensitive) == 0) {
        // Send symbols in file to client
        QVector<QByteArray> v;
        QJsonObject message = this->sendFile(docObj,sender,v);
        //QByteArray toSend = this->createByteArrayJsonImage(message,v);

        //this->sendByteArray(sender,toSend);


    }
    if (typeVal.toString().compare(QLatin1String("close"),
                                   Qt::CaseInsensitive) == 0){
        QJsonObject message = this->closeFile(docObj,sender);
        this->sendJson(sender,message);
    }
    if (typeVal.toString().compare(QLatin1String("filename_from_sharedLink"),
                                   Qt::CaseInsensitive) == 0){
        QJsonObject message = this->getFilenameFromSharedLink(
                    docObj, sender->getUsername()
        );
        this->sendJson(sender,message);
    }
}

QJsonObject Server::updateNick(ServerWorker *sender,const QJsonObject &doc) {
    const QJsonValue user = doc.value(QLatin1String("username"));
    QJsonObject message;
    message["type"] = QStringLiteral("nickname");

    if (user.isNull() || !user.isString()) {
        message["success"] = false;
        message["reason"] = QStringLiteral("Wrong username format");
        return message;
    }
    const QString username = user.toString().simplified();
    if (username.isEmpty()) {
        message["success"] = false;
        message["reason"] = QStringLiteral("Empty email");
        return message;
    }
    const QJsonValue nick = doc.value(QLatin1String("nickname"));
    if (nick.isNull() || !nick.isString()) {
        message["success"] = false;
        message["reason"] = QStringLiteral("Wrong nickname format");
        return message;
    }
    const QString nickname = nick.toString().simplified();
    if (nickname.isEmpty()) {
        message["success"] = false;
        message["reason"] = QStringLiteral("Empty nickname");
        return message;
    }
    sender->setNickname(nickname);
    DatabaseError result = this->db.updateNickname(username,nickname);
    if (result == CONNECTION_ERROR || result == QUERY_ERROR) {
        message["success"] = false;
        message["reason"] = QStringLiteral("Database error");
        return message;
    }
    if (result == NON_EXISTING_USER) {
        message["success"] = false;
        message["reason"] = QStringLiteral("The username doens't exists");
        return message;
    }

    message["success"] = true;
    return message;
}


QJsonObject Server::updatePass(const QJsonObject &doc) {
    const QJsonValue user = doc.value(QLatin1String("username"));
    QJsonObject message;
    message["type"] = QStringLiteral("password");

    if (user.isNull() || !user.isString()) {
        message["success"] = false;
        message["reason"] = QStringLiteral("Wrong username format");
        return message;
    }
    const QString username = user.toString().simplified();
    if (username.isEmpty()) {
        message["success"] = false;
        message["reason"] = QStringLiteral("Empty email");
        return message;
    }

    const QJsonValue oldpass = doc.value(QLatin1String("oldpass"));
    if (oldpass.isNull() || !oldpass.isString()) {
        message["success"] = false;
        message["reason"] = QStringLiteral("Wrong old password format");
        return message;
    }
    const QString oldpassword = oldpass.toString().simplified();
    if (oldpassword.isEmpty()) {
        message["success"] = false;
        message["reason"] = QStringLiteral("Empty old password");
        return message;
    }

    const QJsonValue newpass = doc.value(QLatin1String("newpass"));
    if (newpass.isNull() || !newpass.isString()) {
        message["success"] = false;
        message["reason"] = QStringLiteral("Wrong new password format");
        return message;
    }
    const QString newpassword = newpass.toString().simplified();
    if (newpassword.isEmpty()) {
        message["success"] = false;
        message["reason"] = QStringLiteral("Empty new password");
        return message;
    }

    DatabaseError result = this->db.updatePassword(username,
                                                   oldpassword, newpassword);

    if (result == NON_EXISTING_USER) {
        message["success"] = false;
        message["reason"] = QStringLiteral("No account found "
                                           "for this username");
        return message;
    } else if (result == WRONG_PASSWORD) {
        message["success"] = false;
        message["reason"] = QStringLiteral("Wrong password");
        return message;
    } else {
        message["success"] = true;
        return message;
    }
}

QJsonObject Server::checkOldPass(const QJsonObject &doc){
    QJsonObject message;
    message["type"] = QStringLiteral("old_password_checked");

    const QJsonValue user = doc.value(QLatin1String("username"));
    if (user.isNull() || !user.isString()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Wrong username format");
        return message;
    }
    const QString username = user.toString().simplified();
    if (username.isEmpty()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Empty email");
        return message;
    }

    const QJsonValue old_pass = doc.value(QLatin1String("old_password"));
    if (old_pass.isNull() || !old_pass.isString()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Wrong password format");
        return message;
    }
    const QString old_password = old_pass.toString().simplified();
    if (old_password.isEmpty()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Empty password");
        return message;
    }

    DatabaseError result = this->db.checkOldPassword(username, old_password);
    if (result == CONNECTION_ERROR || result == QUERY_ERROR){
        message["success"] = false;
        message["reason"] = QStringLiteral("Database error");
        return message;
    } else if (result == NON_EXISTING_USER){
        message["success"] = false;
        message["reason"] = QStringLiteral("The username doens't exists");
        return message;
    } else if (result == WRONG_PASSWORD) {
        message["success"] = false;
        message["reason"] = QStringLiteral("Wrong password");
        return message;
    }

    message["success"] = true;
    return message;
}


QJsonObject Server::getFiles(const QJsonObject &doc, bool shared) {
    const QJsonValue user = doc.value(QLatin1String("username"));
    QJsonObject message;
    message["type"] = QStringLiteral("list_files");

    if (user.isNull() || !user.isString()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Wrong username format");
        return message;
    }
    const QString username = user.toString().simplified();
    if (username.isEmpty()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Empty username");
        return message;
    }

    QVector<QPair<QString, QString>> files;
    DatabaseError result = this->db.getFiles(username, files, shared);
    if (result == CONNECTION_ERROR || result == QUERY_ERROR){
        message["success"] = false;
        message["reason"] = QStringLiteral("Database error.");
        return message;
    }
    if (result == NO_FILES_AVAILABLE){
        message["success"] = false;
        message["reason"] = QStringLiteral("You don't have files yet.");
        return message;
    }

    QJsonArray array_files;

    QVector<QPair<QString, QString>>::iterator i;
    for (i = files.begin(); i != files.end(); ++i){
        // Use initializer list to construct QJsonObject
        auto data = QJsonObject({
                        qMakePair(QString("name"), QJsonValue(i->first)),
                        qMakePair(QString("owner"), QJsonValue(i->second))
        });

        array_files.push_back(QJsonValue(data));
    }

    message["shared"] = shared;
    message["success"] = true;
    message["files"]=array_files;
    return message;
}

QJsonObject Server::getFilenameFromSharedLink(const QJsonObject& doc,
                                              const QString& user) {
    QJsonObject message;
    message["type"] = QStringLiteral("filename_from_sharedLink");

    const QJsonValue sharedLink_json = doc.value(QLatin1String("sharedLink"));
    if (sharedLink_json.isNull() || !sharedLink_json.isString()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Wrong shared link format");
        return message;
    }
    const QString sharedLink = sharedLink_json.toString().simplified();
    if (sharedLink.isEmpty()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Empty shared link");
        return message;
    }

    QString filename;
    DatabaseError result = this->db.getFilenameFromSharedLink(sharedLink,
                                                              filename, user);
    if (result == CONNECTION_ERROR || result == QUERY_ERROR){
        message["success"] = false;
        message["reason"] = QStringLiteral("Database error.");
        return message;
    }
    if (result == NON_EXISTING_FILE){
        message["success"] = false;
        message["reason"] = QStringLiteral("No file corresponding "
                                           "to shared link.");
        return message;
    }

    message["success"] = true;
    message["filename"] = filename;
    return message;
}

QJsonObject Server::createNewFile(const QJsonObject &doc, ServerWorker *sender)
{
    QJsonObject message;
    message["type"] = QStringLiteral("new_file");

    const QJsonValue user = doc.value(QLatin1String("author"));
    if (user.isNull() || !user.isString()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Wrong username format");
        return message;
    }
    const QString username = user.toString().simplified();
    if (username.isEmpty()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Empty username");
        return message;
    }

    const QJsonValue name = doc.value(QLatin1String("filename"));
    if (name.isNull() || !name.isString()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Wrong filename format");
        return message;
    }
    const QString filename = name.toString().simplified();
    if (filename.isEmpty()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Empty old password");
        return message;
    }

    QString sharedLink;
    DatabaseError result = this->db.newFile(username, filename, sharedLink);
    if (result == CONNECTION_ERROR || result == QUERY_ERROR){
        message["success"] = false;
        message["reason"] = QStringLiteral("Database error.");
        return message;
    }
    if (result == ALREADY_EXISTING_FILE){
        message["success"] = false;
        message["reason"] = QStringLiteral("This file already exists. "
                                           "Please enter a new filename.");
        return message;
    }

    // Save the name of the file used by the worker
    sender->setFilename(filename + "," + username);

    // Create empty file for the specified user
    if (!db.insertNewFile(filename)) {
       throw std::runtime_error("File shouldn't already exist.");
    }

    // Add current worker to the map <file, list_of_workers>
    QList<ServerWorker*>* list=new QList<ServerWorker*>();
    list->append(sender);
    mapFileWorkers->insert(filename + "," + username,list);

    if (!symbols_list.contains(sender->getFilename())) {
        symbols_list.insert(sender->getFilename(),
                            new QMap<QString,QJsonObject>());
        changed.insert(sender->getFilename(), true);
    }

    message["success"] = true;
    message["shared_link"] = sharedLink;
    return message;
}

void Server::storeSymbolsServerMemory(ServerWorker* sender,QList<QJsonObject> array){
    // Store symbols in server memory
    foreach (const QJsonValue & symbol, array) {
        QString position = fromJsonArraytoString(symbol["position"].toArray());
        if (!symbols_list.contains(sender->getFilename())) {
            symbols_list.insert(sender->getFilename(),
                                new QMap<QString,QJsonObject>());
        }
        symbols_list.value(sender->getFilename())->insert(position, symbol.toObject());
        changed.insert(sender->getFilename(), false);
    }
}

QJsonObject Server::sendFile(const QJsonObject &doc, ServerWorker *sender,
                             QVector<QByteArray> &v){

    QJsonObject message;
    message["type"] = QStringLiteral("file_to_open");

    const QJsonValue name = doc.value(QLatin1String("filename"));
    if (name.isNull() || !name.isString()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Wrong filename format");
        QByteArray toSend = this->createByteArrayJsonImage(message,v);
        this->sendByteArray(sender,toSend);
        return message;
    }
    const QString filename = name.toString().simplified();
    if (filename.isEmpty()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Empty filename");
        QByteArray toSend = this->createByteArrayJsonImage(message,v);
        this->sendByteArray(sender,toSend);
        return message;
    }

    int pos = filename.lastIndexOf(QChar(','));
    QString file=filename.left(pos);
    QString author = filename.right(filename.length() - pos - 1);

    sender->setFilename(filename);
    int index = 0;

    if (mapFileWorkers->contains(filename)) {
        index = mapFileWorkers->value(filename)->size();
        mapFileWorkers->value(filename)->append(sender);
    } else {
        QList<ServerWorker*>* list=new QList<ServerWorker*>();
        list->append(sender);
        mapFileWorkers->insert(filename,list);
    }

    QList<ServerWorker*>* list=mapFileWorkers->value(filename);
    QJsonArray array_users;

    // TODO: per ora manda il contenuto del file insieme alla lista di chi è connesso;
    //d agestire il caso in cui le connessioni cambiano mentre o dopo il messaggio è inviato

    QString image_path;

    for (int i = 0; i <list->count(); i++){
        // Use initializer list to construct QJsonObject
        if (sender->getUsername()==list->at(i)->getUsername()) {
            continue;
        }
        auto data = QJsonObject({
                qMakePair(QString("username"),
                          QJsonValue(list->at(i)->getUsername())),
                qMakePair(QString("nickname"),
                          QJsonValue(list->at(i)->getNickname()))
        });

        array_users.push_back(QJsonValue(data));

        image_path = QDir::currentPath() + IMAGES_PATH + "/"
                     + QJsonValue(list->at(i)->getUsername()).toString()
                     + ".png";

        QFileInfo file(image_path);
        if (file.exists()) {
            QImage p(image_path);
            QByteArray bArray;
            QBuffer buffer(&bArray);
            buffer.open(QIODevice::WriteOnly);
            p.save(&buffer, "PNG");
            v.append(bArray);
        }
        else{
            QByteArray bArray;
            v.append(bArray);
        }
    }

    int start=0;
    bool cont=false;
    QJsonArray symbols;
    bool success = true;
    QList<QJsonObject> l;
    if (symbols_list.contains(filename))
        l=symbols_list.value(filename)->values();
    else{
        success = db.retrieveFile(filename,l);
        storeSymbolsServerMemory(sender,l);
    }

    // Retrieve shared link
    QString sharedLink;
    db.getSharedLink(author, file, sharedLink);

    int tot_symbols=l.size();
    qDebug()<<"To send: "<<tot_symbols<<" symbols";
    for (int i=start;i<l.size();i++){
        int old_size=symbols.size();
        QJsonObject o=l[i];
        symbols.append(o);
        int size=QJsonDocument(o).toJson().size();
        if (symbols.size()==old_size){
            start=i;
            cont=true;
            break;
            }
    }





    if (success == true) {
            message["success"] = true;
            message["content"] = symbols;
            message["filename"]= filename;
            message["tot_symbols"]=tot_symbols;
            message["info"]=true;
            message["users"] = array_users;
            message["shared_link"] = sharedLink;
            /*		message["color"] = color;*/
        } else {
            message["success"] = false;
            message["reason"] = QStringLiteral("File content "
                                               "different form json array");
        }


    qDebug()<<"start "<<start;

    QByteArray toSend = this->createByteArrayJsonImage(message,v);
    this->sendByteArray(sender,toSend);
    qDebug()<<"First sent: sending "<<symbols.size()<<" symbols\n";


    while (cont && success){
        qDebug()<<"HERE";
        symbols=QJsonArray();
        message=QJsonObject();
        message["type"] = QStringLiteral("file_to_open");
        message["success"] = true;
        message["filename"]= filename;
        message["tot_symbols"]=tot_symbols;

        //continua a mandare come fosse un file normale
        int i;
        for (i=start;i<symbols_list.value(filename)->values().size();i++){
            int old_size=symbols.size();
            QJsonObject o=l[i];
            symbols.append(o);
            int size=QJsonDocument(o).toJson().size();
            if (symbols.size()==old_size){
                start=i;
                cont=true;
                break;
                }
           }
        if (i==symbols_list.value(filename)->values().size())
            cont=false;
       message["content"] = symbols;
       QVector<QByteArray> tmp;
       QByteArray toSend = this->createByteArrayJsonImage(message,tmp);
       this->sendByteArray(sender,toSend);
       qDebug()<<"Another sent: sending "<<symbols.size()<<" symbols\n";
    }



    // Inform all the connected clients of the new connection
    QJsonObject message_broadcast;
    message_broadcast["type"] = QStringLiteral("connection");
    message_broadcast["filename"]=filename;
    message_broadcast["username"]= sender->getUsername();
    message_broadcast["nickname"]= sender->getNickname();

    image_path = QDir::currentPath() + IMAGES_PATH + "/"
                 + sender->getUsername() + ".png";

    QFileInfo fileImage(image_path);
    QByteArray bArray;
    if (fileImage.exists()) {
        QImage p(image_path);
        QBuffer buffer(&bArray);
        buffer.open(QIODevice::WriteOnly);
        p.save(&buffer, "PNG");
    }

    this->broadcastByteArray(message_broadcast,bArray,sender);
    return message;
}

bool Server::udpateSymbolListAndCommunicateDisconnection(QString filename,
                                                         ServerWorker* sender){
    // Remove client from list of clients using current file
    if (mapFileWorkers->contains(filename)){
        if (!mapFileWorkers->value(filename)->removeOne(sender))
            return false;
    } else {
        return false;
    }

    // If the only client using the document is the one disconnecting
    if (mapFileWorkers->value(filename)->isEmpty()) {
        // Remove file from memory
        delete mapFileWorkers->value(filename);
        mapFileWorkers->remove(filename);

        // Save file on disk to avoid missing the last changes
        this->saveFile();

        // Empty symbol list
        symbols_list.remove(sender->getFilename());
        changed.remove(sender->getFilename());
    } else {
        QJsonObject message_broadcast;
        message_broadcast["type"] = QStringLiteral("disconnection");
        message_broadcast["filename"]=filename;
        message_broadcast["user"]=sender->getUsername();
        message_broadcast["nickname"]=sender->getNickname();
        this->broadcast(message_broadcast,sender);
    }

    return true;
}

QJsonObject Server::closeFile(const QJsonObject &doc, ServerWorker *sender){

    QJsonObject message;
    message["type"] = QStringLiteral("close");

    const QJsonValue name = doc.value(QLatin1String("filename"));
    if (name.isNull() || !name.isString()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Wrong filename format");
        return message;
    }
    const QString filename = name.toString().simplified();
    if (filename.isEmpty()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Empty old password");
        return message;
    }

    const QJsonValue user = doc.value(QLatin1String("username"));

    if (user.isNull() || !user.isString()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Wrong username format");
        return message;
    }
    const QString username = user.toString().simplified();
    if (username.isEmpty()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Empty username");
        return message;
    }

    const QJsonValue nick = doc.value(QLatin1String("nickname"));

    if (nick.isNull() || !nick.isString()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Wrong nickname format");
        return message;
    }
    const QString nickname = nick.toString().simplified();
    if (nickname.isEmpty()){
        message["success"] = false;
        message["reason"] = QStringLiteral("Empty nickname");
        return message;
    }

    if (!udpateSymbolListAndCommunicateDisconnection(filename,sender)){
        message["success"] = false;
        message["reason"] = QStringLiteral("File not exist");
        return message;
    }
    //forse questo si deve mettere fuori
    sender->closeFile();

    message["success"] = true;
    return message;
}

QString Server::fromJsonArraytoString(const QJsonArray& data) {
    QJsonDocument doc;
    doc.setArray(data);
    QString str(doc.toJson());
    return str;
}

void Server::saveFile() {
    for (QString filename : symbols_list.keys()) {
        if (changed.value(filename) == true) {
            // Save from main memory to QVector
            QVector<QByteArray> qvector;
            for (QJsonObject symbol : symbols_list.value(filename)->values()) {
                qvector.push_back(QJsonDocument(symbol).toJson());
            }

            // Convert QVector to QByteArray
            QByteArray data;
            QDataStream stream(&data, QIODevice::WriteOnly);
            stream << qvector;

            // Save binary file into db
            db.saveFile(filename, data);
        }
        // Reset value to false
        changed.insert(filename, false);
    }
}
