#include "device_state.h"
#include "dual_network_board.h"
#include "codecs/box_audio_codec.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"


#include <esp_log.h>
#include <driver/i2c_master.h>
#include <wifi_station.h>

#define TAG "AI4gBoard"

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

class AI4gBoard : public DualNetworkBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;

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

    void InitializeButtons() {
        // 单击切换对话状态
        boot_button_.OnClick([this]() {
            ESP_LOGI(TAG, "Boot button clicked, toggle chat state");
            auto& app = Application::GetInstance();
            app.ToggleChatState(); // 如果idle切到listening，speaking则打断，alarm则停止
        });
        // 双击切换网络
        boot_button_.OnDoubleClick([this]() {
            ESP_LOGI(TAG, "Boot button double clicked, switch network type");
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting || app.GetDeviceState() == kDeviceStateWifiConfiguring  || app.GetDeviceState() == kDeviceStateIdle) {
                SwitchNetworkType();
            }
        });
        // 长按重置 wifi
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "Boot button long clicked, reset wifi");
            auto& app = Application::GetInstance();
            if (GetNetworkType() == NetworkType::WIFI) {
                if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                    // cast to WifiBoard
                    auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
                    wifi_board.ResetWifiConfiguration();
                }
            }
            app.ToggleChatState();
        });

        volume_up_button_.OnClick([this]() {
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
            ESP_LOGI(TAG, "Volume up button long pressed");
            GetAudioCodec()->SetOutputVolume(100);
            ESP_LOGI(TAG, "Volume up button long pressed, volume 100");
        });

        volume_down_button_.OnClick([this]() {
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
            ESP_LOGI(TAG, "Volume down button long pressed");
            GetAudioCodec()->SetOutputVolume(0);
            ESP_LOGI(TAG, "Volume down button long pressed, volume 0");
        });
    }
public:
    AI4gBoard() : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC),
        boot_button_(BOOT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeI2c();
        InitializeButtons();
        ESP_LOGI(TAG, "Initialized AI4gBoard");
    }

    virtual AudioCodec* GetAudioCodec() override {
        static LichuangAudioCodec audio_codec(i2c_bus_);
        return &audio_codec;
    }

};

DECLARE_BOARD(AI4gBoard);
