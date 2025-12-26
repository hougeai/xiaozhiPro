#include "device_state.h"
#include "dual_network_board.h"
#include "audio/codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "led/gpio_led.h"
#include "config.h"
#include "power_save_timer.h"
#include "i2c_device.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <wifi_station.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_ili9341.h>

#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#define TAG "AI4g231tftBoard"

int EstimateBatteryPercent(int voltage_mv)
{
    struct Point {
        int mv;
        int percent;
    };

    // 电压 - 电量 曲线（你可以自行调整）
    const Point curve[] = {
        {4168, 100},
        {4100,  80},
        {4000,  60},
        {3900,  40},
        {3800,  20},
        {3700,  10},
        {3600,   5},
        {3400,   0},
    };

    const int size = sizeof(curve) / sizeof(curve[0]);

    // 高于最高值
    if (voltage_mv >= curve[0].mv)
        return 100;

    // 低于最低值
    if (voltage_mv <= curve[size - 1].mv)
        return 0;

    // 分段线性插值
    for (int i = 0; i < size - 1; i++) {
        if (voltage_mv >= curve[i + 1].mv && voltage_mv <= curve[i].mv) {
            int mv_high = curve[i].mv;
            int mv_low = curve[i + 1].mv;
            int pct_high = curve[i].percent;
            int pct_low = curve[i + 1].percent;

            float ratio = (float)(voltage_mv - mv_low) / (mv_high - mv_low);
            return pct_low + (int)(ratio * (pct_high - pct_low) + 0.5f);
        }
    }
    return 0;
}

class Charge : public I2cDevice {
public:
    Charge(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        read_buffer_ = new uint8_t[4];
        // 初始化电池相关信息
        battery_level_ = 100;
        is_charging_ = false;
        is_discharging_ = false;
    }
    ~Charge() {
        delete[] read_buffer_;
    }
    
    void UpdateBatteryInfo() {
        // 读取电压寄存器
        ReadRegs(0x08, read_buffer_, 2);
        int16_t voltage = static_cast<uint16_t>(read_buffer_[1] << 8 | read_buffer_[0]);
        // 用电压估算电量
        battery_level_ = EstimateBatteryPercent(voltage);
        // ESP_LOGI(TAG, "Battery voltage: %d mV Battery level: %d ", voltage, battery_level_);
        // 读取电流寄存器，判断充电状态
        ReadRegs(0x0c, read_buffer_ + 2, 2);
        int16_t current = static_cast<int16_t>(read_buffer_[3] << 8 | read_buffer_[2]);
        // 如果电流为正，表示正在充电；如果为负，表示正在放电
        is_charging_ = (current > 10);  // 10mA阈值，避免微小电流干扰
        is_discharging_ = (current < -10);
        // ESP_LOGI(TAG, "Battery current: %d mA is_charging: %d is_discharging: %d", current, is_charging_, is_discharging_);
    }
    
    int GetBatteryLevel() const { return battery_level_; }
    bool IsCharging() const { return is_charging_; }
    bool IsDischarging() const { return is_discharging_; }

private:
    uint8_t* read_buffer_ = nullptr;
    int battery_level_;
    bool is_charging_;
    bool is_discharging_;
};


class AI4g231tftBoard : public DualNetworkBoard {
private:
    i2c_master_bus_handle_t i2c_bus_; // 用于音频的I2C总线
    SpiLcdDisplay* display_ = nullptr;
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;

    // 电池管理相关变量
    Charge* charge_ = nullptr;

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };

        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SDA;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SCL;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeIli9342Display() {
        ESP_LOGI(TAG, "Init IlI9342");
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS;
        io_config.dc_gpio_num = DISPLAY_DC;
        io_config.spi_mode = 2;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        ESP_LOGI(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_BACKLIGHT_OUTPUT_INVERT));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        ESP_LOGI(TAG, "SpiLcdDisplay display initialized successfully");
    }


    void InitializeButtons() {
        // 单击切换对话状态
        boot_button_.OnClick([this]() {
            // power_save_timer_->WakeUp();
            ESP_LOGI(TAG, "Boot button clicked, toggle chat state");
            auto& app = Application::GetInstance();
            app.ToggleChatState(); // 如果idle切到listening，speaking则打断，alarm则停止
        });
        // 双击切换网络
        boot_button_.OnDoubleClick([this]() {
            // power_save_timer_->WakeUp();
            ESP_LOGI(TAG, "Boot button double clicked, switch network type");
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateActivating ||app.GetDeviceState() == kDeviceStateStarting || app.GetDeviceState() == kDeviceStateWifiConfiguring  || app.GetDeviceState() == kDeviceStateIdle) {
                SwitchNetworkType();
            }
        });

        volume_up_button_.OnClick([this]() {
            // power_save_timer_->WakeUp();
            ESP_LOGI(TAG, "Volume up button clicked");
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            ESP_LOGI(TAG, "Volume up button clicked %d", volume);
        });

        volume_up_button_.OnLongPress([this]() {
            // power_save_timer_->WakeUp();
            ESP_LOGI(TAG, "Volume up button long pressed");
            GetAudioCodec()->SetOutputVolume(100);
            ESP_LOGI(TAG, "Volume up button long pressed, volume 100");
        });

        volume_down_button_.OnClick([this]() {
            // power_save_timer_->WakeUp();
            ESP_LOGI(TAG, "Volume down button clicked");
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            ESP_LOGI(TAG, "Volume down button clicked %d", volume);
        });

        volume_down_button_.OnLongPress([this]() {
            // power_save_timer_->WakeUp();
            ESP_LOGI(TAG, "Volume down button long pressed");
            GetAudioCodec()->SetOutputVolume(0);
            ESP_LOGI(TAG, "Volume down button long pressed, volume 0");
        });
    }

    void InitializeBatteryManagement() {
        charge_ = new Charge(i2c_bus_, 0x55);
        ESP_LOGI(TAG, "Battery management initialized");
    }

    // 开启ML307模块电源
    void InitializeMl307Power() {
        // 配置ML307电源控制引脚为输出模式
        gpio_reset_pin(ML307_POWER_GPIO);
        gpio_set_direction(ML307_POWER_GPIO, GPIO_MODE_OUTPUT);
        // 设置为高电平，给ML307模块供电
        gpio_set_level(ML307_POWER_GPIO, 1);
        ESP_LOGI(TAG, "ML307 power initialized, pin %d set to high", ML307_POWER_GPIO);
    }

public:
    AI4g231tftBoard() : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC),
        boot_button_(BOOT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeMl307Power();
        InitializeI2c();
        InitializeSpi();
        InitializeIli9342Display();
        InitializeBatteryManagement();
        InitializeButtons();
        GetBacklight()->SetBrightness(80, true);
        ESP_LOGI(TAG, "Initialized AI4g231tftBoard");
    }
    
    virtual Led* GetManualLed() override {
        static GpioLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            GPIO_NUM_NC, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    // 添加电池状态获取方法
    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (charge_ != nullptr) {
            charge_->UpdateBatteryInfo();
            level = charge_->GetBatteryLevel();
            charging = charge_->IsCharging();
            discharging = charge_->IsDischarging();
            return true;
        }
        return false;
    }

    // 添加背光控制方法
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual void SetPowerSaveMode(bool enabled) override {
        auto backlight = GetBacklight();
        if (backlight != nullptr) {
            if (!enabled) {
                ESP_LOGI(TAG, "Exiting power save mode, restoring brightness to 100");
                backlight->SetBrightness(100);
            } else {
                ESP_LOGI(TAG, "Entering power save mode, reducing brightness to 10");
                backlight->SetBrightness(10);
            }
        } else {
            ESP_LOGW(TAG, "Backlight is null in SetPowerSaveMode");
        }
    }
};

DECLARE_BOARD(AI4g231tftBoard);
