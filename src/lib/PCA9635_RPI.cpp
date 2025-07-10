#include "PCA9635_RPI.h"
#include <iostream>

PCA9635::PCA9635(uint8_t address) {
    _i2cAddr = address;
    i2c_fd = -1;
}

PCA9635::~PCA9635() {
    if (i2c_fd >= 0) {
        close(i2c_fd);
    }
}

bool PCA9635::begin() {
    const char *filename = "/dev/i2c-1";

    if ((i2c_fd = open(filename, O_RDWR)) < 0) {
        perror("Failed to open I2C bus");
        return false;
    }

    if (ioctl(i2c_fd, I2C_SLAVE, _i2cAddr) < 0) {
        perror("Failed to connect to I2C device");
        return false;
    }

    // MODE1 - normal mode
    if (!setRegister(PCA9635_MODE1, 0x00)) return false;
    usleep(500); // stabilize

    // MODE2 - Totem pole
    if (!setRegister(PCA9635_MODE2, PCA9635_MODE2_OUTDRV)) return false;

    // Turn all LEDs OFF
    for (uint8_t reg = PCA9635_LEDOUT0; reg <= PCA9635_LEDOUT3; reg++) {
        if (!setRegister(reg, 0x00)) return false;
    }

    // Set all PWM to 0
    for (uint8_t reg = PCA9635_PWM0; reg <= PCA9635_PWM15; reg++) {
        if (!setRegister(reg, 0x00)) return false;
    }

    // Set Group PWM and Freq
    if (!setRegister(PCA9635_GRPPWM, 0xFF)) return false;
    if (!setRegister(PCA9635_GRPFREQ, 0x00)) return false;

    return true;
}

bool PCA9635::setRegister(uint8_t regAddr, uint8_t value) {
    uint8_t buf[2] = {regAddr, value};
    if (write(i2c_fd, buf, 2) != 2) {
        perror("I2C Write failed");
        return false;
    }
    return true;
}

uint8_t PCA9635::getRegister(uint8_t regAddr) {
    if (write(i2c_fd, &regAddr, 1) != 1) {
        perror("I2C Read (write phase) failed");
        return 0;
    }

    uint8_t data;
    if (read(i2c_fd, &data, 1) != 1) {
        perror("I2C Read (read phase) failed");
        return 0;
    }

    return data;
}

void PCA9635::updateLEDOUTRegister(uint8_t ledNum, uint8_t state) {
    if (ledNum > 15 || state > 3) return;

    uint8_t reg = PCA9635_LEDOUT0 + (ledNum / 4);
    uint8_t shift = (ledNum % 4) * 2;
    uint8_t mask = 0x03 << shift;

    uint8_t current = getRegister(reg);
    uint8_t newVal = (current & ~mask) | (state << shift);

    setRegister(reg, newVal);
}

void PCA9635::digitalWrite(uint8_t ledNum, bool value) {
    updateLEDOUTRegister(ledNum, value ? PCA9635_LED_ON : PCA9635_LED_OFF);
}

void PCA9635::analogWrite(uint8_t ledNum, uint8_t pwm) {
    setLEDPWM(ledNum, pwm);
}

void PCA9635::setLEDState(uint8_t ledNum, uint8_t state) {
    updateLEDOUTRegister(ledNum, state);
}

void PCA9635::setLEDPWM(uint8_t ledNum, uint8_t pwm) {
    if (ledNum > 15) return;
    updateLEDOUTRegister(ledNum, PCA9635_LED_PWM);
    setRegister(PCA9635_PWM0 + ledNum, pwm);
}

void PCA9635::setGroupPWM(uint8_t pwm) {
    setRegister(PCA9635_GRPPWM, pwm);
}

void PCA9635::setGroupFrequency(uint8_t freq) {
    setRegister(PCA9635_GRPFREQ, freq);
}

void PCA9635::setMode1(uint8_t config) {
    setRegister(PCA9635_MODE1, config);
}

void PCA9635::setMode2(uint8_t config) {
    setRegister(PCA9635_MODE2, config);
}
