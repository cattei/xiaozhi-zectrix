#ifndef I2C_BUS_LOCK_H
#define I2C_BUS_LOCK_H

#include <esp_err.h>
#include <mutex>

// 同一条 I2C 总线上的 NFC 与 RTC 需要互斥访问
class ScopedI2cBusLock {
public:
    explicit ScopedI2cBusLock(const char* reason = nullptr) {
        i2c_bus_mutex().lock();
        locked_ = true;
        status_ = ESP_OK;
        (void)reason;
    }
    ~ScopedI2cBusLock() {
        if (locked_) {
            i2c_bus_mutex().unlock();
        }
    }
    ScopedI2cBusLock(const ScopedI2cBusLock&) = delete;
    ScopedI2cBusLock& operator=(const ScopedI2cBusLock&) = delete;

    bool locked() const { return locked_; }
    esp_err_t status() const { return status_; }

private:
    static std::recursive_mutex& i2c_bus_mutex() {
        static std::recursive_mutex m;
        return m;
    }
    bool locked_ = false;
    esp_err_t status_ = ESP_OK;
};

#endif  // I2C_BUS_LOCK_H
