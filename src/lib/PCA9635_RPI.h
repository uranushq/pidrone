#ifndef PCA9635_RPI_H
#define PCA9635_RPI_H

#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <string.h>

#define HIGH 1
#define LOW  0

// Register definitions
const uint8_t PCA9635_MODE1      = 0x00;
const uint8_t PCA9635_MODE2      = 0x01;
const uint8_t PCA9635_PWM0       = 0x02;
const uint8_t PCA9635_PWM15      = 0x11;
const uint8_t PCA9635_GRPPWM     = 0x12;
const uint8_t PCA9635_GRPFREQ    = 0x13;
const uint8_t PCA9635_LEDOUT0    = 0x14;
const uint8_t PCA9635_LEDOUT3    = 0x17;

// MODE2
const uint8_t PCA9635_MODE2_OUTDRV = 0x04;

// LED states
const uint8_t PCA9635_LED_OFF   = 0x00;
const uint8_t PCA9635_LED_ON    = 0x01;
const uint8_t PCA9635_LED_PWM   = 0x02;
const uint8_t PCA9635_LED_GROUP = 0x03;

class PCA9635 {
public:
    PCA9635(uint8_t address);
    ~PCA9635();

    bool begin();
    void digitalWrite(uint8_t ledNum, bool value);
    void analogWrite(uint8_t ledNum, uint8_t pwm);
    void setLEDState(uint8_t ledNum, uint8_t state);
    void setLEDPWM(uint8_t ledNum, uint8_t pwm);
    void setGroupPWM(uint8_t pwm);
    void setGroupFrequency(uint8_t freq);
    void setMode1(uint8_t config);
    void setMode2(uint8_t config);
    uint8_t getRegister(uint8_t regAddr);
    bool setRegister(uint8_t regAddr, uint8_t value);

private:
    int i2c_fd;
    uint8_t _i2cAddr;
    void updateLEDOUTRegister(uint8_t ledNum, uint8_t state);
};

#endif // PCA9635_RPI_H
