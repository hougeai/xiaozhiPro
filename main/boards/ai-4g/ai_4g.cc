#include "dual_network_board.h"
#include "codecs/box_audio_codec.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"


#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
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
        buscfg.mosi_io_num = GPIO_NUM_40;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_41;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = 2048;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
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

        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting || app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                SwitchNetworkType();
            }
        });
    }
public:
    AI4gBoard() : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC),
        boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeButtons();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static LichuangAudioCodec audio_codec(i2c_bus_);
        return &audio_codec;
    }

};

DECLARE_BOARD(AI4gBoard);
