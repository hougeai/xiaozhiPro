#include "device_state.h"
#include "dual_network_board.h"
#include "codecs/box_audio_codec.h"
#include "display/oled_display_custom.h"
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
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#define TAG "AI4gDisplayBoard"

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
        // 读取电池电量百分比 (StateOfCharge register address: 0x02)
        // ReadRegs(0x1C, read_buffer_, 2); // 读取两个字节
        // BQ27220 的 StateOfCharge 返回 16 位固定点值，单位为 1/256 %。
        // LSB = 1/256%，MSB = 整数部分。
        // uint16_t soc_raw = (uint16_t)read_buffer_[0] | ((uint16_t)read_buffer_[1] << 8);
        // 转换为实际百分比
        // float soc = soc_raw / 256.0f;
        // ESP_LOGI(TAG, "Battery level(raw): %.1f%%", soc);
        // 限制范围，防止意外超出
        // if (soc > 100.0f) soc = 100.0f;
        // if (soc < 0.0f) soc = 0.0f;
        // 保存到成员变量，可选是否保留小数
        // battery_level_ = static_cast<uint8_t>(soc + 0.5f);  // 四舍五入成整数%
        // ESP_LOGI(TAG, "Battery level(final): %d%%", battery_level_);
        // 读取电压寄存器
        ReadRegs(0x08, read_buffer_, 2);
        int16_t voltage = static_cast<uint16_t>(read_buffer_[1] << 8 | read_buffer_[0]);
        // 用电压估算电量
        battery_level_ = EstimateBatteryPercent(voltage);
        ESP_LOGI(TAG, "Battery voltage: %d mV Battery level: %d ", voltage, battery_level_);
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

class LichuangAudioCodec : public BoxAudioCodec {
public:
    LichuangAudioCodec(i2c_master_bus_handle_t i2c_bus)
        : BoxAudioCodec(i2c_bus,
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
                        AUDIO_INPUT_REFERENCE) {}

    virtual void EnableOutput(bool enable) override {
        BoxAudioCodec::EnableOutput(enable);
        // 如有硬件需要，未来可以扩展
    }
};

class AI4gDisplayBoard : public DualNetworkBoard {
private:
    i2c_master_bus_handle_t i2c_bus_; // 用于音频的I2C总线
    i2c_master_bus_handle_t display_i2c_bus_;  // 用于显示屏的I2C总线
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;

    // 电池管理相关变量
    Charge* charge_ = nullptr;
    // PowerSaveTimer* power_save_timer_ = nullptr;
    // bool last_charging_state_ = false;  // 充电状态

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

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)0,  // 使用不同的I2C端口
            .sda_io_num = DISPLAY_I2C_SDA_PIN,  // 使用显示屏专用的SDA引脚
            .scl_io_num = DISPLAY_I2C_SCL_PIN,  // 使用显示屏专用的SCL引脚
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };

        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new CustomOledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeButtons() {
        // 单击切换对话状态
        boot_button_.OnClick([this]() {
            // power_save_timer_->WakeUp();
            ESP_LOGI(TAG, "Boot button clicked, toggle chat state");
            auto& app = Application::GetInstance();
            if (GetNetworkType() == NetworkType::WIFI) {
                if (app.GetDeviceState() == kDeviceStateStarting) {
                    // cast to WifiBoard
                    auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
                    wifi_board.EnterWifiConfigMode();
                    return;
                }
            }
            app.ToggleChatState(); // 如果idle切到listening，speaking则打断，alarm则停止
        });
        // 双击切换网络
        boot_button_.OnDoubleClick([this]() {
            // power_save_timer_->WakeUp();
            ESP_LOGI(TAG, "Boot button double clicked, switch network type");
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting || app.GetDeviceState() == kDeviceStateWifiConfiguring  || app.GetDeviceState() == kDeviceStateIdle) {
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

    void InitializePowerSaveTimer() {
        // // 创建电源管理定时器
        // // 参数: CPU最大频率(-1表示不改变), 60秒后进入休眠, 300秒后关机
        // power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        // // 设置进入休眠模式时的回调
        // power_save_timer_->OnEnterSleepMode([this]() {
        //     GetDisplay()->SetPowerSaveMode(true);
        // });
        
        // // 设置退出休眠模式时的回调
        // power_save_timer_->OnExitSleepMode([this]() {
        //     GetDisplay()->SetPowerSaveMode(false);
        // });
        
        // // 设置关机请求时的回调
        // power_save_timer_->OnShutdownRequest([this]() {
        //     ESP_LOGI(TAG, "Shutting down");
        //     // 关闭显示
        //     esp_lcd_panel_disp_on_off(panel_, false);
        //     // 进入深度睡眠
        //     esp_deep_sleep_start();
        // });
        
        // // 启用电源管理定时器
        // power_save_timer_->SetEnabled(true);
        ESP_LOGI(TAG, "Power save timer initialized");
    }

public:
    AI4gDisplayBoard() : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC),
        boot_button_(BOOT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeI2c();
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeBatteryManagement();
        InitializePowerSaveTimer();
        InitializeButtons();
        ESP_LOGI(TAG, "Initialized AI4gDisplayBoard");
    }

    // virtual Led* GetLed() override {
    //     static GpioLed led(BUILTIN_LED_GPIO);
    //     return &led;
    // }
    
    virtual Led* GetManualLed() override {
        static GpioLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static LichuangAudioCodec audio_codec(i2c_bus_);
        return &audio_codec;
    }

    // 添加电池状态获取方法
    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (charge_ != nullptr) {
            charge_->UpdateBatteryInfo();
            level = charge_->GetBatteryLevel();
            charging = charge_->IsCharging();
            discharging = charge_->IsDischarging();
            
            // if (charging != last_charging_state_) {
            //     if (charging) {
            //         // 充电时禁用休眠功能
            //         power_save_timer_->SetEnabled(false);
            //     } else {
            //         // 不充电时启用休眠功能
            //         power_save_timer_->SetEnabled(true);
            //     }
            //     last_charging_state_ = charging;
            // }
            return true;
        }
        return false;
    }

};

DECLARE_BOARD(AI4gDisplayBoard);
