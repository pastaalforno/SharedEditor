#ifndef LOGIN_H
#define LOGIN_H

#include <QMainWindow>
#include "home.h"
#include "client.h"

namespace Ui {
class Login;
}

class Login : public QWidget
{
    Q_OBJECT

public:
    Login(QWidget *parent = 0,Client* client=0);
    void setClient(Client* client);
    ~Login();

private slots:
    void on_pushButtonLogin_clicked();

    void on_pushButtonNewAccount_clicked();

    void on_lineEditUsername_editingFinished();

    void on_lineEditUsername_textChanged(const QString &arg1);

    void on_lineEditPassword_textChanged(const QString &arg1);

    void connectedToServer();
    void attemptLogin(const QString &username, const QString &password);
    void loggedIn();
    void loginFailed(const QString &reason);
//    void messageReceived(const QString &sender, const QString &text);
//    void sendMessage();
//    void disconnectedFromServer();
//    void userJoined(const QString &username);
//    void userLeft(const QString &username);
    void error(QAbstractSocket::SocketError socketError);

public slots:
    void disconnect();

signals:
    void access(int i);

private:
    Ui::Login *ui;
    Client *client;
};

#endif // LOGIN_H
