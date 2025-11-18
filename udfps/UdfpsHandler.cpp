/*
 * Copyright (C) 2022-2024 The LineageOS Project
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
#include <fstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>

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

#define TOUCH_DEV_PATH "/dev/xiaomi-touch"
#define TOUCH_MAGIC 'T'
#define TOUCH_IOC_SET_CUR_VALUE _IO(TOUCH_MAGIC, SET_CUR_VALUE)
#define TOUCH_IOC_GET_CUR_VALUE _IO(TOUCH_MAGIC, GET_CUR_VALUE)

#define DISP_FEATURE_PATH "/dev/mi_display/disp_feature"

#define FOD_PRESS_STATUS_PATH "/sys/class/touch/touch_dev/fod_press_status"

// Vendor code constants
#define VENDOR_CODE_WAITING_FINGER 21
#define VENDOR_CODE_FINGER_DOWN 22
#define VENDOR_CODE_FINGER_UP 23

using ::aidl::android::hardware::biometrics::fingerprint::AcquiredInfo;

namespace {

static bool readBool(int fd) {
    char c;
    int rc;

    rc = lseek(fd, 0, SEEK_SET);
    if (rc) {
        LOG(ERROR) << "failed to seek fd, err: " << rc;
        return false;
    }

    rc = read(fd, &c, sizeof(char));
    if (rc != 1) {
        LOG(ERROR) << "failed to read bool from fd, err: " << rc;
        return false;
    }

    return c != '0';
}

static disp_event_resp* parseDispEvent(int fd) {
    static char event_data[1024] = {0};
    ssize_t size;

    memset(event_data, 0x0, sizeof(event_data));
    size = read(fd, event_data, sizeof(event_data));
    if (size < 0) {
        LOG(ERROR) << "read fod event failed";
        return nullptr;
    }

    if (size < sizeof(struct disp_event)) {
        LOG(ERROR) << "Invalid event size " << size << ", expect at least "
                   << sizeof(struct disp_event);
        return nullptr;
    }

    return (struct disp_event_resp*)&event_data[0];
}

}  // anonymous namespace

class XiaomiSm6225UdfpsHandler : public UdfpsHandler {
  public:
    XiaomiSm6225UdfpsHandler() : enrolling_(false), isFpcFod_(false), 
                                  shouldStop_(false), mDevice_(nullptr) {}

    ~XiaomiSm6225UdfpsHandler() {
        LOG(INFO) << __func__ << " - Cleaning up";
        
        // Signal threads to stop
        shouldStop_.store(true);
        
        // Wait for threads to finish
        if (fodPressThread_.joinable()) {
            fodPressThread_.join();
        }
        if (dispEventThread_.joinable()) {
            dispEventThread_.join();
        }
        
        LOG(INFO) << __func__ << " - Cleanup complete";
    }

    void init(fingerprint_device_t* device) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        
        mDevice_ = device;
        touch_fd_ = android::base::unique_fd(open(TOUCH_DEV_PATH, O_RDWR));
        if (touch_fd_.get() < 0) {
            LOG(ERROR) << "Failed to open touch device: " << TOUCH_DEV_PATH;
        }
        
        disp_fd_ = android::base::unique_fd(open(DISP_FEATURE_PATH, O_RDWR));
        if (disp_fd_.get() < 0) {
            LOG(ERROR) << "Failed to open display device: " << DISP_FEATURE_PATH;
        }

        std::string fpVendor = android::base::GetProperty("persist.vendor.sys.fp.vendor", "none");
        LOG(DEBUG) << __func__ << " fingerprint vendor is: " << fpVendor;
        isFpcFod_ = fpVendor == "fpc_fod";

        // Start FOD press status monitoring thread
        fodPressThread_ = std::thread(&XiaomiSm6225UdfpsHandler::fodPressStatusThread, this);
        
        // Start display event monitoring thread
        dispEventThread_ = std::thread(&XiaomiSm6225UdfpsHandler::dispEventThread, this);
    }

    void onFingerDown(uint32_t /*x*/, uint32_t /*y*/, float /*minor*/, float /*major*/) {
        LOG(INFO) << __func__;

        /*
         * On fpc_fod devices, the waiting for finger message is not reliably sent...
         * The finger down message is only reliably sent when the screen is turned off, so enable
         * fod_status better late than never.
         */
        if (isFpcFod_) {
            setFodStatus(FOD_STATUS_ON);
        }

        setFingerDown(true);
    }

    void onFingerUp() {
        LOG(INFO) << __func__;
        setFingerDown(false);
    }

    void onAcquired(int32_t result, int32_t vendorCode) {
        LOG(INFO) << __func__ << " result: " << result << " vendorCode: " << vendorCode;
        
        if (static_cast<AcquiredInfo>(result) == AcquiredInfo::GOOD) {
            // Request to disable HBM already, even if the finger is still pressed
            if (disp_fd_.get() >= 0) {
                disp_local_hbm_req req;
                req.base.flag = 0;
                req.base.disp_id = MI_DISP_PRIMARY;
                req.local_hbm_value = LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
                int rc = ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req);
                if (rc < 0) {
                    LOG(ERROR) << "Failed to disable HBM: " << rc;
                }
            }
            
            bool isEnrolling;
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                isEnrolling = enrolling_;
            }
            
            if (!isEnrolling) {
                setFodStatus(FOD_STATUS_OFF);
            }
        }

        /* vendorCode for goodix_fod devices:
         * 21: waiting for finger
         * 22: finger down
         * 23: finger up
         * On fpc_fod devices, the waiting for finger message is not reliably sent...
         * The finger down message is only reliably sent when the screen is turned off, so enable
         * fod_status better late than never.
         */
        if (!isFpcFod_ && vendorCode == VENDOR_CODE_WAITING_FINGER) {
            setFodStatus(FOD_STATUS_ON);
        } else if (isFpcFod_ && vendorCode == VENDOR_CODE_FINGER_DOWN) {
            setFodStatus(FOD_STATUS_ON);
        }
    }

    void cancel() {
        LOG(INFO) << __func__;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            enrolling_ = false;
        }
        setFodStatus(FOD_STATUS_OFF);
    }

    void preEnroll() {
        LOG(INFO) << __func__;
        std::lock_guard<std::mutex> lock(stateMutex_);
        enrolling_ = true;
    }

    void enroll() {
        LOG(INFO) << __func__;
        std::lock_guard<std::mutex> lock(stateMutex_);
        enrolling_ = true;
    }

    void postEnroll() {
        LOG(INFO) << __func__;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            enrolling_ = false;
        }
        setFodStatus(FOD_STATUS_OFF);
    }

  private:
    fingerprint_device_t* mDevice_;
    android::base::unique_fd touch_fd_;
    android::base::unique_fd disp_fd_;
    
    std::mutex stateMutex_;
    bool enrolling_;
    bool isFpcFod_;
    
    std::atomic<bool> shouldStop_;
    std::thread fodPressThread_;
    std::thread dispEventThread_;

    void fodPressStatusThread() {
        LOG(INFO) << "FOD press status thread started";
        
        android::base::unique_fd fd(open(FOD_PRESS_STATUS_PATH, O_RDONLY));
        if (fd.get() < 0) {
            LOG(ERROR) << "Failed to open " << FOD_PRESS_STATUS_PATH << ", err: " << fd.get();
            return;
        }

        struct pollfd fodPressStatusPoll = {
                .fd = fd.get(),
                .events = POLLERR | POLLPRI,
                .revents = 0,
        };

        while (!shouldStop_.load()) {
            int rc = poll(&fodPressStatusPoll, 1, 1000);  // 1 second timeout
            if (rc < 0) {
                LOG(ERROR) << "Failed to poll " << FOD_PRESS_STATUS_PATH << ", err: " << rc;
                continue;
            }
            
            if (rc == 0) {
                // Timeout, check shouldStop flag
                continue;
            }
            
            const bool pressed = readBool(fd.get());
            LOG(DEBUG) << "fod_press_status changed: " << (pressed ? "pressed" : "released");
            setFingerDown(pressed);
        }
        
        LOG(INFO) << "FOD press status thread stopped";
    }

    void dispEventThread() {
        LOG(INFO) << "Display event thread started";
        
        android::base::unique_fd fd(open(DISP_FEATURE_PATH, O_RDWR));
        if (fd.get() < 0) {
            LOG(ERROR) << "Failed to open " << DISP_FEATURE_PATH << ", err: " << fd.get();
            return;
        }

        // Register for FOD events
        disp_event_req req;
        req.base.flag = 0;
        req.base.disp_id = MI_DISP_PRIMARY;
        req.type = MI_DISP_EVENT_FOD;
        int rc = ioctl(fd.get(), MI_DISP_IOCTL_REGISTER_EVENT, &req);
        if (rc < 0) {
            LOG(ERROR) << "Failed to register for display events: " << rc;
            return;
        }

        struct pollfd dispEventPoll = {
                .fd = fd.get(),
                .events = POLLIN,
                .revents = 0,
        };

        while (!shouldStop_.load()) {
            rc = poll(&dispEventPoll, 1, 1000);  // 1 second timeout
            if (rc < 0) {
                LOG(ERROR) << "Failed to poll display events, err: " << rc;
                continue;
            }
            
            if (rc == 0) {
                // Timeout, check shouldStop flag
                continue;
            }

            struct disp_event_resp* response = parseDispEvent(fd.get());
            if (response == nullptr) {
                continue;
            }

            if (response->base.type != MI_DISP_EVENT_FOD) {
                LOG(ERROR) << "Unexpected display event: " << response->base.type;
                continue;
            }

            int value = response->data[0];
            LOG(DEBUG) << "Received data: " << std::bitset<8>(value);

            bool localHbmUiReady = value & LOCAL_HBM_UI_READY;

            fingerprint_device_t* device;
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                device = mDevice_;
            }
            
            if (device != nullptr) {
                device->extCmd(device, COMMAND_NIT,
                              localHbmUiReady ? PARAM_NIT_FOD : PARAM_NIT_NONE);
            }
        }
        
        LOG(INFO) << "Display event thread stopped";
    }

    void setFodStatus(int value) {
        if (touch_fd_.get() < 0) {
            LOG(ERROR) << "Touch device not available";
            return;
        }
        
        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, value};
        int rc = ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);
        if (rc < 0) {
            LOG(ERROR) << "Failed to set FOD status: " << rc;
        }
    }

    void setFingerDown(bool pressed) {
        if (touch_fd_.get() < 0) {
            LOG(ERROR) << "Touch device not available";
            return;
        }
        
        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_FOD_DOWNUP_CTL, pressed ? 1 : 0};
        int rc = ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);
        if (rc < 0) {
            LOG(ERROR) << "Failed to set finger down status: " << rc;
        }
        
        // Request HBM
        if (disp_fd_.get() >= 0) {
            disp_local_hbm_req req;
            req.base.flag = 0;
            req.base.disp_id = MI_DISP_PRIMARY;
            req.local_hbm_value = pressed ? LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT
                                          : LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
            rc = ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req);
            if (rc < 0) {
                LOG(ERROR) << "Failed to set HBM: " << rc;
            }
        }
        
        fingerprint_device_t* device;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            device = mDevice_;
        }
        
        if (device != nullptr) {
            device->extCmd(device, COMMAND_FOD_PRESS_STATUS,
                          pressed ? PARAM_FOD_PRESSED : PARAM_FOD_RELEASED);
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