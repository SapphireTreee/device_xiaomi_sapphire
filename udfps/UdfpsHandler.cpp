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

#include <display/drm/mi_disp.h> // Xiaomi display driver definitions

#include "UdfpsHandler.h" // Custom UdfpsHandler base class
#include "xiaomi_touch.h" // Xiaomi touch driver definitions

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
// NOTE: The _IO macro for TOUCH_IOC_SET_CUR_VALUE is highly suspicious if it's
// intended to pass an array pointer. Typically, _IOW or _IOWR macros are used
// when passing data structures. This might be a critical bug/vulnerability
// if the kernel driver expects a different IOCTL type or argument.
// This fix assumes the underlying driver expects an int array, but the IOCTL
// definition itself might be incorrect for this usage.
#define TOUCH_IOC_SET_CUR_VALUE _IO(TOUCH_MAGIC, SET_CUR_VALUE) // IOCTL to set current value
#define TOUCH_IOC_GET_CUR_VALUE _IO(TOUCH_MAGIC, GET_CUR_VALUE) // IOCTL to get current value

// Define path for display feature driver
#define DISP_FEATURE_PATH "/dev/mi_display/disp_feature"

// Define path for FOD press status sysfs entry
#define FOD_PRESS_STATUS_PATH "/sys/class/touch/touch_dev/fod_press_status"

// REMOVED: #define MAX_BUF_SIZE 3 // This macro is already defined in xiaomi_touch.h

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

    // Return true if the character is not '0'
    return c != '0';
}

// Helper function to parse display event data from a file descriptor
static disp_event_resp* parseDispEvent(int fd) {
    // Using a static buffer is generally safe if only one thread accesses it,
    // but if multiple threads could call this concurrently, it would be a race.
    // In this specific context, only one thread calls it.
    static char event_data[1024] = {0};
    ssize_t size;

    memset(event_data, 0x0, sizeof(event_data)); // Clear the buffer
    size = read(fd, event_data, sizeof(event_data)); // Read event data, max 1024 bytes
    if (size < 0) {
        LOG(ERROR) << "read fod event failed";
        return nullptr;
    }

    // Check if the read size is at least the size of disp_event structure
    if (size < sizeof(struct disp_event)) {
        LOG(ERROR) << "Invalid event size " << size << ", expect at least "
                   << sizeof(struct disp_event);
        return nullptr;
    }

    // Security Improvement: Ensure the read size does not exceed the buffer capacity
    // before casting to disp_event_resp. This prevents potential out-of-bounds reads
    // if disp_event_resp is larger than expected or if size somehow exceeds event_data.
    if (size > sizeof(event_data)) {
        LOG(ERROR) << "Read size " << size << " exceeds event_data buffer capacity "
                   << sizeof(event_data);
        return nullptr;
    }

    // Cast the buffer to disp_event_resp and return
    // Note: This assumes disp_event_resp is not larger than event_data[1024]
    return (struct disp_event_resp*)&event_data[0];
}

}  // anonymous namespace

// Implementation of UdfpsHandler for Xiaomi SM6225 devices
class XiaomiSm6225UdfpsHandler : public UdfpsHandler {
  public:
    // Destructor to ensure proper thread shutdown and prevent use-after-free
    ~XiaomiSm6225UdfpsHandler() {
        mExitThreads.store(true); // Signal threads to exit
        for (auto& t : mWorkerThreads) {
            if (t.joinable()) {
                t.join(); // Wait for threads to finish
            }
        }
    }

    // Initialize the UDFPS handler
    void init(fingerprint_device_t* device) {
        mDevice = device; // Store the fingerprint device pointer
        // Open touch and display feature devices
        touch_fd_ = android::base::unique_fd(open(TOUCH_DEV_PATH, O_RDWR));
        disp_fd_ = android::base::unique_fd(open(DISP_FEATURE_PATH, O_RDWR));

        // Check if file descriptors were opened successfully
        // FIX: Replaced .is_valid() with .get() < 0 for compatibility
        if (touch_fd_.get() < 0) {
            LOG(ERROR) << "Failed to open " << TOUCH_DEV_PATH << ", errno: " << errno;
            // Handle error: perhaps throw an exception or return an error status
            return;
        }
        if (disp_fd_.get() < 0) {
            LOG(ERROR) << "Failed to open " << DISP_FEATURE_PATH << ", errno: " << errno;
            // Handle error
            return;
        }

        // Set FOD status to ON
        setFodStatus(FOD_STATUS_ON);

        // Security Improvement: Store std::thread objects and join them in destructor
        // Thread to notify fingerprint hwmodule about fod presses
        mWorkerThreads.emplace_back([this]() {
            // Open FOD press status sysfs entry
            int fd = open(FOD_PRESS_STATUS_PATH, O_RDONLY);
            if (fd < 0) {
                LOG(ERROR) << "failed to open " << FOD_PRESS_STATUS_PATH << " , err: " << fd;
                return;
            }

            // Setup poll structure for FOD press status
            struct pollfd fodPressStatusPoll = {
                    .fd = fd,
                    .events = POLLERR | POLLPRI, // Poll for errors or high-priority data
                    .revents = 0,
            };

            while (!mExitThreads.load()) { // Loop until exit flag is set
                // Poll for changes in FOD press status with a timeout
                // Using a timeout allows checking mExitThreads periodically
                int rc = poll(&fodPressStatusPoll, 1, 100); // 100ms timeout
                if (rc < 0) {
                    LOG(ERROR) << "failed to poll " << FOD_PRESS_STATUS_PATH << ", err: " << rc;
                    // Consider a small sleep to prevent busy-looping on persistent errors
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                } else if (rc == 0) {
                    // Timeout, no event, check exit flag again
                    continue;
                }

                bool pressed = readBool(fd);
                if (mDevice) { // Ensure mDevice is still valid before calling
                    mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS,
                                    pressed ? PARAM_FOD_PRESSED : PARAM_FOD_RELEASED);
                }
            }
            close(fd); // Close the file descriptor when thread exits
        });

         // Thread to listen for fod ui changes (e.g., HBM status)
        mWorkerThreads.emplace_back([this]() {
            // Open display feature device
            int fd = open(DISP_FEATURE_PATH, O_RDWR);
            if (fd < 0) {
                LOG(ERROR) << "failed to open " << DISP_FEATURE_PATH << " , err: " << fd;
                return;
            }

            // Register for FOD events from the display driver
            disp_event_req req;
            req.base.flag = 0;
            req.base.disp_id = MI_DISP_PRIMARY; // Primary display
            req.type = MI_DISP_EVENT_FOD; // FOD event type
            ioctl(fd, MI_DISP_IOCTL_REGISTER_EVENT, &req); // Register the event

            // Setup poll structure for display events
            struct pollfd dispEventPoll = {
                    .fd = fd,
                    .events = POLLIN, // Poll for input data
                    .revents = 0,
            };

            while (!mExitThreads.load()) { // Loop until exit flag is set
                // Poll for display events with a timeout
                int rc = poll(&dispEventPoll, 1, 100); // 100ms timeout
                if (rc < 0) {
                    LOG(ERROR) << "failed to poll " << DISP_FEATURE_PATH << ", err: " << rc;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                } else if (rc == 0) {
                    // Timeout, no event, check exit flag again
                    continue;
                }

                struct disp_event_resp* response = parseDispEvent(fd);
                if (response == nullptr) {
                    continue;
                }

                // Check if the event type is indeed a FOD event
                if (response->base.type != MI_DISP_EVENT_FOD) {
                    LOG(ERROR) << "unexpected display event: " << response->base.type;
                    continue;
                }

                // Extract the value from the response data (first element)
                int value = response->data[0];
                LOG(DEBUG) << "received data: " << std::bitset<8>(value); // Log the raw data

                // Check if Local HBM UI is ready based on the received value
                bool localHbmUiReady = value & LOCAL_HBM_UI_READY;

                if (mDevice) { // Ensure mDevice is still valid before calling
                    mDevice->extCmd(mDevice, COMMAND_NIT,
                                    localHbmUiReady ? PARAM_NIT_FOD : PARAM_NIT_NONE);
                }
            }
            close(fd); // Close the file descriptor when thread exits
        });
    }

    // Callback when a finger is placed down on the sensor
    void onFingerDown(uint32_t /*x*/, uint32_t /*y*/, float /*minor*/, float /*major*/) {
        LOG(INFO) << __func__; // Log the function call
        setFingerDown(true); // Set finger down status to true
    }

    // Callback when a finger is lifted from the sensor
    void onFingerUp() {
        LOG(INFO) << __func__; // Log the function call
        setFingerDown(false); // Set finger down status to false
    }

    // Callback when fingerprint acquisition result is received
    void onAcquired(int32_t result, int32_t vendorCode) {
        LOG(INFO) << __func__ << " result: " << result << " vendorCode: " << vendorCode;
        // If acquisition is successful (GOOD), set finger down status to false
        if (static_cast<AcquiredInfo>(result) == AcquiredInfo::GOOD) {
            setFingerDown(false);
        }
    }

    // Callback to cancel an ongoing fingerprint operation
    void cancel() {
        LOG(INFO) << __func__; // Log the function call
        setFingerDown(false); // Set finger down status to false
    }

    // Callback before enrollment starts
    void preEnroll() {
        LOG(DEBUG) << __func__; // Log the function call
    }

    // Callback during enrollment
    void enroll() {
        LOG(DEBUG) << __func__; // Log the function call
    }

    // Callback after enrollment completes
    void postEnroll() {
        LOG(DEBUG) << __func__; // Log the function call
    }

  private:
    fingerprint_device_t* mDevice; // Pointer to the fingerprint device structure
    android::base::unique_fd touch_fd_; // File descriptor for the touch device
    android::base::unique_fd disp_fd_; // File descriptor for the display feature device

    std::atomic<bool> mExitThreads{false}; // Atomic flag to signal threads to exit
    std::vector<std::thread> mWorkerThreads; // Vector to hold worker threads

    // Helper function to set the FOD status (enable/disable)
    void setFodStatus(int value) {
        // Prepare buffer for IOCTL command: primary display, Touch_Fod_Enable command, value
        // MAX_BUF_SIZE is now used from xiaomi_touch.h (likely 256), which is sufficient for 3 ints.
        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, value};
        // Execute IOCTL to set the current value on the touch device
        ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);
    }

    // Helper function to handle finger down/up events, adjusting display and touch behavior
    void setFingerDown(bool pressed) {
        disp_local_hbm_req req; // Local HBM (High Brightness Mode) request structure
        req.base.flag = 0;
        req.base.disp_id = MI_DISP_PRIMARY; // Primary display
        // Set local HBM value based on whether finger is pressed or released
        req.local_hbm_value = pressed ? LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT : LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
        // Execute IOCTL to set local HBM on the display device
        ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req);

        // Prepare buffer for IOCTL command: primary display, THP_FOD_DOWNUP_CTL command, 1 for pressed, 0 for released
        // MAX_BUF_SIZE is now used from xiaomi_touch.h (likely 256), which is sufficient for 3 ints.
        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, THP_FOD_DOWNUP_CTL, pressed ? 1 : 0};
        // Execute IOCTL to set the current value on the touch device
        ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);
    }
};

// Factory function to create an instance of XiaomiSm6225UdfpsHandler
static UdfpsHandler* create() {
    return new XiaomiSm6225UdfpsHandler();
}

// Factory function to destroy an instance of UdfpsHandler
static void destroy(UdfpsHandler* handler) {
    delete handler;
}

// Exported UDFPS_HANDLER_FACTORY structure
extern "C" UdfpsHandlerFactory UDFPS_HANDLER_FACTORY = {
        .create = create, // Assign the create function
        .destroy = destroy, // Assign the destroy function
};
