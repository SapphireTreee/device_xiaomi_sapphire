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

// Minimum time (ms) a finger must stay down/up before we trust the event.
// Filters out spurious UP events that some panels/firmwares report right
// after a DOWN, which would otherwise cause a flicker of the FOD icon/HBM.
#define FALSE_EVENT_FILTER_MS 250
// Extra grace period (ms) used to double-check a fast hardware UP before
// discarding it as a false positive.
#define FALSE_EVENT_RECHECK_MS 100

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

static inline uint64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
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

        if (isFpcFod) {
            screenThread_ = std::thread([this]() { screenStateMonitorThread(); });
        }

        LOG(INFO) << "UDFPS handler initialized";
    }

    void onFingerDown(uint32_t /*x*/, uint32_t /*y*/, float /*minor*/, float /*major*/) {
        LOG(INFO) << __func__;

        mFbDownTimeMs.store(nowMs());

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

        uint64_t elapsed = nowMs() - mFbDownTimeMs.load();

        if (elapsed < FALSE_EVENT_FILTER_MS) {
            LOG(INFO) << "UDFPS: Ignoring false framework UP event (" << elapsed << "ms elapsed)";
            return;
        }

        setFingerDown(false);
    }

    void onAcquired(int32_t result, int32_t vendorCode) {
        LOG(INFO) << __func__ << " result: " << result << " vendorCode: " << vendorCode;

        if (static_cast<AcquiredInfo>(result) == AcquiredInfo::GOOD) {
            // Disable HBM on successful acquisition
            applyLocalHbm(LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP);

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

    void cancel() {
        LOG(INFO) << __func__;
        enrolling.store(false);
        setFodStatus(FOD_STATUS_OFF);
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
    }

  private:
    fingerprint_device_t* mDevice;
    android::base::unique_fd touch_fd_;
    android::base::unique_fd disp_fd_;
    std::atomic<bool> enrolling{false};
    std::atomic<bool> isRunning{true};
    bool isFpcFod;

    std::atomic<uint64_t> mFbDownTimeMs{0};

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
                    // If toggle is disabled, do not enable touch FOD
                    if (currentState == 0 && isFpcFod && isScreenOffEnabled) {
                        setFodStatus(FOD_STATUS_ON);
                    } else if (currentState == 1 && isFpcFod) {
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
            int rc = poll(&fodPressStatusPoll, 1, 1000);  // 1 second timeout

            if (rc < 0) {
                if (errno == EINTR) continue;
                LOG(ERROR) << "Poll failed: " << strerror(errno);
                break;
            }

            if (rc == 0) continue;

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

            const bool pressed = readBool(fd);
            uint64_t now = nowMs();

            // FALSE-UP FILTER: hardware sometimes reports a release right
            // after a press. If UP arrives too soon after DOWN, double-check
            // that the finger is really gone before honoring it.
            if (pressed) {
                mFbDownTimeMs.store(now);
            } else {
                uint64_t elapsed = now - mFbDownTimeMs.load();
                if (elapsed < FALSE_EVENT_FILTER_MS) {
                    LOG(INFO) << "UDFPS: Hardware UP too fast (" << elapsed
                              << "ms), rechecking after " << FALSE_EVENT_RECHECK_MS << "ms";
                    std::this_thread::sleep_for(std::chrono::milliseconds(FALSE_EVENT_RECHECK_MS));
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
             * ANTI-FALSE-POSITIVE: this thread exists only so the sensor can
             * wake the device while the screen is OFF (the raw
             * fod_press_status node fires on *any* touch in that physical
             * area, regardless of what is on screen).
             *
             * Unlocking with the screen ON already works reliably via
             * onFingerDown()/onFingerUp(), which the framework only invokes
             * when the touch actually lands on the UDFPS icon/overlay.
             * Forwarding "pressed" here too while the screen is on used to
             * light up the HBM when pressing the "0" on the PIN pad, pulling
             * down the control center, or ending a call: those touches land
             * on the same physical sensor area purely by layout coincidence,
             * not because the user wanted to use the fingerprint sensor.
             *
             * "released" events are always forwarded so we never leave the
             * HBM/finger-down state stuck if the screen changes state
             * mid-gesture.
             */
            if (pressed && !screenOff) {
                LOG(DEBUG) << "UDFPS: Ignoring raw touch with screen on "
                              "(not a real touch on the fingerprint icon)";
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

    // Sends a value to the xiaomi-touch driver for the given mode.
    void writeTouchValue(int mode, int value) {
        std::lock_guard<std::mutex> lock(touch_mutex_);

        if (touch_fd_.get() < 0) {
            LOG(ERROR) << "Touch device not opened";
            return;
        }

        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, mode, value};
        if (ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf) < 0) {
            LOG(ERROR) << "Failed to write touch value (mode " << mode << "): " << strerror(errno);
        }
    }

    // Applies a local HBM brightness value on the primary display.
    void applyLocalHbm(int value) {
        std::lock_guard<std::mutex> lock(disp_mutex_);

        if (disp_fd_.get() < 0) {
            LOG(ERROR) << "Display device not opened";
            return;
        }

        disp_local_hbm_req req;
        req.base.flag = 0;
        req.base.disp_id = MI_DISP_PRIMARY;
        req.local_hbm_value = value;
        if (ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req) < 0) {
            LOG(ERROR) << "Failed to set HBM: " << strerror(errno);
        }
    }

    void setFodStatus(int value) {
        writeTouchValue(Touch_Fod_Enable, value);
        LOG(DEBUG) << "Set FOD status to " << value;
    }

    void setFingerDown(bool pressed) {
        // Update touch controller
        writeTouchValue(THP_FOD_DOWNUP_CTL, pressed ? 1 : 0);

        // Update display HBM
        applyLocalHbm(pressed ? LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT
                              : LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP);

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
