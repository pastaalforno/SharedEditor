#include "serverwindow.h"
#include <QApplication>
#include <QDebug>
#include <stdlib.h>
#include "mongo.h"

#define PORT 1500

int main(int argc, char *argv[])
{
//	Mongo db;
//	qDebug() << db.signup("enrico@gmail.com", "Admin11.");
//	QString nick;
//	qDebug() << db.login("enrico@gmail.com", "Amin11.", nick);
//	qDebug() << nick;

	QApplication a(argc, argv);
	quint16 port = PORT;

	QStringList args = QCoreApplication::arguments();
	if (args.length() != 1 && args.length() != 2) {
		qDebug() << "Usage: ./Server <port_number>";
		qDebug() << "If no argument provided port 1500 is used.";
		exit(-1);
	}

	if (args.length() == 2) {
		bool ok;
		unsigned int tmp = args.at(1).toUInt(&ok);
		if (ok) {
			port = tmp;
		}
	}

	ServerWindow w(nullptr, port);
	w.show();
	return a.exec();
}
