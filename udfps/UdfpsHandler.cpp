/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "UdfpsHandler.xiaomi_sm6225"

#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>

#include <poll.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include <display/drm/mi_disp.h>

#include "UdfpsHandler.h"
#include "xiaomi_touch.h"

#define COMMAND_NIT 10
#define PARAM_NIT_FOD 1
#define PARAM_NIT_NONE 0

#define COMMAND_FOD_PRESS_STATUS 1
#define PARAM_FOD_PRESSED 1
#define PARAM_FOD_RELEASED 0

#define FOD_STATUS_OFF 0
#define FOD_STATUS_ON 1

// ANTI-FALSO POSITIVO (PRESS): el nodo crudo fod_press_status se dispara
// con cualquier toque en la zona del sensor, incluso un simple roce/deslizar
// sin intencion real de desbloquear. Antes de confiar en un "pressed",
// se re-muestrea varias veces seguidas para confirmar que el dedo se
// mantiene quieto ahi, evitando que un roce encienda el HBM.
#define RAW_PRESS_CONFIRM_SAMPLES 3
#define RAW_PRESS_CONFIRM_INTERVAL_MS 20

#define TOUCH_DEV_PATH "/dev/xiaomi-touch"
#define TOUCH_MAGIC 'T'
#define TOUCH_IOC_SET_CUR_VALUE _IO(TOUCH_MAGIC, SET_CUR_VALUE)
#define TOUCH_IOC_GET_CUR_VALUE _IO(TOUCH_MAGIC, GET_CUR_VALUE)

#define DISP_FEATURE_PATH "/dev/mi_display/disp_feature"
#define FOD_PRESS_STATUS_PATH "/sys/class/touch/touch_dev/fod_press_status"
#define BRIGHTNESS_PATH "/sys/class/backlight/panel0-backlight/brightness"

using ::aidl::android::hardware::biometrics::fingerprint::AcquiredInfo;

namespace {

static bool readBool(int fd) {
    char c;
    int rc;

    rc = pread(fd, &c, sizeof(char), 0);
    if (rc != 1) {
        LOG(ERROR) << "failed to read bool from fd, err: " << rc;
        return false;
    }

    return c != '0';
}

static disp_event_resp* parseDispEvent(int fd) {
    static char event_data[1024];
    ssize_t size;

    memset(event_data, 0, sizeof(event_data));
    size = read(fd, event_data, sizeof(event_data));
    if (size < 0) {
        LOG(ERROR) << "read fod event failed";
        return nullptr;
    }

    if (size < (ssize_t)sizeof(struct disp_event)) {
        LOG(ERROR) << "Invalid event size " << size << ", expect at least "
                   << sizeof(struct disp_event);
        return nullptr;
    }

    return reinterpret_cast<disp_event_resp*>(event_data);
}

}  // anonymous namespace

class XiaomiSm6225UdfpsHandler : public UdfpsHandler {
  public:
    XiaomiSm6225UdfpsHandler() : mDevice(nullptr), isFpcFod(false) {}

    ~XiaomiSm6225UdfpsHandler() {
        LOG(INFO) << "Destructor called, shutting down threads";
        shutdownThreads();
    }

    void init(fingerprint_device_t* device) {
        LOG(INFO) << "Initializing UDFPS handler";
        
        mDevice = device;
        
        // Open device nodes
        touch_fd_ = android::base::unique_fd(open(TOUCH_DEV_PATH, O_RDWR));
        if (touch_fd_.get() < 0) {
            LOG(ERROR) << "Failed to open touch device: " << strerror(errno);
        }

        disp_fd_ = android::base::unique_fd(open(DISP_FEATURE_PATH, O_RDWR));
        if (disp_fd_.get() < 0) {
            LOG(ERROR) << "Failed to open display device: " << strerror(errno);
        }

        // Determine fingerprint vendor
        std::string fpVendor = android::base::GetProperty("persist.vendor.sys.fp.vendor", "none");
        LOG(INFO) << "Fingerprint vendor: " << fpVendor;
        isFpcFod = (fpVendor == "fpc_fod");

        // Start monitoring threads
        fodThread_ = std::thread([this]() { fodPressMonitorThread(); });
        dispThread_ = std::thread([this]() { displayEventMonitorThread(); });

        // FIX: trước đây thread này chỉ chạy khi isFpcFod == true, nghĩa là
        // với các vendor khac (vd goodix), Touch_Fod_Enable không bao giờ
        // được bật khi tắt màn hình -> vùng cảm biến FOD trên touch IC
        // không được giữ ở chế độ quét -> chạm vào lúc màn hình tắt hoàn
        // toàn im lặng (không rung, không sáng). Chạy thread này cho mọi
        // vendor, chỉ còn phụ thuộc toggle persist.vendor.sys.fp.screen_off.
        screenThread_ = std::thread([this]() { screenStateMonitorThread(); });

        LOG(INFO) << "UDFPS handler initialized";
    }

    void onFingerDown(uint32_t /*x*/, uint32_t /*y*/, float /*minor*/, float /*major*/) {
        LOG(INFO) << __func__;
        
        mFbDownTimeMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

        /*
         * On fpc_fod devices, enable FOD status when finger down is detected
         * since the waiting message is not reliably sent.
         */
        if (isFpcFod) {
            setFodStatus(FOD_STATUS_ON);
        }

        setFingerDown(true);
    }

    void onFingerUp() {
        LOG(INFO) << __func__;
        
        uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        uint64_t elapsed = now - mFbDownTimeMs.load();

        if (elapsed < 250) {
            // FIX: don't blindly discard this UP just because it came in
            // fast - verify against the raw sensor node first. Blindly
            // trusting elapsed time here is what let the HBM circle get
            // stuck lit (e.g. after a quick failed attempt): a real UP
            // was thrown away and setFingerDown(false) never ran.
            int rawFd = open(FOD_PRESS_STATUS_PATH, O_RDONLY);
            bool stillPressed = false;
            if (rawFd >= 0) {
                stillPressed = readBool(rawFd);
                close(rawFd);
            }

            if (stillPressed) {
                LOG(INFO) << "UDFPS: Ignoring false framework UP (" << elapsed
                           << "ms), finger still detected on sensor";
                return;
            }

            LOG(INFO) << "UDFPS: framework UP after " << elapsed
                       << "ms, but sensor confirms finger is up - honoring it";
        }

        setFingerDown(false);
    }

    void onAcquired(int32_t result, int32_t vendorCode) {
        LOG(INFO) << __func__ << " result: " << result << " vendorCode: " << vendorCode;
        
        if (static_cast<AcquiredInfo>(result) == AcquiredInfo::GOOD) {
            // Disable HBM on successful acquisition
            {
                std::lock_guard<std::mutex> lock(disp_mutex_);
                if (disp_fd_.get() >= 0) {
                    disp_local_hbm_req req;
                    req.base.flag = 0;
                    req.base.disp_id = MI_DISP_PRIMARY;
                    req.local_hbm_value = LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
                    ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req);
                }
            }
            
            if (!enrolling.load()) {
                setFodStatus(FOD_STATUS_OFF);
            }
        }

        /*
         * Vendor codes:
         * 21: waiting for finger (goodix_fod)
         * 22: finger down (fpc_fod)
         * 23: finger up
         */
        if (!isFpcFod && vendorCode == 21) {
            setFodStatus(FOD_STATUS_ON);
        } else if (isFpcFod && vendorCode == 22) {
            setFodStatus(FOD_STATUS_ON);
        }
    }

    void onError(int32_t error, int32_t vendorCode) {
        LOG(INFO) << __func__ << " error: " << error << " vendorCode: " << vendorCode;

        // FIX: onError() (e.g. ERROR_LOCKOUT after "too many attempts",
        // ERROR_CANCELED, ERROR_TIMEOUT) ends the auth session on its own -
        // the framework does not necessarily call onFingerUp() or cancel()
        // afterwards, since as far as it's concerned the operation already
        // finished (with an error). This handler had no onError() override
        // at all, so it fell through to the base class' no-op default and
        // nothing ever told the touch controller / Local HBM to turn back
        // off. That's exactly the "stuck lit" white circle seen after a
        // lockout: it stayed on through the whole PIN entry screen because
        // no code path ever ran setFingerDown(false) for this case.
        if (!enrolling.load()) {
            setFodStatus(FOD_STATUS_OFF);
        }
        setFingerDown(false);
    }

    void cancel() {
        LOG(INFO) << __func__;
        enrolling.store(false);
        setFodStatus(FOD_STATUS_OFF);
        // FIX: cancel() solo apagaba la deteccion cruda del driver de touch,
        // pero dejaba el Local HBM (brillo blanco) encendido si la sesion se
        // cancelaba antes de un onFingerUp() real (p.ej. al forzar PIN tras
        // demasiados intentos fallidos). Sin esto el circulo blanco se queda
        // pegado en pantalla hasta que algo mas lo resetee.
        setFingerDown(false);
    }

    void preEnroll() {
        LOG(INFO) << __func__;
        enrolling.store(true);
    }

    void enroll() {
        LOG(INFO) << __func__;
        enrolling.store(true);
    }

    void postEnroll() {
        LOG(INFO) << __func__;
        enrolling.store(false);
        setFodStatus(FOD_STATUS_OFF);
        // FIX: mismo problema que en cancel(), asegurar que el HBM quede apagado.
        setFingerDown(false);
    }

  private:
    fingerprint_device_t* mDevice;
    android::base::unique_fd touch_fd_;
    android::base::unique_fd disp_fd_;
    std::atomic<bool> enrolling{false};
    std::atomic<bool> isRunning{true};
    bool isFpcFod;
    
    std::atomic<uint64_t> mFbDownTimeMs{0};
    // FIX: independent timestamp for the raw hardware sensor thread.
    // mFbDownTimeMs used to be shared between the real framework
    // onFingerDown()/onFingerUp() path and fodPressMonitorThread().
    // The raw thread stamped mFbDownTimeMs on every physical press
    // pulse, even ones it went on to ignore (e.g. a stray touch on the
    // "0" PIN key, which sits right over the sensor, with the screen
    // already on). That reset the clock onFingerUp() uses to detect
    // bounce, so a real finger-up right after such a stray touch was
    // misread as "bounce" and setFingerDown(false) was skipped -
    // leaving the HBM circle stuck on screen. Keeping the raw thread's
    // own timing fully separate stops it from corrupting the
    // framework-side debounce.
    std::atomic<uint64_t> mRawDownTimeMs{0};

    // WATCHDOG: lưới an toàn cuối cùng. Không thể đảm bảo framework/driver
    // luôn gọi lại onFingerUp()/onError()/cancel() khi kết thúc phiên xác
    // thực (vd: lockout sau 5 lần sai không phát sinh callback nào xuống
    // handler này - xem log). Nếu trạng thái "pressed" bị treo quá lâu,
    // chủ động ép tắt HBM/FOD để đèn vân tay không sáng vĩnh viễn.
    std::atomic<bool> mFingerIsDown{false};
    std::atomic<uint64_t> mLastDownTimeMs{0};
    static constexpr uint64_t WATCHDOG_TIMEOUT_MS = 3000;

    // Mutexes for thread safety
    std::mutex touch_mutex_;
    std::mutex disp_mutex_;
    std::mutex device_mutex_;

    // Thread objects
    std::thread fodThread_;
    std::thread dispThread_;
    std::thread screenThread_;

    int getBrightness() {
        int fd = open(BRIGHTNESS_PATH, O_RDONLY);
        if (fd < 0) return -1;
        char buf[12];
        ssize_t len = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (len <= 0) return -1;
        buf[len] = '\0';
        return atoi(buf);
    }

    void screenStateMonitorThread() {
        int lastState = -1;
        while (isRunning.load()) {
            int brightness = getBrightness();
            if (brightness != -1) {
                int currentState = (brightness == 0) ? 0 : 1;
                
                // Read Android screen-off toggle state
                bool isScreenOffEnabled = android::base::GetBoolProperty("persist.vendor.sys.fp.screen_off", true);

                if (currentState != lastState) {
                    // FIX: bỏ điều kiện isFpcFod - áp dụng cho mọi vendor,
                    // nếu không vùng FOD trên touch IC không được giữ quét
                    // khi tắt màn hình (xem ghi chú ở init()).
                    if (currentState == 0 && isScreenOffEnabled) {
                        setFodStatus(FOD_STATUS_ON);
                    } else if (currentState == 1) {
                        if (!enrolling.load()) {
                            setFodStatus(FOD_STATUS_OFF);
                        }
                    }
                    lastState = currentState;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    void shutdownThreads() {
        isRunning.store(false);
        // Join threads if they are running
        if (fodThread_.joinable()) {
            fodThread_.join();
        }
        if (dispThread_.joinable()) {
            dispThread_.join();
        }
        if (screenThread_.joinable()) {
            screenThread_.join();
        }
    }

    void fodPressMonitorThread() {
        LOG(INFO) << "FOD press monitor thread started";
        
        int fd = open(FOD_PRESS_STATUS_PATH, O_RDONLY);
        if (fd < 0) {
            LOG(ERROR) << "Failed to open " << FOD_PRESS_STATUS_PATH 
                       << ", error: " << strerror(errno);
            return;
        }

        // Initial dummy read to clear state
        readBool(fd);

        struct pollfd fodPressStatusPoll = {
            .fd = fd,
            .events = POLLERR | POLLPRI,
            .revents = 0,
        };

        while (isRunning.load()) {
            // Trả lại 1000ms: bản 200ms trước đó bị nghi là nguyên nhân
            // khiến vân tay không hoạt động khi tắt màn hình (theo xác nhận
            // của người dùng: "trước khi chỉnh 3000ms xuống 200ms" vẫn hoạt
            // động bình thường). Watchdog "hỏi thẳng raw node" bên dưới vẫn
            // giữ nguyên, chỉ chạy thưa hơn - vẫn tắt đèn nhanh hơn nhiều so
            // với bản chỉ chờ 3000ms mù trước đây.
            int rc = poll(&fodPressStatusPoll, 1, 1000);

            if (rc < 0) {
                if (errno == EINTR) continue;
                LOG(ERROR) << "Poll failed: " << strerror(errno);
                break;
            }

            if (rc == 0) {
                if (mFingerIsDown.load()) {
                    // Ưu tiên hỏi thẳng cảm biến vật lý: nếu ngón tay đã
                    // thực sự rời cảm biến (raw = released) mà state vẫn
                    // "pressed" (framework không gửi onFingerUp/onError/
                    // cancel - đúng tình huống khoá vân tay sau 5 lần sai),
                    // tắt ngay lập tức, không chờ đủ WATCHDOG_TIMEOUT_MS.
                    if (!readBool(fd)) {
                        LOG(WARNING) << "UDFPS: Watchdog - raw sensor already "
                                        "released while state stuck, forcing off";
                        setFingerDown(false);
                        if (!enrolling.load()) {
                            setFodStatus(FOD_STATUS_OFF);
                        }
                    } else {
                        // Cảm biến vẫn báo có ngón tay nhưng đã quá lâu -
                        // lưới an toàn cuối cùng, phòng trường hợp không tin
                        // được node raw (vd: driver kẹt giá trị cũ).
                        uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        if (now - mLastDownTimeMs.load() > WATCHDOG_TIMEOUT_MS) {
                            LOG(WARNING) << "UDFPS: Watchdog - finger-down state stuck > "
                                         << WATCHDOG_TIMEOUT_MS
                                         << "ms despite raw still pressed, forcing off";
                            setFingerDown(false);
                            if (!enrolling.load()) {
                                setFodStatus(FOD_STATUS_OFF);
                            }
                        }
                    }
                }
                continue;
            }

            // Check for expected events
            if (!(fodPressStatusPoll.revents & (POLLERR | POLLPRI))) {
                if (fodPressStatusPoll.revents & (POLLHUP | POLLNVAL)) {
                    LOG(ERROR) << "Poll error event: " << fodPressStatusPoll.revents;
                    break;
                }
                fodPressStatusPoll.revents = 0;
                continue;
            }

            // Clear revents
            fodPressStatusPoll.revents = 0;

            bool pressed = readBool(fd);

            // FILTRO DE FALSOS PRESS HARDWARE (roce/deslizar sobre el sensor)
            if (pressed) {
                bool stillPressed = true;
                for (int sample = 0; sample < RAW_PRESS_CONFIRM_SAMPLES; sample++) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(RAW_PRESS_CONFIRM_INTERVAL_MS));
                    if (!readBool(fd)) {
                        stillPressed = false;
                        break;
                    }
                }
                if (!stillPressed) {
                    LOG(DEBUG) << "UDFPS: Ignorando toque breve (el dedo ya no esta), "
                                  "probablemente un roce sobre el sensor";
                    continue;
                }
                pressed = true;
            }

            uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            
            // FILTRO DE FALSOS UP HARDWARE
            // FIX: use the raw-thread-only timestamp here, not the one
            // shared with onFingerDown()/onFingerUp(), so a raw pulse
            // (forwarded or not) can never reset the framework's own
            // bounce timer.
            if (pressed) {
                mRawDownTimeMs.store(now);
            } else {
                uint64_t elapsed = now - mRawDownTimeMs.load();
                if (elapsed < 250) {
                    LOG(INFO) << "UDFPS: Hardware UP too fast (" << elapsed << "ms). Esperando 100ms...";
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    if (readBool(fd)) {
                        LOG(INFO) << "UDFPS: Finger still present, ignoring false physical UP event.";
                        continue; 
                    }
                }
            }

            // SECURITY: Only send touch event if Screen-Off is enabled or screen is on
            bool isScreenOffEnabled = android::base::GetBoolProperty("persist.vendor.sys.fp.screen_off", true);
            bool screenOff = (getBrightness() == 0);
            if (!isScreenOffEnabled && screenOff) {
                LOG(INFO) << "UDFPS: Touch ignored, Screen-Off feature disabled.";
                continue;
            }

            /*
             * ANTI-FALSO POSITIVO: este hilo solo existe para permitir que el
             * sensor despierte el dispositivo con la pantalla APAGADA (el nodo
             * crudo fod_press_status se dispara por hardware con cualquier
             * toque en esa zona fisica, sin importar que hay en pantalla).
             *
             * El desbloqueo con la pantalla ENCENDIDA ya funciona de forma
             * fiable via onFingerDown()/onFingerUp(), que el framework solo
             * invoca cuando el toque cae realmente sobre el icono/overlay de
             * UDFPS. Reenviar aqui tambien los "pressed" con pantalla
             * encendida es lo que causaba que se iluminara el HBM al pulsar
             * el "0" del PIN, al bajar el centro de control o al colgar una
             * llamada: esos toques caen sobre la misma zona fisica del
             * sensor por pura coincidencia de layout, no porque el usuario
             * quisiera usar la huella.
             *
             * Los "released" siempre se reenvian para no dejar el HBM/estado
             * de dedo-abajo colgado si la pantalla cambia de estado a mitad
             * de un gesto.
             */
            if (pressed && !screenOff) {
                LOG(DEBUG) << "UDFPS: Toque crudo ignorado con pantalla encendida "
                              "(no es un toque real sobre el icono de huella)";
                continue;
            }

            LOG(DEBUG) << "fod_press_status changed: " << (pressed ? "pressed" : "released");
            setFingerDown(pressed);
        }

        close(fd);
        LOG(INFO) << "FOD press monitor thread stopped";
    }

    void displayEventMonitorThread() {
        LOG(INFO) << "Display event monitor thread started";
        
        int fd = open(DISP_FEATURE_PATH, O_RDWR);
        if (fd < 0) {
            LOG(ERROR) << "Failed to open " << DISP_FEATURE_PATH 
                       << ", error: " << strerror(errno);
            return;
        }

        // Register for FOD events
        disp_event_req req;
        req.base.flag = 0;
        req.base.disp_id = MI_DISP_PRIMARY;
        req.type = MI_DISP_EVENT_FOD;
        if (ioctl(fd, MI_DISP_IOCTL_REGISTER_EVENT, &req) < 0) {
            LOG(ERROR) << "Failed to register for display events: " << strerror(errno);
            close(fd);
            return;
        }

        struct pollfd dispEventPoll = {
            .fd = fd,
            .events = POLLIN,
            .revents = 0,
        };

        while (isRunning.load()) {
            int rc = poll(&dispEventPoll, 1, 1000);  // 1 second timeout
            
            if (rc < 0) {
                if (errno == EINTR) continue;
                LOG(ERROR) << "Display poll failed: " << strerror(errno);
                break;
            }

            if (rc == 0) continue;

            // Check for expected events
            if (!(dispEventPoll.revents & POLLIN)) {
                if (dispEventPoll.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    LOG(ERROR) << "Display poll error: " << dispEventPoll.revents;
                    break;
                }
                dispEventPoll.revents = 0;
                continue;
            }

            // Clear revents
            dispEventPoll.revents = 0;

            struct disp_event_resp* response = parseDispEvent(fd);
            if (response == nullptr) {
                continue;
            }

            if (response->base.type != MI_DISP_EVENT_FOD) {
                LOG(WARNING) << "Unexpected display event: " << response->base.type;
                continue;
            }

            int value = response->data[0];
            LOG(DEBUG) << "Display event data: 0x" << std::hex << value;

            bool localHbmUiReady = value & LOCAL_HBM_UI_READY;
            
            std::lock_guard<std::mutex> deviceLock(device_mutex_);
            if (mDevice != nullptr) {
                mDevice->extCmd(mDevice, COMMAND_NIT,
                              localHbmUiReady ? PARAM_NIT_FOD : PARAM_NIT_NONE);
            }
        }

        close(fd);
        LOG(INFO) << "Display event monitor thread stopped";
    }

    void setFodStatus(int value) {
        std::lock_guard<std::mutex> lock(touch_mutex_);
        
        if (touch_fd_.get() < 0) {
            LOG(ERROR) << "Touch device not opened";
            return;
        }

        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, value};
        if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) < 0) {
            LOG(ERROR) << "Failed to set FOD status: " << strerror(errno);
        } else {
            LOG(DEBUG) << "Set FOD status to " << value;
        }
    }

    void setFingerDown(bool pressed) {
        // DEBUG: single choke point for every HBM/finger-down change, so
        // logcat can show exactly when and how many times this gets called
        // with which value, no matter which callback triggered it.
        LOG(INFO) << "UDFPS setFingerDown(" << (pressed ? "true" : "false") << ") called";

        // WATCHDOG: ghi nhận trạng thái/mốc thời gian để vòng poll bên dưới
        // có thể phát hiện nếu "pressed" bị treo quá lâu mà không ai tắt.
        mFingerIsDown.store(pressed);
        if (pressed) {
            mLastDownTimeMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        // Update touch controller
        {
            std::lock_guard<std::mutex> lock(touch_mutex_);
            if (touch_fd_.get() >= 0) {
                int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_FOD_DOWNUP_CTL, pressed ? 1 : 0};
                if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) < 0) {
                    LOG(ERROR) << "Failed to set finger down: " << strerror(errno);
                }
            }
        }

        // Update display HBM
        {
            std::lock_guard<std::mutex> lock(disp_mutex_);
            if (disp_fd_.get() >= 0) {
                disp_local_hbm_req req;
                req.base.flag = 0;
                req.base.disp_id = MI_DISP_PRIMARY;
                req.local_hbm_value = pressed ? LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT
                                              : LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
                if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) < 0) {
                    LOG(ERROR) << "Failed to set HBM: " << strerror(errno);
                }
            }
        }

        // Notify fingerprint device
        {
            std::lock_guard<std::mutex> lock(device_mutex_);
            if (mDevice != nullptr) {
                mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS,
                              pressed ? PARAM_FOD_PRESSED : PARAM_FOD_RELEASED);
            }
        }
    }
};

static UdfpsHandler* create() {
    return new XiaomiSm6225UdfpsHandler();
}

static void destroy(UdfpsHandler* handler) {
    delete handler;
}

extern "C" UdfpsHandlerFactory UDFPS_HANDLER_FACTORY = {
    .create = create,
    .destroy = destroy,
};