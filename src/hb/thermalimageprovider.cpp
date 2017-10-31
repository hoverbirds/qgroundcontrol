#include "thermalimageprovider.h"

LEPTONImageProvider::LEPTONImageProvider(QNetworkAccessManager *nam, QObject *parent) :
    QObject(parent), QQuickImageProvider(QQuickImageProvider::Pixmap)
{
    m_mjpegGrabber = 0;
    m_nam = nam;
}

void LEPTONImageProvider::onNewPixmap(const QPixmap &pixmap)
{
    m_currentPixmap = pixmap;
//    qDebug() << "new thermal image";
    Q_EMIT refresh();
}

void LEPTONImageProvider::onNewThermalUrl(const QUrl &url)
{
    if (m_mjpegGrabber) {
        m_mjpegGrabber->deleteLater();
        m_mjpegGrabber = 0;
    }
    m_camUrl = url;
    if (m_camUrl.isEmpty()) {
        onNewPixmap(QPixmap());
        return;
    }

    qDebug() << "delay request to the thermal server for it to start properly";
    QTimer::singleShot(2000, this, SLOT(startImageGrabber()));
}

bool LEPTONImageProvider::hasThermalData()
{
    return !m_camUrl.isEmpty();
}

QPixmap	LEPTONImageProvider::requestPixmap(const QString & id, QSize * size, const QSize & requestedSize)
{
    Q_UNUSED(id)
    Q_UNUSED(requestedSize)

    *size = m_currentPixmap.size();
    return m_currentPixmap;
}


void LEPTONImageProvider::startImageGrabber()
{
    qDebug() << "starting thermal image grabber";
    m_mjpegGrabber = new MJPEGImageGrabber(m_nam, m_camUrl, this);
    connect(m_mjpegGrabber, SIGNAL(newFrame(QPixmap)),
            this, SLOT(onNewPixmap(QPixmap)));
}

