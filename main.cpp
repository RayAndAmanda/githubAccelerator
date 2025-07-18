#include "mainwindow.h"
#include<QObject>
#include <QApplication>
#include <QWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QTextEdit>
#include <QTcpSocket>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>
#include <QMap>
#include <QStringList>
#include <QLabel>
#include <QMessageBox>
#include <QScrollBar>
#include <QTimer>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

struct IPResult {
    QString ip;
    qint64 latency;
};

qint64 testLatency(const QString& ip, int port = 443, int timeoutMs = 1000) {
    QTcpSocket socket;
    QElapsedTimer timer;
    timer.start();
    socket.connectToHost(ip, port);
    if (socket.waitForConnected(timeoutMs)) {
        return timer.elapsed();
    }
    return -1;
}

bool writeHostsEntries(const QMap<QString, QString>& domainIpMap) {
    QString hostsPath;
#ifdef Q_OS_WIN
    hostsPath = "C:/Windows/System32/drivers/etc/hosts";
#else
    hostsPath = "/etc/hosts";
#endif
    QFile file(hostsPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;

    QString content;
    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine();
        bool keep = true;
        for (const QString& domain : domainIpMap.keys()) {
            if (line.contains(domain)) {
                keep = false;
                break;
            }
        }
        if (keep) content += line + "\n";
    }
    file.close();

    for (auto it = domainIpMap.constBegin(); it != domainIpMap.constEnd(); ++it) {
        content += QString("%1 %2\n").arg(it.value(), it.key());
    }

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) return false;

    QTextStream out(&file);
    out << content;
    file.close();
    return true;
}

class AcceleratorApp : public QWidget {
    Q_OBJECT
public:
    AcceleratorApp() {
        setWindowTitle("GitHub 加速器 Qt GUI");
        resize(600, 400);

        QVBoxLayout *layout = new QVBoxLayout(this);
        startBtn = new QPushButton("手动测速并更新 hosts");
        logEdit = new QTextEdit();
        logEdit->setReadOnly(true);
        logEdit->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(startBtn);
        layout->addWidget(logEdit);

        tray = new QSystemTrayIcon(QIcon::fromTheme("network-server"), this);
        trayMenu = new QMenu(this);
        QAction *exitAction = new QAction("退出", this);
        trayMenu->addAction(exitAction);
        tray->setContextMenu(trayMenu);
        tray->show();

        connect(startBtn, &QPushButton::clicked, this, &AcceleratorApp::performCheck);
        connect(exitAction, &QAction::triggered, qApp, &QApplication::quit);

        timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &AcceleratorApp::performCheck);
        timer->start(6 * 60 * 60 * 1000); // 每6小时自动测速

        performCheck(); // 启动时自动测速
    }

private:
    QPushButton *startBtn;
    QTextEdit *logEdit;
    QSystemTrayIcon *tray;
    QMenu *trayMenu;
    QTimer *timer;

    void performCheck() {
        logEdit->append("\n🕒 开始测速...");

        QNetworkAccessManager *manager = new QNetworkAccessManager(this);
        QNetworkRequest request(QUrl("https://raw.githubusercontent.com/521xueweihan/GitHub520/main/hosts.json"));
        QNetworkReply *reply = manager->get(request);

        connect(reply, &QNetworkReply::finished, this, [=]() {
            if (reply->error() != QNetworkReply::NoError) {
                logEdit->append("❌ 无法下载远程 IP 数据源");
                reply->deleteLater();
                return;
            }

            QMap<QString, QStringList> domainCandidates;
            QByteArray data = reply->readAll();
            QJsonDocument doc = QJsonDocument::fromJson(data);
            if (doc.isArray()) {
                // 兼容二维数组格式
                QJsonArray arr = doc.array();
                for (const QJsonValue& item : arr) {
                    if (!item.isArray()) continue;
                    QJsonArray pair = item.toArray();
                    if (pair.size() != 2) continue;
                    QString ip = pair[0].toString();
                    QString domain = pair[1].toString();
                    domainCandidates[domain].append(ip);
                }
            } else if (!doc.isObject()) {
                logEdit->append("❌ 远程 JSON 数据格式错误");
                reply->deleteLater();
                return;
            }else if (doc.isObject()) {

            QJsonObject root = doc.object();
            for (const QString& key : root.keys()) {
                QJsonArray arr = root.value(key).toArray();
                QStringList ipList;
                for (const QJsonValue& val : arr) ipList << val.toString();
                domainCandidates[key] = ipList;
            }

            }
            QMap<QString, QString> bestIPs;
            for (auto it = domainCandidates.constBegin(); it != domainCandidates.constEnd(); ++it) {
                QString domain = it.key();
                QStringList candidates = it.value();
                QList<IPResult> results;
                for (const QString& ip : candidates) {
                    qint64 latency = testLatency(ip);
                    if (latency >= 0) {
                        logEdit->append(QString("[%1] IP %2 延迟: %3ms").arg(domain, ip).arg(latency));
                        results.append({ip, latency});
                    } else {
                        logEdit->append(QString("[%1] IP %2 不可连接").arg(domain, ip));
                    }
                }
                std::sort(results.begin(), results.end(), [](const IPResult& a, const IPResult& b) {
                    return a.latency < b.latency;
                });
                if (!results.isEmpty()) bestIPs[domain] = results.first().ip;
                else logEdit->append(QString("[%1] 无可用 IP").arg(domain));
            }

            if (!bestIPs.isEmpty()) {
                if (writeHostsEntries(bestIPs)) {
                    logEdit->append("\n✅ 成功写入 hosts：");
                    for (auto it = bestIPs.constBegin(); it != bestIPs.constEnd(); ++it) {
                        logEdit->append(QString("%1 => %2").arg(it.key(), it.value()));
                    }
                } else {
                    logEdit->append("❌ 写入 hosts 失败，请使用管理员权限运行。\n");
                    QMessageBox::warning(this, "权限错误", "写入 hosts 失败，请以管理员身份运行本程序。");
                }
            } else {
                logEdit->append("❌ 所有域名均无可用 IP\n");
            }

            logEdit->verticalScrollBar()->setValue(logEdit->verticalScrollBar()->maximum());
            reply->deleteLater();
        });
    }
};

#include "main.moc"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    AcceleratorApp window;
    window.show();
    return a.exec();
}

