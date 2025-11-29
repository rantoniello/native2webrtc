/**
 */

#include "WebRTCStreamer.h"

#include <nlohmann/json.hpp>

using namespace rtc;
using namespace std::chrono_literals;
using json = nlohmann::json;
template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

WebRTCStreamer::Client::Client(const string& stunServer = "") :
    _signalingQueueIn(32),
    _signalingQueueOut(32),
    _stunServer(stunServer)
{
    // Allocate peer connection
    Configuration conf;
    if (_stunServer.length()) {
        cout << "STUN server is " << stunServer << endl;
        conf.iceServers.emplace_back(stunServer);
    }
    // - Disable auto-negotiation. If set to true, the user is
    // responsible for calling 'rtcSetLocalDescription' after creating a data
    // channel and after setting the remote description
    conf.disableAutoNegotiation = true;
    _peerConnection = make_shared<PeerConnection>(conf);

    // Callbacks...
    _peerConnection->onStateChange([this](PeerConnection::State state) {
        cout << "State: " << state << endl;
        if (state == PeerConnection::State::Disconnected ||
            state == PeerConnection::State::Failed ||
            state == PeerConnection::State::Closed) {
            // request to delete this client
            this->_signalingQueueOut.push("quit");
        }
    });
    _peerConnection->onGatheringStateChange(
            [this, wpc = make_weak_ptr(_peerConnection)]
            (PeerConnection::GatheringState state) {
        cout << "Gathering State: " << state << endl;
        if (state == PeerConnection::GatheringState::Complete) {
            if(auto pc = wpc.lock()) {
                auto description = pc->localDescription();
                json message = {
                    {"id", this->_id},
                    {"type", description->typeString()},
                    {"sdp", string(description.value())}
                };
                // Gathering complete, send answer
                this->_signalingQueueOut.push(message.dump());
            }
        }
    });

    // Add video track to peer connection
    const uint8_t payloadType = 102;
    const uint32_t ssrc = 1;
    const string cname = "video-stream";
    const string msid = "stream1";
    auto video = Description::Video(cname);
    video.addH264Codec(payloadType);
    video.addSSRC(ssrc, cname, msid, cname);
    auto track = _peerConnection->addTrack(video);
    // create RTP configuration
    auto rtpConfig = make_shared<RtpPacketizationConfig>(ssrc, cname,
        payloadType, H264RtpPacketizer::ClockRate);
    // create packetizer
    auto packetizer = make_shared<H264RtpPacketizer>(
        NalUnit::Separator::StartSequence, rtpConfig);
    // add RTCP SR handler
    auto srReporter = make_shared<RtcpSrReporter>(rtpConfig);
    packetizer->addToChain(srReporter);
    // add RTCP NACK handler
    auto nackResponder = make_shared<RtcpNackResponder>();
    packetizer->addToChain(nackResponder);
    // set handler
    track->setMediaHandler(packetizer);
    const function<void (void)> onOpen = [this]() {
        cout << "Video from " << this->_id << " opened" << endl;
        //TODO: FIFO push {"signaling":"video track opened","peerId":<id>}
        //vstreamThread = thread(vstreamThr, video); //FIXME!!
    };
    track->onOpen(onOpen);

//    auto dc = pc->createDataChannel("ping-pong");
//    dc->onOpen([_id, wdc = make_weak_ptr(dc)]() {
//        //TODO: FIFO push {"signaling":"data channel opened","peerId":<id>}
//        //if (auto dc = wdc.lock()) {dc->send("Ping");} //FIXME!!: action to do
//    });
//    dc->onMessage(nullptr, [peerId, wdc = make_weak_ptr(dc)](string msg) {
//        cout << "Message from " << peerId << " received: " << msg << endl;
//        //if (auto dc = wdc.lock()) {dc->send("Ping");} //FIXME!!: action to do
//    });

    // Set local description
    _peerConnection->setLocalDescription();
}

void WebRTCStreamer::Client::signalingSet(string& message)
{
    unique_lock lock(_publicApiMutex);
    _signalingQueueIn.push(message);
}

optional<string> WebRTCStreamer::Client::signalingGet(
    chrono::milliseconds timeout)
{
    unique_lock lock(_publicApiMutex);
    return _signalingQueueOut.pop_for(timeout);
}

void WebRTCStreamer::Client::sendFrame(const void *data, size_t size,
        rtc::FrameInfo info)
{
    unique_lock lock(_publicApiMutex);

    // NOTE: for this POC client manages only one track (source video)
    bool justOnce = true;
    _peerConnection->iterateTracks([&](shared_ptr<Track> track) {
        if (!justOnce) return;
        track->send(static_cast<const byte*>(data), size);
        justOnce = false;
    });
}

WebRTCStreamer::WebRTCStreamer(const string& signalingServer) :
        _publicApiMutex(),
        _do_term(false),
        _clientsMap{},
        _signalingServer(signalingServer),
        _signalingWS(make_shared<WebSocket>()),
        _signalingQueueIn(256),
        _framesQueueIn(256)
{
        // Launch signaling thread
        _signalingThread = thread([&] {this->_signalingThr(); });

        // Launch multimedia processing thread
        _multimediaThread = thread([&] {this->_multimediaThr(); });
}

WebRTCStreamer::~WebRTCStreamer()
{
    _do_term = true;
    _signalingWS->close();

    //TODO: wait join signaling thread (thus confir it recieves close!! and unlock)
    //TODO: join signaling thread (after stoping web socket)
    _signalingThread.join();
}

void WebRTCStreamer::pushData(void *const data, size_t size)
{
    unique_lock lock(_publicApiMutex);
    _pushData(data, size);
}

void WebRTCStreamer::_signalingThr()
{
    cout << "Launching signaling thread..." << "\n";
    cout << "URL is " << _signalingServer << endl;

    // Initialize signaling web socket callbacks
    _signalingWS->onOpen([]() {
        cout << "WebSocket connected, signaling ready" << endl;
    });
    _signalingWS->onClosed([&]() {
        cout << "WebSocket closed" << endl;
        if (!_do_term) {
            cout << "WebSocket unexpectedly closed, try to reopen..." << endl;
            // sleep: simple throttling to avoid busy "closed" loops
            this_thread::sleep_for(chrono::milliseconds(1));
            // Open web-socket
            _signalingWS->open(_signalingServer);
        }
    });
    _signalingWS->onError([](const string &error) {
        cout << "WebSocket failed: " << error << endl;
    });
    _signalingWS->onMessage([&](variant<binary, string> data) {
        if (!holds_alternative<string>(data))
            return;
        _signalingQueueIn.push(get<string>(data));
    });

    // Open web-socket
    _signalingWS->open(_signalingServer);

    // Signals processing loop
    while (!_do_term) {
        cout << "Signaling thread polling..." << "\n";
        optional<string> message = _signalingQueueIn.pop();
        if (message == std::nullopt)
            continue;
        cout << "Signaling thread: message is '" << message.value() << "'\n";

        json json_msg = json::parse(message.value());
        auto it = json_msg.find("id");
        if (it == json_msg.end())
            continue;
        string id = it->get<string>();
        it = json_msg.find("type");
        if (it == json_msg.end())
            continue;
        string type = it->get<string>();

        if (type == "request") {
            cout << "Instantiating new client peer..." << "\n";
            _clientsMap.emplace(id, make_shared<WebRTCStreamer::Client>(""));
        } else if (type == "answer") {
            // Push remote peer answer to our client class instance
            auto it = _clientsMap.find(id);
            if (it != _clientsMap.end()) {
                string sdp = json_msg["sdp"].get<string>();
                (it->second)->signalingSet(sdp);
            }
        }
    }
}

void WebRTCStreamer::_pushData(void *const data, size_t size)
{
    if (data == nullptr || size == 0)
        return;

    Frame frame(data, size);
    _framesQueueIn.push(frame);
}

void WebRTCStreamer::_multimediaThr()
{
    cout << "Launching multimedia thread..." << "\n";

    // Multimedia processing loop
    while (!_do_term) {
        optional<Frame> optFrame = _framesQueueIn.pop();
        if (optFrame == std::nullopt)
            continue;

        // Itearte clients instances
        unordered_map<string, shared_ptr<Client>>::iterator it;
        for (it = _clientsMap.begin(); it != _clientsMap.end(); it++) {
            auto client = it->second;
            //video->track->send(static_cast<const byte*>(frame.data), frame.size);
        }
    }
}
