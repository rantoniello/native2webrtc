/**
 */

#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <optional>
#include <sys/time.h>
#include <rtc/rtc.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

using namespace std;

class VideoProducer {
public:
    VideoProducer(function<void(const uint8_t *data, size_t size)> onSample,
            string inputUrl = "");
    ~VideoProducer();

private:
    void dummyStreamThr();
    function<void (const uint8_t *data, size_t size)> sampleHandler;
    string inputUrl;
    thread _videoThread;
};
