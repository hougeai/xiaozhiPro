#include "dual_network_board.h"
#include "zhengchen_eye_lcd_display.h"
#include "audio/codecs/box_audio_codec.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "config.h"
#include "assets/lang_config.h"
#include <font_awesome.h>

#include <esp_lcd_panel_vendor.h>
#include <wifi_station.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include "power_save_timer.h"
#include "../zhengchen-1.54tft-wifi/power_manager.h"
#include "driver/touch_pad.h"

// 如果240屏幕，用esp_lcd_gc9a01定义在components
#if CONFIG_USE_LCD_240X240_GIF
#include <esp_lcd_gc9a01.h>
static const gc9a01_lcd_init_cmd_t gc9107_lcd_init_cmds[] = {
    //  {cmd, { data }, data_size, delay_ms}
    {0xfe, (uint8_t[]){0x00}, 0, 0},
    {0xef, (uint8_t[]){0x00}, 0, 0},
    {0xb0, (uint8_t[]){0xc0}, 1, 0},
    {0xb1, (uint8_t[]){0x80}, 1, 0},
    {0xb2, (uint8_t[]){0x27}, 1, 0},
    {0xb3, (uint8_t[]){0x13}, 1, 0},
    {0xb6, (uint8_t[]){0x19}, 1, 0},
    {0xb7, (uint8_t[]){0x05}, 1, 0},
    {0xac, (uint8_t[]){0xc8}, 1, 0},
    {0xab, (uint8_t[]){0x0f}, 1, 0},
    {0x3a, (uint8_t[]){0x05}, 1, 0},
    {0xb4, (uint8_t[]){0x04}, 1, 0},
    {0xa8, (uint8_t[]){0x08}, 1, 0},
    {0xb8, (uint8_t[]){0x08}, 1, 0},
    {0xea, (uint8_t[]){0x02}, 1, 0},
    {0xe8, (uint8_t[]){0x2A}, 1, 0},
    {0xe9, (uint8_t[]){0x47}, 1, 0},
    {0xe7, (uint8_t[]){0x5f}, 1, 0},
    {0xc6, (uint8_t[]){0x21}, 1, 0},
    {0xc7, (uint8_t[]){0x15}, 1, 0},
    {0xf0,
    (uint8_t[]){0x1D, 0x38, 0x09, 0x4D, 0x92, 0x2F, 0x35, 0x52, 0x1E, 0x0C,
                0x04, 0x12, 0x14, 0x1f},
    14, 0},
    {0xf1,
    (uint8_t[]){0x16, 0x40, 0x1C, 0x54, 0xA9, 0x2D, 0x2E, 0x56, 0x10, 0x0D,
                0x0C, 0x1A, 0x14, 0x1E},
    14, 0},
    {0xf4, (uint8_t[]){0x00, 0x00, 0xFF}, 3, 0},
    {0xba, (uint8_t[]){0xFF, 0xFF}, 2, 0},
};

#else
#include "esp_lcd_gc9d01n.h"
#endif

#define TAG "ZHENGCHEN_EYE"


class ZHENGCHEN_EYE : public DualNetworkBoard {
private:
    i2c_master_bus_handle_t i2c_bus_; // I2C总线句柄
    Button boot_button_;
    ZhengchenEyeLcdDisplay* display_;
    PowerSaveTimer* power_save_timer_; // 省电定时器
    PowerManager* power_manager_; // 电源管理器
    esp_lcd_panel_io_handle_t panel_io = nullptr; // LCD面板IO句柄
    esp_lcd_panel_handle_t panel = nullptr; // LCD面板句柄
    uint32_t touch_value = 0;  // 头部触摸值
    uint32_t touch_value1 = 0; // 身体触摸值


    void InitializePowerManager() {
        power_manager_ = new PowerManager(GPIO_NUM_7); // 创建电源管理器实例
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
                ESP_LOGW(TAG, "Charging, disable power save timer");
            } else {
                power_save_timer_->SetEnabled(true);
                ESP_LOGW(TAG, "Not charging, enable power save timer");
            }
        });
    }

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(240, 60, -1); // 创建省电定时器(60秒无操作进入睡眠，永不关机)
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
         
        power_save_timer_->SetEnabled(true);
    }

    void touch_init() {
        touch_pad_init();
        touch_pad_config(TOUCH_PAD_NUM4); // 配置 GPIO4 为触摸引脚
        touch_pad_config(TOUCH_PAD_NUM5); // 配置 GPIO5 为触摸引脚
        touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V); // 触摸传感器配置以提高稳定性
        touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER); // 设置 FSM 模式为定时器模式
        touch_pad_fsm_start();
        vTaskDelay(40 / portTICK_PERIOD_MS);
    }

    static void touch_read_task(void* arg) {
        ZHENGCHEN_EYE* self = static_cast<ZHENGCHEN_EYE*>(arg);
        uint32_t last_touch4 = 0, last_touch5 = 0;
        int retry_count = 0;
        while (1) {
            touch_pad_read_raw_data(TOUCH_PAD_NUM4, &self->touch_value);
            touch_pad_read_raw_data(TOUCH_PAD_NUM5, &self->touch_value1);
            if (self->touch_value == last_touch4 && self->touch_value1 == last_touch5) {
                retry_count++;
                if (retry_count > 3) {
                    ESP_LOGI(TAG, "Touch sensor stuck, reinitializing...");
                    touch_pad_fsm_stop();
                    vTaskDelay(10 / portTICK_PERIOD_MS);
                    self->touch_init();
                    retry_count = 0;
                }
            } else {
                retry_count = 0;
            }
            last_touch4 = self->touch_value;
            last_touch5 = self->touch_value1;
            ESP_LOGI(TAG, "Touch pad 4: %d, pad 5: %d", self->touch_value, self->touch_value1);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
        }
    }
   
    // 音频编解码器I2C初始化
    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
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


    void InitializeButtons() {
        // 单击切换对话状态
        boot_button_.OnClick([this]() {
            ESP_LOGI(TAG, "Boot button clicked, toggle chat state");
            auto& app = Application::GetInstance();
            app.ToggleChatState(); // 如果idle切到listening，speaking则打断，alarm则停止
        });
        // 双击切换网络
#if CONFIG_USE_4G_WIFI
        boot_button_.OnDoubleClick([this]() {
            // power_save_timer_->WakeUp();
            ESP_LOGI(TAG, "Boot button double clicked, switch network type");
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting || app.GetDeviceState() == kDeviceStateWifiConfiguring  || app.GetDeviceState() == kDeviceStateIdle) {
                SwitchNetworkType();
            }
        });
#endif
        boot_button_.OnLongPress([this]() {
            if (GetNetworkType() == NetworkType::WIFI) {
                ESP_LOGI(TAG, "Boot button long pressed, reset wifi configuration");
                auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
                wifi_board.ResetWifiConfiguration();
            }
            
        });
    }

    // SPI总线初始化（用于显示屏）
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

    
    void InitializeGc9107Display(){
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = -1;
        io_config.dc_gpio_num = DISPLAY_DC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

#if CONFIG_USE_LCD_240X240_GIF
       // 初始化液晶屏驱动芯片
       ESP_LOGD(TAG, "Install LCD driver for 240x240");
       esp_lcd_panel_dev_config_t panel_config = {};
       panel_config.reset_gpio_num =DISPLAY_RES;
       panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
       panel_config.bits_per_pixel = 16;
      
       ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel));
       gc9a01_vendor_config_t gc9107_vendor_config = {
           .init_cmds = gc9107_lcd_init_cmds,
           .init_cmds_size = sizeof(gc9107_lcd_init_cmds) / sizeof(gc9a01_lcd_init_cmd_t),
       };       

       esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        panel_config.vendor_config = &gc9107_vendor_config;
#else
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RES;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9d01n(panel_io, &panel_config, &panel));
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
#endif       
         
        display_ = new ZhengchenEyeLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        display_->SetupUI();    
    }

public:
    ZHENGCHEN_EYE() : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN),
        boot_button_(BOOT_BUTTON_GPIO){
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeCodecI2c();
        InitializeButtons();
#if !CONFIG_USE_NOLCD
        InitializeSpi();
        InitializeGc9107Display();
        GetBacklight()->SetBrightness(80);
#endif
        touch_init();
        GetAudioCodec()->SetOutputVolume(80);
        xTaskCreate(touch_read_task, "touch_read_task", 2048, this, 5, NULL);
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
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
    
#if !CONFIG_USE_NOLCD
    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
#endif

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = power_manager_->GetBatteryLevel();
        return true;
    }
  

    virtual bool GetTemperature(float& esp32temp)  override {
        esp32temp = power_manager_->GetTemperature();
        return true;
    }

    virtual bool Gethead_value(uint32_t& head_value)  override {
        head_value = touch_value;
        return true;
    }

    virtual bool Getbody_value(uint32_t& body_value)  override {
        body_value = touch_value1;
        return true;
    }
};

DECLARE_BOARD(ZHENGCHEN_EYE);
