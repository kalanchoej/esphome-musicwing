#include "vs10xx_base.h"
#include "esphome/core/log.h"

namespace esphome {
namespace vs10xx_base {

VS10XXBase::VS10XXBase(const char* log_tag, const Chipset supported_chipset): log_tag_(log_tag), supported_chipset_version_(supported_chipset) {}


void VS10XXBase::setup() {
  this->dreq_pin_->setup();
  this->xdcs_pin_->setup();
  this->xcs_pin_->setup();
  if (this->reset_pin_ != nullptr) {
    this->reset_pin_->setup();
  }
}

void VS10XXBase::dump_config() {
  ESP_LOGCONFIG(this->log_tag_, "  XCS Pin: %s", this->xcs_pin_->dump_summary().c_str());
  ESP_LOGCONFIG(this->log_tag_, "  XDCS Pin: %s", this->xdcs_pin_->dump_summary().c_str());
  ESP_LOGCONFIG(this->log_tag_, "  DREQ Pin: %s", this->dreq_pin_->dump_summary().c_str());
  if (this->reset_pin_ == nullptr) {
    ESP_LOGCONFIG(this->log_tag_, "  RESET Pin: N/A");
  } else {
    ESP_LOGCONFIG(this->log_tag_, "  RESET Pin: %s", this->reset_pin_->dump_summary().c_str());
  }
}

void VS10XXBase::loop() {
  switch (this->state_) {
    case VS10XX_INIT:
      this->state_init_();
      break;
    case VS10XX_RESET:
      this->state_reset_();
      break;
    case VS10XX_SETUP_SLOW_SPI:
      this->state_setup_slow_spi_();
      break;
    case VS10XX_SETUP_FAST_SPI:
      this->state_setup_fast_spi_();
      break;
    case VS10XX_REPORT_FAILED:
      this->state_report_failed_();
      break;
    case VS10XX_READY:
      // NOOP
      break;
    case VS10XX_FAILED:
      // NOOP
      break;
  }
}

void VS10XXBase::state_init_() {
  ESP_LOGI(this->log_tag_, "Initializing device");

  // Disable both the command and data SPI channel.
  this->xcs_pin_->digital_write(true);
  this->xdcs_pin_->digital_write(true);

  this->to_state_(VS10XX_RESET);
}

void VS10XXBase::state_reset_() {
  // Sanity check in case no reset pin is defined.
  if (!this->data_request_ready_() && this->reset_pin_ == nullptr) {
    ESP_LOGE(this->log_tag_, "DREQ not pulled HIGH by the device and no reset pin defined");
    ESP_LOGE(this->log_tag_, "Did you forget to pull up the reset pin, to boot the device?");
    this->to_state_(VS10XX_REPORT_FAILED); 
    return;
  }
  
  // Perform a hard reset, when the reset pin has been defined.
  this->hard_reset_();

  this->to_state_(VS10XX_SETUP_SLOW_SPI);
}

void VS10XXBase::state_setup_slow_spi_() {
  // Some basic communication tests to see if SPI is working in slow mode.
  this->spi_->go_slow();
  if (!this->test_communication_()) {
    this->to_state_(VS10XX_REPORT_FAILED); 
    return;
  }

  // Soft reset and see if SCI_MODE is set to the expected default value.
  this->soft_reset_();
  ESP_LOGD(this->log_tag_, "Checking default status of SCI_MODE");
  auto mode = this->read_register_(SCI_MODE);
  if (mode != SM_SDINEW) {
    ESP_LOGE(this->log_tag_, "SCI_MODE not SM_SDINEW after reset (value is %d)", mode);
    this->to_state_(VS10XX_REPORT_FAILED); 
    return;
  }

  // Check if the expected VS10XX chipset is in use.
  auto chipset = this->get_chipset_version_();
  if (chipset != this->supported_chipset_version_) {
    ESP_LOGE(this->log_tag_, "Unsupported chipset version: %d", chipset);
    this->to_state_(VS10XX_REPORT_FAILED); 
    return;
  }

  // Set device clock multiplier to the recommended value for typical use.
  // After this, we can safely use a SPI speed of 4MHz therefore.
  //
  // Note:
  // For VS1003 and VS1053, I see 0x9800 in both data sheets. They mean different things
  // for each chipset, but since the value is the same, I haven't moved this into
  // chipset-specific code for now.
  ESP_LOGD(this->log_tag_, "Configuring device to allow high speed SPI clock");
  this->write_register_(SCI_CLOCKF, 0x9800);

  this->to_state_(VS10XX_SETUP_FAST_SPI); 
}

void VS10XXBase::state_setup_fast_spi_() {
  // Some basic communication tests to see if SPI is working in fast mode.
  this->spi_->go_fast();
  if (!this->test_communication_()) {
    this->to_state_(VS10XX_REPORT_FAILED); 
    return;
  }

  // Setup the device audio.
  ESP_LOGD(this->log_tag_, "Turning on analog audio at 44.1kHz");
  this->write_register_(SCI_VOL, 0xffff);
  this->write_register_(SCI_AUDATA, 44101);

  // All is okay, the device can be used.
  this->to_state_(VS10XX_READY); 
  ESP_LOGI(this->log_tag_, "Device initialized successfully");
}

void VS10XXBase::state_report_failed_() {
    ESP_LOGE(this->log_tag_, "Device initialized failed");

    this->to_state_(VS10XX_FAILED);
}

void VS10XXBase::to_state_(State state) {
  this->state_ = state;
  this->state_timer_ = millis();
}

bool VS10XXBase::state_ms_passed_(uint32_t nr_of_ms) const {
  auto time_passed = millis() - this->state_timer_;
  return time_passed >= nr_of_ms;
}

uint8_t VS10XXBase::get_chipset_version_() {
  // From the datasheet:
  // SCI_STATUS register has SS_VER in bits 4:7
  auto status = this->read_register_(SCI_STATUS);
  auto version = (status & 0xf0) >> 4;
  return version;
}

bool VS10XXBase::test_communication_() {
  // The device must have pulled the DREQ pin high at this point.
  if (this->dreq_pin_->digital_read() == false) {
    ESP_LOGE(this->log_tag_, "DREQ is not HIGH, device connected correctly?");
    return false;
  }
  // Now test if we can write and read data over the
  // bus without errors. In fast SPI mode, we can perform more
  // write operations in the same time.
  auto step_size = this->spi_->is_fast() ? 15 : 300;
  auto cycles = 0;
  auto failures = 0;
  for (int value = 0; value < 0xFFFF; value += step_size) {
    cycles++;
    this->write_register_(SCI_VOL, value);
    auto read1 = this->read_register_(SCI_VOL);
    auto read2 = this->read_register_(SCI_VOL);
    if (value != read1 || value != read2) {
      failures++;
      ESP_LOGE(this->log_tag_, "SPI test failure after %d cycles; wrote %d, read back %d and %d",
               cycles, value, read1, read2);
      // Limit the number of reported failures.
      if (failures == 10) {
        break;
      }
    }
  }
  if (failures == 0) {
    ESP_LOGD(this->log_tag_, "SPI communication successful during %d write/read cycles", cycles);
    return true;
  } else {
    return false;
  }
}

void VS10XXBase::hard_reset_() {
  ESP_LOGD(this->log_tag_, "Hard resetting the device");
  if (this->reset_pin_ == nullptr) {
    ESP_LOGW(this->log_tag_, "Not performing hard reset, no reset pin defined");
    return;
  }
  // When the XRESET-signal is driven low, the device is reset.
  this->reset_pin_->digital_write(false);
  delay(1);
  this->reset_pin_->digital_write(true);
  // The datasheet specifies max 50000 XTALI cycles for boot initialization.
  // At the default XTALI of 12.288 MHz, this takes about 4ms.
  delay(5);
  // After initialization, the DREQ pin is pulled HIGH.
  this->wait_for_data_request_();
}

void VS10XXBase::soft_reset_() {
  ESP_LOGD(this->log_tag_, "Soft resetting the device");

  // Turn on "NEW MODE", which means that the two SPI chip select pins XCS and
  // XDCS are controlled independently. These can be used to flag the device
  // that respectively the serial commmand interface (SCI) or serial data
  // interface (SDI) must be activated on the SPI bus.
  // In "SHARED MODE", only the XCS pin is used for controlling both SCI (when
  // the pin is pulled LOW) and SDI (when the pin is pulled HIGH). While this
  // saves a GPIO on the MCU, this prevents using the SPI bus for any other
  // SPI devices, since the device will always be listening for either commands
  // or data.
  this->write_register_(SCI_MODE, SM_SDINEW | SM_RESET);

  // Wait for the device to become ready again.
  delay(2);
  this->wait_for_data_request_();
}

void VS10XXBase::write_register_(uint8_t reg, uint16_t value) {
  this->control_mode_on_();
  this->spi_->write_byte(2); // write command
  this->spi_->write_byte(reg);
  this->spi_->write_byte16(value);
  this->control_mode_off_();
  ESP_LOGVV(this->log_tag_, "write_register: 0x%02X: 0x%02X", reg, value);
}

uint16_t VS10XXBase::read_register_(uint8_t reg) {
  this->control_mode_on_();
  this->spi_->write_byte(3); // read command
  this->spi_->write_byte(reg);
  uint16_t value = this->spi_->read_byte() << 8 | this->spi_->read_byte();
  this->control_mode_off_();
  ESP_LOGVV(this->log_tag_, "read_register: 0x%02X: 0x%02X", reg, value);
  return value;
}

void VS10XXBase::control_mode_on_() {
  this->spi_->enable();
  this->xdcs_pin_->digital_write(true);
  this->xcs_pin_->digital_write(false);
}

void VS10XXBase::control_mode_off_() {
  this->spi_->disable();
  this->xdcs_pin_->digital_write(true);
  this->xcs_pin_->digital_write(true);
}

void VS10XXBase::data_mode_on_() {
  this->spi_->enable();
  this->xcs_pin_->digital_write(true);
  this->xdcs_pin_->digital_write(false);
}

void VS10XXBase::data_mode_off_() {
  this->spi_->disable();
  this->xdcs_pin_->digital_write(true);
  this->xcs_pin_->digital_write(true);
}

bool VS10XXBase::data_request_ready_() const {
  return this->dreq_pin_->digital_read() == true;
}

void VS10XXBase::wait_for_data_request_() const {
  while (!this->data_request_ready_()) {
    delay(1);
  }
}

}  // namespace vs10xx_base
}  // namespace esphome
