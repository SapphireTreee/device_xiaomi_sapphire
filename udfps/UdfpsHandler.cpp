/*
 * Copyright (C) 2022-2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "UdfpsHandler.xiaomi_sm6225" // Define the log tag for logging messages

#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h> // AIDL interface for fingerprint HAL
#include <android-base/logging.h> // Android logging utilities
#include <android-base/unique_fd.h> // Utility for managing file descriptors

#include <poll.h> // For polling file descriptors
#include <sys/ioctl.h> // For device I/O control operations
#include <fstream> // For file stream operations (though not directly used in the provided snippet, often useful)
#include <thread> // For creating new threads
#include <atomic> // For atomic flags to control thread termination
#include <vector> // To store std::thread objects for joining
#include <chrono> // For std::chrono::milliseconds
#include <bitset> // For std::bitset to print raw event data

#include <display/drm/mi_disp.h> // Xiaomi display driver definitions

#include "UdfpsHandler.h" // Custom UdfpsHandler base class
#include "xiaomi_touch.h" // Xiaomi touch driver definitions (provides MAX_BUF_SIZE, MODE_CMD, MODE_TYPE)

// Define commands and parameters for display NIT (brightness) control
#define COMMAND_NIT 10
#define PARAM_NIT_FOD 1 // Parameter for enabling FOD NIT (high brightness)
#define PARAM_NIT_NONE 0 // Parameter for disabling FOD NIT

// Define commands and parameters for FOD press status
#define COMMAND_FOD_PRESS_STATUS 1
#define PARAM_FOD_PRESSED 1 // Parameter indicating finger is pressed
#define PARAM_FOD_RELEASED 0 // Parameter indicating finger is released

// Define FOD status values
#define FOD_STATUS_OFF 0
#define FOD_STATUS_ON 1

// Define paths and IOCTL commands for Xiaomi touch driver
#define TOUCH_DEV_PATH "/dev/xiaomi-touch"
#define TOUCH_MAGIC 'T' // Magic number for touch IOCTLs

// Reverting to original definition as per user request.
// NOTE: As discussed previously, using _IO for an IOCTL that passes a pointer to data
// (like 'buf' in setFodStatus and setFingerDown) is often incorrect and true behavior
// depends on kernel implementation. If issues arise, verify kernel's IOCTL definition.
#define TOUCH_IOC_SET_CUR_VALUE _IO(TOUCH_MAGIC, SET_CUR_VALUE) // IOCTL to set current value
#define TOUCH_IOC_GET_CUR_VALUE _IO(TOUCH_MAGIC, GET_CUR_VALUE) // IOCTL to get current value

#define DISP_FEATURE_PATH "/dev/mi_display/disp_feature"

#define FOD_PRESS_STATUS_PATH "/sys/class/touch/touch_dev/fod_press_status"

// MAX_BUF_SIZE is now correctly taken from xiaomi_touch.h

using ::aidl::android::hardware::biometrics::fingerprint::AcquiredInfo; // Alias for AcquiredInfo enum

namespace { // Anonymous namespace for helper functions

// Helper function to read a boolean value from a file descriptor
static bool readBool(int fd) {
    char c;
    int rc;

    // Seek to the beginning of the file descriptor
    rc = lseek(fd, 0, SEEK_SET);
    if (rc) {
        LOG(ERROR) << "failed to seek fd, err: " << rc;
        return false;
    }

    // Read a single character
    rc = read(fd, &c, sizeof(char));
    if (rc != 1) {
        LOG(ERROR) << "failed to read bool from fd, err: " << rc;
        return false;
    }

    return c != '0';
}

// Helper function to parse display event data from a file descriptor
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

    if (size > sizeof(event_data)) {
        LOG(ERROR) << "Read size " << size << " exceeds event_data buffer capacity "
                   << sizeof(event_data);
        return nullptr;
    }

    return (struct disp_event_resp*)&event_data[0];
}

}  // anonymous namespace

// Implementation of UdfpsHandler for Xiaomi SM6225 devices
class XiaomiSm6225UdfpsHandler : public UdfpsHandler {
  public:
    // Destructor to ensure proper thread shutdown and preventing use-after-free
    ~XiaomiSm6225UdfpsHandler() {
        mExitThreads.store(true); // Signal threads to exit
        for (auto& t : mWorkerThreads) {
            if (t.joinable()) {
                t.join(); // Wait for threads to finish
            }
        }
        // Set FOD status to OFF when the HAL is destroyed
        LOG(INFO) << "Setting FOD status to OFF during HAL destruction.";
        setFodStatus(FOD_STATUS_OFF);
    }

    // Initialize the UDFPS handler
    void init(fingerprint_device_t* device) {
        mDevice = device; // Store the fingerprint device pointer
        // Open touch and display feature devices
        touch_fd_ = android::base::unique_fd(open(TOUCH_DEV_PATH, O_RDWR));
        disp_fd_ = android::base::unique_fd(open(DISP_FEATURE_PATH, O_RDWR));

        // Check if file descriptors were opened successfully
        if (touch_fd_.get() < 0) {
            LOG(ERROR) << "Failed to open " << TOUCH_DEV_PATH << ", errno: " << errno;
            return;
        }
        if (disp_fd_.get() < 0) {
            LOG(ERROR) << "Failed to open " << DISP_FEATURE_PATH << ", errno: " << errno;
            return;
        }

        // Set FOD status to ON
        LOG(INFO) << "Setting FOD status to ON during HAL init.";
        setFodStatus(FOD_STATUS_ON);

        // Thread to notify fingerprint hwmodule about fod presses
        mWorkerThreads.emplace_back([this]() {
            int fd = open(FOD_PRESS_STATUS_PATH, O_RDONLY);
            if (fd < 0) {
                LOG(ERROR) << "failed to open " << FOD_PRESS_STATUS_PATH << " , err: " << fd;
                return;
            }

            struct pollfd fodPressStatusPoll = {
                    .fd = fd,
                    .events = POLLERR | POLLPRI,
                    .revents = 0,
            };

            while (!mExitThreads.load()) {
                int rc = poll(&fodPressStatusPoll, 1, 100);
                if (rc < 0) {
                    LOG(ERROR) << "failed to poll " << FOD_PRESS_STATUS_PATH << ", err: " << rc;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                } else if (rc == 0) {
                    continue;
                }

                bool pressed = readBool(fd);
                LOG(INFO) << "FOD_PRESS_STATUS_PATH reports: " << (pressed ? "PRESSED" : "RELEASED");

                // Only report fingerprint touch events if a fingerprint operation is active
                if (mIsFingerprintOperationActive.load()) {
                    if (mDevice) {
                        mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS,
                                        pressed ? PARAM_FOD_PRESSED : PARAM_FOD_RELEASED);
                        LOG(INFO) << "Sent COMMAND_FOD_PRESS_STATUS with param: " << (pressed ? "PARAM_FOD_PRESSED" : "PARAM_FOD_RELEASED");
                    }
                } else {
                    // Log if a touch is detected but not reported due to inactive operation
                    if (pressed) {
                        LOG(DEBUG) << "FOD touch detected but ignored (operation not active).";
                    }
                }
            }
            close(fd);
        });

         // Thread to listen for fod ui changes
        mWorkerThreads.emplace_back([this]() {
            int fd = open(DISP_FEATURE_PATH, O_RDWR);
            if (fd < 0) {
                LOG(ERROR) << "failed to open " << DISP_FEATURE_PATH << " , err: " << fd;
                return;
            }

            // Register for FOD events
            disp_event_req req;
            req.base.flag = 0;
            req.base.disp_id = MI_DISP_PRIMARY;
            req.type = MI_DISP_EVENT_FOD;
            ioctl(fd, MI_DISP_IOCTL_REGISTER_EVENT, &req);

            struct pollfd dispEventPoll = {
                    .fd = fd,
                    .events = POLLIN,
                    .revents = 0,
            };

            while (!mExitThreads.load()) {
                int rc = poll(&dispEventPoll, 1, 100);
                if (rc < 0) {
                    LOG(ERROR) << "failed to poll " << DISP_FEATURE_PATH << ", err: " << rc;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                } else if (rc == 0) {
                    continue;
                }

                struct disp_event_resp* response = parseDispEvent(fd);
                if (response == nullptr) {
                    continue;
                }

                if (response->base.type != MI_DISP_EVENT_FOD) {
                    LOG(ERROR) << "unexpected display event: " << response->base.type;
                    continue;
                }

                int value = response->data[0];
                bool localHbmUiReady = value & LOCAL_HBM_UI_READY;
                LOG(INFO) << "Received MI_DISP_EVENT_FOD. Raw data: " << std::bitset<8>(value)
                          << ", LOCAL_HBM_UI_READY: " << (localHbmUiReady ? "TRUE" : "FALSE");

                if (mDevice) {
                    mDevice->extCmd(mDevice, COMMAND_NIT,
                                    localHbmUiReady ? PARAM_NIT_FOD : PARAM_NIT_NONE);
                    LOG(INFO) << "Sent COMMAND_NIT with param: " << (localHbmUiReady ? "PARAM_NIT_FOD (HBM ON)" : "PARAM_NIT_NONE (HBM OFF)");
                }
            }
            close(fd);
        });
    }

    // New method: Called by the framework when an authentication operation is started.
    void authenticate(uint64_t /*operationId*/, int32_t /*groupId*/) {
        LOG(INFO) << __func__ << ": Authentication started. Enabling touch reporting.";
        mIsFingerprintOperationActive.store(true); // Proactively enable reporting
    }

    void onFingerDown(uint32_t /*x*/, uint32_t /*y*/, float /*minor*/, float /*major*/) {
        LOG(INFO) << __func__ << ": Finger DOWN detected. Activating HBM.";
        // mIsFingerprintOperationActive is now set in authenticate/preEnroll
        setFingerDown(true);
    }

    void onFingerUp() {
        LOG(INFO) << __func__ << ": Finger UP detected. Deactivating HBM and disabling touch reporting.";
        mIsFingerprintOperationActive.store(false); // Disable reporting of FOD touches
        setFingerDown(false);
    }

    void onAcquired(int32_t result, int32_t vendorCode) {
        LOG(INFO) << __func__ << " result: " << result << " vendorCode: " << vendorCode;
        if (static_cast<AcquiredInfo>(result) == AcquiredInfo::GOOD) {
            LOG(INFO) << __func__ << ": AcquiredInfo::GOOD. Deactivating HBM and disabling touch reporting.";
            mIsFingerprintOperationActive.store(false); // Disable reporting of FOD touches
            setFingerDown(false);
        } else {
            LOG(INFO) << __func__ << ": AcquiredInfo NOT GOOD. Keeping touch reporting active for retry.";
            // If result is not GOOD, the operation is likely still ongoing (e.g., retry needed).
            // Keep mIsFingerprintOperationActive true. It will be disabled on onFingerUp or cancel.
        }
    }

    void cancel() {
        LOG(INFO) << __func__ << ": Operation cancelled. Deactivating HBM and disabling touch reporting.";
        mIsFingerprintOperationActive.store(false); // Disable reporting of FOD touches
        setFingerDown(false);
    }

    void preEnroll() {
        LOG(DEBUG) << __func__ << ": Pre-enrollment started. Enabling touch reporting.";
        mIsFingerprintOperationActive.store(true); // Proactively enable reporting for enrollment
    }

    void enroll() {
        LOG(DEBUG) << __func__ << ": Enrollment in progress.";
        // mIsFingerprintOperationActive is already set by preEnroll
    }

    void postEnroll() {
        LOG(DEBUG) << __func__ << ": Post-enrollment. Disabling touch reporting.";
        mIsFingerprintOperationActive.store(false); // Ensure touch reporting is off after enrollment session
    }

  private:
    fingerprint_device_t* mDevice;
    android::base::unique_fd touch_fd_;
    android::base::unique_fd disp_fd_;

    std::atomic<bool> mExitThreads{false};
    std::vector<std::thread> mWorkerThreads;
    std::atomic<bool> mIsFingerprintOperationActive{false}; // New flag to control touch reporting

    void setFodStatus(int value) {
        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, value};
        ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);
        LOG(INFO) << "setFodStatus called with value: " << value;
    }

    void setFingerDown(bool pressed) {
        disp_local_hbm_req req;
        req.base.flag = 0;
        req.base.disp_id = MI_DISP_PRIMARY;
        req.local_hbm_value = pressed ? LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT : LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
        ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req);
        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_FOD_DOWNUP_CTL, pressed ? 1 : 0};
        ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);
        LOG(INFO) << "setFingerDown called with pressed: " << (pressed ? "TRUE (HBM ON)" : "FALSE (HBM OFF)");
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
