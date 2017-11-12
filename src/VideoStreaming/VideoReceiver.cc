/****************************************************************************
 *
 *   (c) 2009-2016 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/


/**
 * @file
 *   @brief QGC Video Receiver
 *   @author Gus Grubba <mavlink@grubba.com>
 */

#include "VideoReceiver.h"
#include "SettingsManager.h"
#include "QGCApplication.h"
#include "VideoManager.h"

#include <QDebug>
#include <QUrl>
#include <QDir>
#include <QDateTime>
#include <QSysInfo>
#include <QThread>

QGC_LOGGING_CATEGORY(VideoReceiverLog, "VideoReceiverLog")

#if defined(QGC_GST_STREAMING)

static const char* kVideoExtensions[] =
{
    "mkv",
    "mov",
    "mp4"
};

static const char* kVideoMuxes[] =
{
    "matroskamux",
    "qtmux",
    "mp4mux"
};

#define NUM_MUXES (sizeof(kVideoMuxes) / sizeof(char*))

#endif


VideoReceiver::VideoReceiver(NodeSelector *piNodeSelector, QObject* parent)
    : QObject(parent)
#if defined(QGC_GST_STREAMING)
    , _running(false)
    , _recording(false)
    , _streaming(false)
    , _starting(false)
    , _stopping(false)
    , _sink(NULL)
    , _tee(NULL)
    , _pipeline(NULL)
    , _pipelineStopRec(NULL)
    , _videoSink(NULL)
    , _socket(NULL)
    , _serverPresent(false)
#endif
    , _videoSurface(NULL)
    , _expectedLatency(20)
    , _videoRunning(false)
    , _showFullScreen(false)
{
    _nodeSelector = piNodeSelector;
    _videoSurface  = new VideoSurface;
#if defined(QGC_GST_STREAMING)
    _setVideoSink(_videoSurface->videoSink());
    _timer.setSingleShot(true);
    connect(&_timer, &QTimer::timeout, this, &VideoReceiver::_timeout);
    connect(this, &VideoReceiver::msgErrorReceived, this, &VideoReceiver::_handleError);
    connect(this, &VideoReceiver::msgEOSReceived, this, &VideoReceiver::_handleEOS);
    connect(this, &VideoReceiver::msgStateChangedReceived, this, &VideoReceiver::_handleStateChanged);
    connect(&_frameTimer, &QTimer::timeout, this, &VideoReceiver::_updateTimer);
//    connect(&_statsTimer, &QTimer::timeout, this, &VideoReceiver::_onStatsTimer);
    _frameTimer.start(1000);
#endif
}

VideoReceiver::~VideoReceiver()
{
#if defined(QGC_GST_STREAMING)
    stop();
    if(_socket) {
        delete _socket;
    }
    if (_videoSink) {
        gst_object_unref(_videoSink);
    }
#endif
    if(_videoSurface)
        delete _videoSurface;
}

#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_setVideoSink(GstElement* sink)
{
    if (_videoSink) {
        gst_object_unref(_videoSink);
        _videoSink = NULL;
    }
    if (sink) {
        _videoSink = sink;
        gst_object_ref_sink(_videoSink);
    }
}
#endif

//-----------------------------------------------------------------------------
void
VideoReceiver::grabImage(QString imageFile)
{
    _imageFile = imageFile;
    emit imageFileChanged();
}

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
static void
newPadCB(GstElement* element, GstPad* pad, gpointer data)
{
    gchar* name;
    name = gst_pad_get_name(pad);
    //g_print("A new pad %s was created\n", name);
    GstCaps* p_caps = gst_pad_get_pad_template_caps (pad);
    gchar* description = gst_caps_to_string(p_caps);
    qCDebug(VideoReceiverLog) << p_caps << ", " << description;
    g_free(description);
    GstElement* sink = GST_ELEMENT(data);
    if(gst_element_link_pads(element, name, sink, "sink") == false)
        qCritical() << "newPadCB : failed to link elements\n";
    g_free(name);
}
#endif

void VideoReceiver::next()
{
    _nodeSelector->selectNext();
//    start();
}

void VideoReceiver::previous()
{
    _nodeSelector->selectPrevious();
//    start();
}

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_connected()
{
    //-- Server showed up. Now we start the stream.
    _timer.stop();
    _socket->deleteLater();
    _socket = NULL;
    _serverPresent = true;
    start();
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_socketError(QAbstractSocket::SocketError socketError)
{
    Q_UNUSED(socketError);
    _socket->deleteLater();
    _socket = NULL;
    //-- Try again in 5 seconds
    _timer.start(5000);
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_timeout()
{
    //-- If socket is live, we got no connection nor a socket error
    if(_socket) {
        delete _socket;
        _socket = NULL;
    }
    //-- RTSP will try to connect to the server. If it cannot connect,
    //   it will simply give up and never try again. Instead, we keep
    //   attempting a connection on this timer. Once a connection is
    //   found to be working, only then we actually start the stream.
    QUrl url(_uri);
    _socket = new QTcpSocket;
    _socket->setProxy(QNetworkProxy::NoProxy);
    connect(_socket, static_cast<void (QTcpSocket::*)(QAbstractSocket::SocketError)>(&QTcpSocket::error), this, &VideoReceiver::_socketError);
    connect(_socket, &QTcpSocket::connected, this, &VideoReceiver::_connected);
    //qCDebug(VideoReceiverLog) << "Trying to connect to:" << url.host() << url.port();
    _socket->connectToHost(url.host(), url.port());
    _timer.start(5000);
}
#endif


void VideoReceiver::delayedStart(const QString &optionsString, bool recording)
{
#if defined(QGC_GST_STREAMING)
    if (!optionsString.isEmpty()) {
        // start new stream
        _nodeSelector->startStreaming(_nodeSelector->currentNode(), optionsString, recording);
        _expectedLatency = _nodeSelector->currentNode().latency;

        QString newUri = QString("udp://0.0.0.0:") + QString::number(_nodeSelector->currentNode().targetStreamingPort);
        qDebug() << newUri;
        setUri(newUri);
    }


    stop();
    start();
//    QTimer::singleShot(50, this, &VideoReceiver::start);
#endif
}

//-----------------------------------------------------------------------------
// When we finish our pipeline will look like this:
//
//                                   +-->queue-->decoder-->_videosink
//                                   |
//    datasource-->demux-->parser-->tee
//
//                                   ^
//                                   |
//                                   +-Here we will later link elements for recording
void VideoReceiver::start()
{
#if defined(QGC_GST_STREAMING)
    qCDebug(VideoReceiverLog) << "start()";

    if (_uri.isEmpty()) {
        qCritical() << "VideoReceiver::start() failed because URI is not specified";
        return;
    }
    if (_videoSink == NULL) {
        qCritical() << "VideoReceiver::start() failed because video sink is not set";
        return;
    }
    if(_running) {
        qCDebug(VideoReceiverLog) << "Already running!";
        return;
    }

    _starting = true;

    bool isUdp  = _uri.contains("udp://");
    bool isRtsp = _uri.contains("rtsp://");
    bool isTCP  = _uri.contains("tcp://");

    //-- For RTSP and TCP, check to see if server is there first
    if(!_serverPresent && (isRtsp || isTCP)) {
        _timer.start(100);
        qDebug() << "rtsp server not preset, not procceding";
        return;
    }

    bool running = false;
    bool pipelineUp = false;

    GstElement*     dataSource  = NULL;
    GstCaps*        caps        = NULL;
    GstElement*     demux       = NULL;
    GstElement*     parser      = NULL;
    GstElement*     queue       = NULL;
    GstElement*     decoder     = NULL;
    GstElement*     queue1      = NULL;
    GstElement*     udpjitter   = NULL;

    do {
        if ((_pipeline = gst_pipeline_new("receiver")) == NULL) {
            qCritical() << "VideoReceiver::start() failed. Error with gst_pipeline_new()";
            break;
        }

        if(isUdp) {
            dataSource = gst_element_factory_make("udpsrc", "udp-source");
        } else if(isTCP) {
            dataSource = gst_element_factory_make("tcpclientsrc", "tcpclient-source");
        } else {
            dataSource = gst_element_factory_make("rtspsrc", "rtsp-source");
        }

        if (!dataSource) {
            qCritical() << "VideoReceiver::start() failed. Error with data source for gst_element_factory_make()";
            break;
        }

        if(isUdp) {
            if ((caps = gst_caps_from_string("application/x-rtp, media=(string)video, clock-rate=(int)90000, encoding-name=(string)H264")) == NULL) {
                qCritical() << "VideoReceiver::start() failed. Error with gst_caps_from_string()";
                break;
            }
            g_object_set(G_OBJECT(dataSource), "uri", qPrintable(_uri), "caps", caps, NULL);
        } else if(isTCP) {
            QUrl url(_uri);
            g_object_set(G_OBJECT(dataSource), "host", qPrintable(url.host()), "port", url.port(), NULL );
        } else {
            g_object_set(G_OBJECT(dataSource), "location", qPrintable(_uri), "latency", 17, "udp-reconnect", 1, "timeout", static_cast<guint64>(5000000), NULL);
        }

        // Currently, we expect H264 when using anything except for TCP.  Long term we may want this to be settable
        if (isTCP) {
            if ((demux = gst_element_factory_make("tsdemux", "mpeg2-ts-demuxer")) == NULL) {
                qCritical() << "VideoReceiver::start() failed. Error with gst_element_factory_make('tsdemux')";
                break;
            }
        } else if (isUdp) {
            if ((udpjitter = gst_element_factory_make("rtpjitterbuffer", "rpp-jitter-buffer")) == NULL) {
                qCritical() << "VideoReceiver::start() failed. Error with gst_element_factory_make('rtpjitterbuffer')";
                break;
            }
            _jitterBuffer = udpjitter;
            _statsTimer.start(1000);
            if ((demux = gst_element_factory_make("rtph264depay", "rtp-h264-depacketizer")) == NULL) {
                qCritical() << "VideoReceiver::start() failed. Error with gst_element_factory_make('rtph264depay')";
                break;
            }

            // set jitter buffers latency to 2 times the initial estimates
            g_object_set(G_OBJECT(udpjitter), "latency", _expectedLatency*2, NULL);
        }

        if ((parser = gst_element_factory_make("h264parse", "h264-parser")) == NULL) {
            qCritical() << "VideoReceiver::start() failed. Error with gst_element_factory_make('h264parse')";
            break;
        }

        if((_tee = gst_element_factory_make("tee", NULL)) == NULL)  {
            qCritical() << "VideoReceiver::start() failed. Error with gst_element_factory_make('tee')";
            break;
        }

        if((queue = gst_element_factory_make("queue", NULL)) == NULL)  {
            // TODO: We may want to add queue2 max-size-buffers=1 to get lower latency
            //       We should compare gstreamer scripts to QGroundControl to determine the need
            qCritical() << "VideoReceiver::start() failed. Error with gst_element_factory_make('queue')";
            break;
        }

        if ((decoder = gst_element_factory_make("avdec_h264", "h264-decoder")) == NULL) {
            qCritical() << "VideoReceiver::start() failed. Error with gst_element_factory_make('avdec_h264')";
            break;
        }

        if ((queue1 = gst_element_factory_make("queue", NULL)) == NULL) {
            qCritical() << "VideoReceiver::start() failed. Error with gst_element_factory_make('queue') [1]";
            break;
        }

        gst_bin_add_many(GST_BIN(_pipeline), dataSource, udpjitter, demux, parser, _tee, queue, decoder, queue1, _videoSink, NULL);
        pipelineUp = true;

        if(isUdp) {
            // Link the pipeline in front of the tee
            if(!gst_element_link_many(dataSource, udpjitter, demux, parser, _tee, queue, decoder, queue1, _videoSink, NULL)) {
                qCritical() << "Unable to link UDP elements.";
                break;
            }
        } else if (isTCP) {
            if(!gst_element_link(dataSource, demux)) {
                qCritical() << "Unable to link TCP dataSource to Demux.";
                break;
            }
            if(!gst_element_link_many(parser, _tee, queue, decoder, queue1, _videoSink, NULL)) {
                qCritical() << "Unable to link TCP pipline to parser.";
                break;
            }
            g_signal_connect(demux, "pad-added", G_CALLBACK(newPadCB), parser);
        } else {
            g_signal_connect(dataSource, "pad-added", G_CALLBACK(newPadCB), demux);
            if(!gst_element_link_many(demux, parser, _tee, queue, decoder, _videoSink, NULL)) {
                qCritical() << "Unable to link RTSP elements.";
                break;
            }
        }

        dataSource = demux = parser = queue = decoder = queue1 = NULL;

        GstBus* bus = NULL;

        if ((bus = gst_pipeline_get_bus(GST_PIPELINE(_pipeline))) != NULL) {
            gst_bus_enable_sync_message_emission(bus);
            g_signal_connect(bus, "sync-message", G_CALLBACK(_onBusMessage), this);
            gst_object_unref(bus);
            bus = NULL;
        }

        running = gst_element_set_state(_pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;

    } while(0);

    if (caps != NULL) {
        gst_caps_unref(caps);
        caps = NULL;
    }

    if (!running) {
        qCritical() << "VideoReceiver::start() failed";

        // In newer versions, the pipeline will clean up all references that are added to it
        if (_pipeline != NULL) {
            gst_object_unref(_pipeline);
            _pipeline = NULL;
        }

        // If we failed before adding items to the pipeline, then clean up
        if (!pipelineUp) {
            if (decoder != NULL) {
                gst_object_unref(decoder);
                decoder = NULL;
            }

            if (parser != NULL) {
                gst_object_unref(parser);
                parser = NULL;
            }

            if (demux != NULL) {
                gst_object_unref(demux);
                demux = NULL;
            }

            if (dataSource != NULL) {
                gst_object_unref(dataSource);
                dataSource = NULL;
            }

            if (_tee != NULL) {
                gst_object_unref(_tee);
                dataSource = NULL;
            }

            if (queue != NULL) {
                gst_object_unref(queue);
                dataSource = NULL;
            }
        }

        _running = false;
    } else {
        _running = true;
        qCDebug(VideoReceiverLog) << "Running";
    }
    _starting = false;
#endif

    qDebug("done with actual start");
}

//-----------------------------------------------------------------------------
void
VideoReceiver::stop()
{
#if defined(QGC_GST_STREAMING)
    qCDebug(VideoReceiverLog) << "stop()";
    if(!_streaming) {
        _shutdownPipeline();
    } else if (_pipeline != NULL && !_stopping) {
        qCDebug(VideoReceiverLog) << "Stopping _pipeline";
        gst_element_send_event(_pipeline, gst_event_new_eos());
        _stopping = true;
        GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(_pipeline));
        GstMessage* message = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, (GstMessageType)(GST_MESSAGE_EOS|GST_MESSAGE_ERROR));
        gst_object_unref(bus);
        if(GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            _shutdownPipeline();
            qCritical() << "Error stopping pipeline!";
        } else if(GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
            _handleEOS();
        }
        gst_message_unref(message);
    }
#endif
}

//-----------------------------------------------------------------------------
void
VideoReceiver::setUri(const QString & uri)
{
    _uri = uri;
}

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_shutdownPipeline() {
    if(!_pipeline) {
        qCDebug(VideoReceiverLog) << "No pipeline";
        return;
    }
    GstBus* bus = NULL;
    if ((bus = gst_pipeline_get_bus(GST_PIPELINE(_pipeline))) != NULL) {
        gst_bus_disable_sync_message_emission(bus);
        gst_object_unref(bus);
        bus = NULL;
    }
    gst_element_set_state(_pipeline, GST_STATE_NULL);
    gst_bin_remove(GST_BIN(_pipeline), _videoSink);
    gst_object_unref(_pipeline);
    _pipeline = NULL;
    delete _sink;
    _sink = NULL;
    _serverPresent = false;
    _streaming = false;
    _recording = false;
    _stopping = false;
    _running = false;

    _statsTimer.stop();
    _jitterBuffer = NULL;

    emit recordingChanged();
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_handleError() {
    qCDebug(VideoReceiverLog) << "Gstreamer error!";
    _shutdownPipeline();
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_handleEOS() {
    if(_stopping) {
        _shutdownPipeline();
        qCDebug(VideoReceiverLog) << "Stopped";
    } else if(_recording && _sink->removing) {
        _shutdownRecordingBranch();
    } else {
        qWarning() << "VideoReceiver: Unexpected EOS!";
        _shutdownPipeline();
    }
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_handleStateChanged() {
    if(_pipeline) {
        _streaming = GST_STATE(_pipeline) == GST_STATE_PLAYING;
        qCDebug(VideoReceiverLog) << "State changed, _streaming:" << _streaming;
    }
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
gboolean
VideoReceiver::_onBusMessage(GstBus* bus, GstMessage* msg, gpointer data)
{
    Q_UNUSED(bus)
    Q_ASSERT(msg != NULL && data != NULL);
    VideoReceiver* pThis = (VideoReceiver*)data;

    switch(GST_MESSAGE_TYPE(msg)) {
    case(GST_MESSAGE_ERROR): {
        gchar* debug;
        GError* error;
        gst_message_parse_error(msg, &error, &debug);
        g_free(debug);
        qCritical() << error->message;
        g_error_free(error);
        pThis->msgErrorReceived();
    }
        break;
    case(GST_MESSAGE_EOS):
        pThis->msgEOSReceived();
        break;
    case(GST_MESSAGE_STATE_CHANGED):
        pThis->msgStateChangedReceived();
        break;
    default:
        break;
    }

    return TRUE;
}
#endif

#if defined(QGC_GST_STREAMING)
void VideoReceiver::_onStatsTimer()
{
    // figure out if we need to switch streaming rates or latency.
    if (!_pipeline || !_jitterBuffer) {
        return;
    }

    GstStructure *stats;
    gint fill_percent = 0;

    g_object_get(_jitterBuffer, "stats", &stats, NULL);
    g_object_get(_jitterBuffer, "percent", &fill_percent, NULL);

    guint64 num_pushed = 0;
    guint64 num_lost = 0;
    guint64 num_late = 0;
    guint64 num_duplicates = 0;
    guint64 rtx_count = 0;
    guint64 rtx_success_count = 0;
    gdouble rtx_per_packet = 0;
    guint64 rtx_rtt = 0;

    gst_structure_get(stats, "num-pushed",
                      gst_structure_get_field_type(stats,"num-pushed"),
                      &num_pushed, NULL);
    gst_structure_get(stats, "num-lost",
                      gst_structure_get_field_type(stats,"num-lost"),
                      &num_lost, NULL);
    gst_structure_get(stats, "num-late",
                      gst_structure_get_field_type(stats,"num-late"),
                      &num_late, NULL);
    gst_structure_get(stats, "num-duplicates",
                      gst_structure_get_field_type(stats,"num-duplicates"),
                      &num_duplicates, NULL);
    gst_structure_get(stats, "rtx-count",
                      gst_structure_get_field_type(stats,"rtx-count"),
                      &rtx_count, NULL);
    gst_structure_get(stats, "rtx-success-count",
                      gst_structure_get_field_type(stats,"rtx-success-count"),
                      &rtx_success_count, NULL);
    gst_structure_get(stats, "rtx-per-packet",
                      gst_structure_get_field_type(stats,"rtx-per-packet"),
                      &rtx_per_packet, NULL);
    gst_structure_get(stats, "rtx-rtt",
                      gst_structure_get_field_type(stats,"rtx-rtt"),
                      &rtx_rtt, NULL);

    qDebug() << "Stats structure: " << gst_structure_get_name(stats);
    qDebug() << "Stats structure num-pushed: " << num_pushed;
    qDebug() << "Stats structure num-lost: " << num_lost;
    qDebug() << "Stats structure num-late: " << num_late;
    qDebug() << "Stats structure num-duplicates: " << num_duplicates;
    qDebug() << "Stats structure rtx-count: " << rtx_count;
    qDebug() << "Stats structure rtx-success-count: " << rtx_success_count;
    qDebug() << "Stats structure rtx-per-packet: " << rtx_per_packet;
    qDebug() << "Stats structure rtx-rtt: " << rtx_rtt;
    qDebug() << "buffer filled percentage" << fill_percent;

    gst_structure_free (stats);
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_cleanupOldVideos()
{
    //-- Only perform cleanup if storage limit is enabled
    if(qgcApp()->toolbox()->settingsManager()->videoSettings()->enableStorageLimit()->rawValue().toBool()) {
        QString savePath = qgcApp()->toolbox()->settingsManager()->appSettings()->videoSavePath();
        QDir videoDir = QDir(savePath);
        videoDir.setFilter(QDir::Files | QDir::Readable | QDir::NoSymLinks | QDir::Writable);
        videoDir.setSorting(QDir::Time);
        //-- All the movie extensions we support
        QStringList nameFilters;
        for(uint32_t i = 0; i < NUM_MUXES; i++) {
            nameFilters << QString("*.") + QString(kVideoExtensions[i]);
        }
        videoDir.setNameFilters(nameFilters);
        //-- get the list of videos stored
        QFileInfoList vidList = videoDir.entryInfoList();
        if(!vidList.isEmpty()) {
            uint64_t total   = 0;
            //-- Settings are stored using MB
            uint64_t maxSize = (qgcApp()->toolbox()->settingsManager()->videoSettings()->maxVideoSize()->rawValue().toUInt() * 1024 * 1024);
            //-- Compute total used storage
            for(int i = 0; i < vidList.size(); i++) {
                total += vidList[i].size();
            }
            //-- Remove old movies until max size is satisfied.
            while(total >= maxSize && !vidList.isEmpty()) {
                total -= vidList.last().size();
                qCDebug(VideoReceiverLog) << "Removing old video file:" << vidList.last().filePath();
                QFile file (vidList.last().filePath());
                file.remove();
                vidList.removeLast();
            }
        }
    }
}
#endif

//-----------------------------------------------------------------------------
// When we finish our pipeline will look like this:
//
//                                   +-->queue-->decoder-->_videosink
//                                   |
//    datasource-->demux-->parser-->tee
//                                   |
//                                   |    +--------------_sink-------------------+
//                                   |    |                                      |
//   we are adding these elements->  +->teepad-->queue-->matroskamux-->_filesink |
//                                        |                                      |
//                                        +--------------------------------------+
void
VideoReceiver::startRecording(void)
{
#if defined(QGC_GST_STREAMING)

    qCDebug(VideoReceiverLog) << "startRecording()";
    // exit immediately if we are already recording
    if(_pipeline == NULL || _recording) {
        qCDebug(VideoReceiverLog) << "Already recording!";
        return;
    }

    QString savePath = qgcApp()->toolbox()->settingsManager()->appSettings()->videoSavePath();
    if(savePath.isEmpty()) {
        qgcApp()->showMessage(tr("Unabled to record video. Video save path must be specified in Settings."));
        return;
    }

    uint32_t muxIdx = qgcApp()->toolbox()->settingsManager()->videoSettings()->recordingFormat()->rawValue().toUInt();
    if(muxIdx >= NUM_MUXES) {
        qgcApp()->showMessage(tr("Invalid video format defined."));
        return;
    }

    //-- Disk usage maintenance
    _cleanupOldVideos();

    _sink           = new Sink();
    _sink->teepad   = gst_element_get_request_pad(_tee, "src_%u");
    _sink->queue    = gst_element_factory_make("queue", NULL);
    _sink->parse    = gst_element_factory_make("h264parse", NULL);
    _sink->mux      = gst_element_factory_make(kVideoMuxes[muxIdx], NULL);
    _sink->filesink = gst_element_factory_make("filesink", NULL);
    _sink->removing = false;

    if(!_sink->teepad || !_sink->queue || !_sink->mux || !_sink->filesink || !_sink->parse) {
        qCritical() << "VideoReceiver::startRecording() failed to make _sink elements";
        return;
    }

    QString videoFile;
    videoFile = savePath + "/" + QDateTime::currentDateTime().toString("yyyy-MM-dd_hh.mm.ss") + "." + kVideoExtensions[muxIdx];

    g_object_set(G_OBJECT(_sink->filesink), "location", qPrintable(videoFile), NULL);
    qCDebug(VideoReceiverLog) << "New video file:" << videoFile;

    gst_object_ref(_sink->queue);
    gst_object_ref(_sink->parse);
    gst_object_ref(_sink->mux);
    gst_object_ref(_sink->filesink);

    gst_bin_add_many(GST_BIN(_pipeline), _sink->queue, _sink->parse, _sink->mux, _sink->filesink, NULL);
    gst_element_link_many(_sink->queue, _sink->parse, _sink->mux, _sink->filesink, NULL);

    gst_element_sync_state_with_parent(_sink->queue);
    gst_element_sync_state_with_parent(_sink->parse);
    gst_element_sync_state_with_parent(_sink->mux);
    gst_element_sync_state_with_parent(_sink->filesink);

    GstPad* sinkpad = gst_element_get_static_pad(_sink->queue, "sink");
    gst_pad_link(_sink->teepad, sinkpad);
    gst_object_unref(sinkpad);

    _recording = true;
    emit recordingChanged();
    qCDebug(VideoReceiverLog) << "Recording started";
#endif
}

//-----------------------------------------------------------------------------
void
VideoReceiver::stopRecording(void)
{
#if defined(QGC_GST_STREAMING)
    qCDebug(VideoReceiverLog) << "stopRecording()";
    // exit immediately if we are not recording
    if(_pipeline == NULL || !_recording) {
        qCDebug(VideoReceiverLog) << "Not recording!";
        return;
    }
    // Wait for data block before unlinking
    gst_pad_add_probe(_sink->teepad, GST_PAD_PROBE_TYPE_IDLE, _unlinkCallBack, this, NULL);
#endif
}

//-----------------------------------------------------------------------------
// This is only installed on the transient _pipelineStopRec in order
// to finalize a video file. It is not used for the main _pipeline.
// -EOS has appeared on the bus of the temporary pipeline
// -At this point all of the recoring elements have been flushed, and the video file has been finalized
// -Now we can remove the temporary pipeline and its elements
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_shutdownRecordingBranch()
{
    gst_bin_remove(GST_BIN(_pipelineStopRec), _sink->queue);
    gst_bin_remove(GST_BIN(_pipelineStopRec), _sink->parse);
    gst_bin_remove(GST_BIN(_pipelineStopRec), _sink->mux);
    gst_bin_remove(GST_BIN(_pipelineStopRec), _sink->filesink);

    gst_element_set_state(_pipelineStopRec, GST_STATE_NULL);
    gst_object_unref(_pipelineStopRec);
    _pipelineStopRec = NULL;

    gst_element_set_state(_sink->filesink,  GST_STATE_NULL);
    gst_element_set_state(_sink->parse,     GST_STATE_NULL);
    gst_element_set_state(_sink->mux,       GST_STATE_NULL);
    gst_element_set_state(_sink->queue,     GST_STATE_NULL);

    gst_object_unref(_sink->queue);
    gst_object_unref(_sink->parse);
    gst_object_unref(_sink->mux);
    gst_object_unref(_sink->filesink);

    delete _sink;
    _sink = NULL;
    _recording = false;

    emit recordingChanged();
    qCDebug(VideoReceiverLog) << "Recording Stopped";
}
#endif

//-----------------------------------------------------------------------------
// -Unlink the recording branch from the tee in the main _pipeline
// -Create a second temporary pipeline, and place the recording branch elements into that pipeline
// -Setup watch and handler for EOS event on the temporary pipeline's bus
// -Send an EOS event at the beginning of that pipeline
#if defined(QGC_GST_STREAMING)
void
VideoReceiver::_detachRecordingBranch(GstPadProbeInfo* info)
{
    Q_UNUSED(info)

    // Also unlinks and unrefs
    gst_bin_remove_many(GST_BIN(_pipeline), _sink->queue, _sink->parse, _sink->mux, _sink->filesink, NULL);

    // Give tee its pad back
    gst_element_release_request_pad(_tee, _sink->teepad);
    gst_object_unref(_sink->teepad);

    // Create temporary pipeline
    _pipelineStopRec = gst_pipeline_new("pipeStopRec");

    // Put our elements from the recording branch into the temporary pipeline
    gst_bin_add_many(GST_BIN(_pipelineStopRec), _sink->queue, _sink->parse, _sink->mux, _sink->filesink, NULL);
    gst_element_link_many(_sink->queue, _sink->parse, _sink->mux, _sink->filesink, NULL);

    // Add handler for EOS event
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(_pipelineStopRec));
    gst_bus_enable_sync_message_emission(bus);
    g_signal_connect(bus, "sync-message", G_CALLBACK(_onBusMessage), this);
    gst_object_unref(bus);

    if(gst_element_set_state(_pipelineStopRec, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        qCDebug(VideoReceiverLog) << "problem starting _pipelineStopRec";
    }

    // Send EOS at the beginning of the pipeline
    GstPad* sinkpad = gst_element_get_static_pad(_sink->queue, "sink");
    gst_pad_send_event(sinkpad, gst_event_new_eos());
    gst_object_unref(sinkpad);
    qCDebug(VideoReceiverLog) << "Recording branch unlinked";
}
#endif

//-----------------------------------------------------------------------------
#if defined(QGC_GST_STREAMING)
GstPadProbeReturn
VideoReceiver::_unlinkCallBack(GstPad* pad, GstPadProbeInfo* info, gpointer user_data)
{
    Q_UNUSED(pad);
    if(info != NULL && user_data != NULL) {
        VideoReceiver* pThis = (VideoReceiver*)user_data;
        // We will only act once
        if(g_atomic_int_compare_and_exchange(&pThis->_sink->removing, FALSE, TRUE)) {
            pThis->_detachRecordingBranch(info);
        }
    }
    return GST_PAD_PROBE_REMOVE;
}
#endif

//-----------------------------------------------------------------------------
void
VideoReceiver::_updateTimer()
{
#if defined(QGC_GST_STREAMING)
    if(_videoSurface) {
        if(stopping() || starting()) {
            return;
        }
        if(streaming()) {
            if(!_videoRunning) {
                _videoSurface->setLastFrame(0);
                _videoRunning = true;
                emit videoRunningChanged();
            }
        } else {
            if(_videoRunning) {
                _videoRunning = false;
                emit videoRunningChanged();
            }
        }
        if(_videoRunning) {
            uint32_t timeout = 1;
            if(qgcApp()->toolbox() && qgcApp()->toolbox()->settingsManager()) {
                timeout = qgcApp()->toolbox()->settingsManager()->videoSettings()->rtspTimeout()->rawValue().toUInt();
            }
            time_t elapsed = 0;
            time_t lastFrame = _videoSurface->lastFrame();
            if(lastFrame != 0) {
                elapsed = time(0) - _videoSurface->lastFrame();
            }
            if(elapsed > (time_t)timeout && _videoSurface) {
                stop();
            }
        } else {
            if(!running() && !_uri.isEmpty()) {
                start();
            }
        }
    }
#endif
}

