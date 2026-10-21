#include <stdio.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_sleep.h>
#include <esp_lvgl_port.h>
#include <wifi_manager.h>

#include "application.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "wifi_board.h"
#include "board_power_bsp.h"
#include "charge_status.h"
#include "custom_lcd_display.h"
#include "rtc_pcf8563.h"
#include "zectrix_nfc.h"
#include "power_save_timer.h"
#include "mcp_server.h"
#include "assets.h"
#include "assets/lang_config.h"

#define TAG "zectrix_s3_epaper_4_2"

/**
 * @brief 检测双按钮（上翻页+下翻页）同时按下，切换OTA启动分区
 * 
 * 在设备启动早期检测GPIO39和GPIO18是否同时被按下（持续500ms），
 * 如果检测到则切换到另一个OTA分区并重启设备。
 * 用于在小智固件和Zectrix固件之间切换。
 */
static void CheckDualButtonForOtaSwitch() {
    // 配置按钮GPIO为输入，启用上拉
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << TODO_UP_BUTTON_GPIO) | (1ULL << TODO_DOWN_BUTTON_GPIO);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    // 等待GPIO稳定
    vTaskDelay(pdMS_TO_TICKS(50));

    // 检测两个按钮是否同时按下（低电平有效）
    bool up_pressed = (gpio_get_level(TODO_UP_BUTTON_GPIO) == 0);
    bool down_pressed = (gpio_get_level(TODO_DOWN_BUTTON_GPIO) == 0);

    if (up_pressed && down_pressed) {
        ESP_LOGI(TAG, "Dual buttons detected, waiting for confirmation...");
        
        // 等待500ms确认不是误触
        vTaskDelay(pdMS_TO_TICKS(500));
        
        // 再次检查按钮状态
        up_pressed = (gpio_get_level(TODO_UP_BUTTON_GPIO) == 0);
        down_pressed = (gpio_get_level(TODO_DOWN_BUTTON_GPIO) == 0);
        
        if (up_pressed && down_pressed) {
            ESP_LOGI(TAG, "Dual button switch confirmed, switching OTA partition...");
            
            // 获取当前运行的分区
            const esp_partition_t *running_partition = esp_ota_get_running_partition();
            ESP_LOGI(TAG, "Current running partition: %s", running_partition->label);
            
            // 获取下一个OTA分区
            const esp_partition_t *next_partition = esp_ota_get_next_update_partition(NULL);
            
            if (next_partition != NULL) {
                ESP_LOGI(TAG, "Switching to partition: %s", next_partition->label);
                
                // 设置下一个启动分区
                esp_err_t err = esp_ota_set_boot_partition(next_partition);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Boot partition set successfully, rebooting in 2 seconds...");
                    
                    // 延迟2秒让用户看到日志
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    
                    // 重启设备
                    esp_restart();
                } else {
                    ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
                }
            } else {
                ESP_LOGE(TAG, "No next update partition found");
            }
        } else {
            ESP_LOGI(TAG, "Dual button press not confirmed, continuing normal boot");
        }
    }
    
    // 恢复 GPIO 为输入上拉（Button 类需要，不能用 gpio_reset_pin 打回高阻）
    gpio_config_t btn_restore = {};
    btn_restore.intr_type = GPIO_INTR_DISABLE;
    btn_restore.mode = GPIO_MODE_INPUT;
    btn_restore.pin_bit_mask = (1ULL << TODO_UP_BUTTON_GPIO) | (1ULL << TODO_DOWN_BUTTON_GPIO);
    btn_restore.pull_up_en = GPIO_PULLUP_ENABLE;
    btn_restore.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&btn_restore);
}

class CustomBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Button up_button_;
    Button down_button_;
    CustomLcdDisplay *display_ = nullptr;
    BoardPowerBsp *power_ = nullptr;
    ChargeStatus charge_status_;
    std::unique_ptr<RtcPcf8563> rtc_;
    std::unique_ptr<ZectrixNfc> nfc_;
    PowerSaveTimer *power_save_timer_ = nullptr;
    bool powered_off_ = false;

    adc_oneshot_unit_handle_t adc1_handle = nullptr;
    adc_cali_handle_t cali_handle = nullptr;

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {};
        i2c_bus_cfg.i2c_port          = (i2c_port_t) 0;
        i2c_bus_cfg.sda_io_num        = AUDIO_CODEC_I2C_SDA_PIN;
        i2c_bus_cfg.scl_io_num        = AUDIO_CODEC_I2C_SCL_PIN;
        i2c_bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 7;
        i2c_bus_cfg.intr_priority     = 0;
        i2c_bus_cfg.trans_queue_depth = 0;
        i2c_bus_cfg.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void ShowSleepImage() {
        void* ptr = nullptr;
        size_t size = 0;
        display_->SetFrozen(true);   // 冻结 LVGL，避免状态栏/表情覆盖休眠图
        if (Assets::GetInstance().GetAssetData("sleep.raw", ptr, size) && ptr != nullptr) {
            display_->WriteRaw1bpp(0, 0, EXAMPLE_LCD_WIDTH, EXAMPLE_LCD_HEIGHT,
                                   (const uint8_t*)ptr, size);
            display_->RequestUrgentFullRefresh();
        } else {
            GetDisplay()->SetEmotion("sleepy");
            GetDisplay()->SetChatMessage("system", "休眠中");
        }
    }

    // 关机：显示休眠图后进入 light sleep，圆键(GPIO0)按下唤醒。
    // 用 light sleep 而不是 deep sleep：deep sleep 唤醒会重走 ROM bootloader，
    // 而 GPIO0 正是下载模式 strapping 脚，长按圆键会被误判成下载模式；
    // light sleep 唤醒不经过 bootloader，长按/短按都不会进下载模式。
    // 注意不能用 VbatPowerOff()：VBAT 是主控自己的电池供电开关，切断后
    // 纯电池状态下整机掉电，圆键再也无法开机。
    void EnterLightSleep() {
        ESP_LOGI(TAG, "Shutdown: stopping audio/wifi/lvgl before light sleep");
        power_->PowerAudioOff();
        // 停 WiFi 与 LVGL tick：避免它们阻止进入 light sleep 或周期唤醒
        WifiManager::GetInstance().StopStation();
        WifiManager::GetInstance().StopConfigAp();
        vTaskDelay(pdMS_TO_TICKS(200));   // 让断网事件处理完再停 LVGL
        lvgl_port_stop();

        // 长按关机时用户可能还按着圆键，先等松开，否则 ext0 会立刻唤醒
        while (gpio_get_level(GPIO_NUM_0) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        // 清空所有唤醒源，只保留圆键(GPIO0)低电平唤醒
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);

        ESP_LOGI(TAG, "Shutdown: entering light sleep (wake on BOOT button)");
        esp_light_sleep_start();

        // 圆键唤醒。等松开再重启：软复位时 bootloader 仍会采样 GPIO0，
        // 若此时还按着就会被当成下载模式。
        ESP_LOGI(TAG, "Woken from light sleep, waiting for BOOT release");
        while (gpio_get_level(GPIO_NUM_0) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        ESP_LOGI(TAG, "Powering on (restart)");
        power_->VbatPowerOn();
        power_->PowerAudioOn();
        power_->PowerEpdOn();
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }

    // 关机流程放到独立任务执行：按钮/PowerSaveTimer 的回调都在 esp_timer 任务里，
    // 在里面 vTaskDelay 会阻塞所有 esp_timer 回调（含省电定时器自身）。
    static void ShutdownTask(void* arg) {
        CustomBoard* self = static_cast<CustomBoard*>(arg);
        for (int i = 0; i < 200 && self->display_ != nullptr && self->display_->IsRefreshPending(); i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        self->EnterLightSleep();
    }

    void Shutdown() {
        if (powered_off_) return;
        powered_off_ = true;
        ESP_LOGI(TAG, "Shutdown: showing sleep image");
        ShowSleepImage();
        if (xTaskCreate(ShutdownTask, "shutdown", 3072, this, 5, nullptr) != pdPASS) {
            ESP_LOGE(TAG, "Shutdown: failed to create task, sleeping in place");
            EnterLightSleep();
        }
    }

    void PowerOn() {
        if (!powered_off_) return;
        powered_off_ = false;
        // 外设断电后需要重新初始化（音频/屏幕），直接重启最干净
        power_->VbatPowerOn();
        power_->PowerAudioOn();
        power_->PowerEpdOn();
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto &app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            if (powered_off_) {
                PowerOn();
                return;
            }
            app.ToggleChatState();
        });

        boot_button_.OnLongPress([this]() {
            Shutdown();
        });

        up_button_.OnClick([this]() {
            auto &app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume / 10));
        });

        down_button_.OnClick([this]() {
            auto &app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume / 10));
        });
    }

    void InitializePowerSaveTimer() {
        // 参数: (cpu_max_freq, 休眠秒数, 关机秒数) = 10 分钟休眠 / 30 分钟关机
        power_save_timer_ = new PowerSaveTimer(-1, 600, 1800);
        power_save_timer_->OnEnterSleepMode([this]() {
            if (powered_off_) return;   // 已关机，无需再处理
            ESP_LOGI(TAG, "Entering sleep mode");
            ShowSleepImage();
        });
        power_save_timer_->OnExitSleepMode([this]() {
            if (powered_off_) return;   // 关机状态下外设已断电，不要刷新/解冻
            ESP_LOGI(TAG, "Exiting sleep mode");
            display_->SetFrozen(false);   // 解冻 LVGL
            GetDisplay()->SetEmotion("neutral");
            display_->ForceFullRedraw();  // 清除休眠图片
        });
        power_save_timer_->OnShutdownRequest([this]() {
            Shutdown();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.disp.network", "重新配网", PropertyList(), [this](const PropertyList &) -> ReturnValue {
            EnterWifiConfigMode();
            return true;
        });

        // 刷新档位：只控制"刷新节流间隔"和"全刷周期"；局部刷/全刷仍由内容差异决定
        mcp_server.AddTool("self.disp.set_refresh_rate",
                           "Set how eagerly the e-paper refreshes. Allowed values: "
                           "\"slow\", \"medium\" (default), \"fast\". "
                           "Each level is a fixed pair: the minimum interval between two refreshes, "
                           "and how many partial refreshes are allowed before a forced full refresh. "
                           "slow = 1000 ms interval + full refresh every 30 partials (calmest, least "
                           "flashing, display a bit stale); "
                           "medium = 300 ms + every 10 partials; "
                           "fast = 100 ms + every 5 partials (most responsive, more flashing). "
                           "This throttles both partial and full refreshes; whether a given update is "
                           "partial or full is still decided by how much of the content changed.",
                           PropertyList({Property("rate", kPropertyTypeString, std::string("medium"))}),
                           [this](const PropertyList &properties) -> ReturnValue {
                               const std::string rate = properties["rate"].value<std::string>();
                               CustomLcdDisplay::RefreshRate target;
                               if (rate == "slow") {
                                   target = CustomLcdDisplay::RefreshRate::kSlow;
                               } else if (rate == "fast") {
                                   target = CustomLcdDisplay::RefreshRate::kFast;
                               } else if (rate == "medium") {
                                   target = CustomLcdDisplay::RefreshRate::kMedium;
                               } else {
                                   return std::string("unknown rate '") + rate +
                                          "', expected slow|medium|fast";
                               }
                               display_->SetRefreshRate(target);
                               return std::string("refresh rate set to ") +
                                      CustomLcdDisplay::RefreshRateName(target) + " (interval " +
                                      std::to_string(CustomLcdDisplay::RefreshRateIntervalMs(target)) +
                                      " ms, full refresh every " +
                                      std::to_string(
                                          CustomLcdDisplay::RefreshRatePartialsBeforeFull(target)) +
                                      " partial refreshes)";
                           });

        mcp_server.AddTool("self.disp.get_refresh_rate",
                           "Get the current e-paper refresh rate: level, minimum interval between "
                           "two refreshes, and how often a full refresh is forced.",
                           PropertyList(), [this](const PropertyList &) -> ReturnValue {
                               const auto rate = display_->GetRefreshRate();
                               return std::string("refresh rate is ") +
                                      CustomLcdDisplay::RefreshRateName(rate) + " (interval " +
                                      std::to_string(CustomLcdDisplay::RefreshRateIntervalMs(rate)) +
                                      " ms, full refresh every " +
                                      std::to_string(
                                          CustomLcdDisplay::RefreshRatePartialsBeforeFull(rate)) +
                                      " partial refreshes)";
                           });

        // 立即做一次全屏刷新（清残影）
        mcp_server.AddTool("self.disp.refresh_now",
                           "Force a full-screen refresh now. Use it when the screen shows ghosting "
                           "or leftover content.",
                           PropertyList(), [this](const PropertyList &) -> ReturnValue {
                               display_->RequestUrgentFullRefresh();
                               return true;
                           });
    }

    void InitializeLcdDisplay() {
        custom_lcd_spi_t lcd_spi_data = {};
        lcd_spi_data.cs               = EPD_CS_PIN;
        lcd_spi_data.dc               = EPD_DC_PIN;
        lcd_spi_data.rst              = EPD_RST_PIN;
        lcd_spi_data.busy             = EPD_BUSY_PIN;
        lcd_spi_data.mosi             = EPD_MOSI_PIN;
        lcd_spi_data.scl              = EPD_SCK_PIN;
        lcd_spi_data.power            = EPD_PWR_PIN;
        lcd_spi_data.spi_host         = EPD_SPI_NUM;
        lcd_spi_data.buffer_len       = EXAMPLE_LCD_WIDTH * EXAMPLE_LCD_HEIGHT / 8;
        display_ = new CustomLcdDisplay(NULL, NULL, EXAMPLE_LCD_WIDTH, EXAMPLE_LCD_HEIGHT,
                                        DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                        lcd_spi_data);
    }

    void Power_Init() {
        int64_t now_ms = esp_timer_get_time() / 1000;
        charge_status_.Init(static_cast<gpio_num_t>(CHARGE_DETECT_GPIO),
                           static_cast<gpio_num_t>(CHARGE_FULL_GPIO),
                           now_ms);

        power_ = new BoardPowerBsp(EPD_PWR_PIN, Audio_PWR_PIN, Audio_AMP_PIN, VBAT_PWR_PIN,
                                   &charge_status_);
        power_->VbatPowerOn();
        power_->PowerAudioOn();
        power_->PowerEpdOn();
        // VBAT 反馈引脚（GPIO18，同时也是下键）配置为输入上拉，避免高阻读到低电平
        gpio_config_t vbat_gpio = {};
        vbat_gpio.intr_type = GPIO_INTR_DISABLE;
        vbat_gpio.mode = GPIO_MODE_INPUT;
        vbat_gpio.pin_bit_mask = 1ULL << VBAT_PWR_GPIO;
        vbat_gpio.pull_up_en = GPIO_PULLUP_ENABLE;
        vbat_gpio.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio_config(&vbat_gpio);
        int vbat_timeout = 100;
        do {
            vTaskDelay(pdMS_TO_TICKS(10));
        } while (!gpio_get_level(static_cast<gpio_num_t>(VBAT_PWR_GPIO)) && --vbat_timeout > 0);
        if (vbat_timeout <= 0) {
            ESP_LOGW(TAG, "VBAT feedback GPIO%d not asserted, continuing anyway", VBAT_PWR_GPIO);
        }
    }

    void InitializeRtc() {
        rtc_ = std::make_unique<RtcPcf8563>(i2c_bus_, RTC_I2C_ADDR);
        rtc_->Init(static_cast<gpio_num_t>(RTC_INT_GPIO));
        ESP_LOGI(TAG, "RTC PCF8563 initialized");
    }

    void InitializeNfc() {
        nfc_ = std::make_unique<ZectrixNfc>(i2c_bus_, NFC_I2C_ADDR,
                                            static_cast<gpio_num_t>(NFC_PWR_GPIO),
                                            static_cast<gpio_num_t>(NFC_FD_GPIO),
                                            NFC_FD_ACTIVE_LEVEL);
        if (nfc_->Init()) {
            ESP_LOGI(TAG, "NFC GT23SC6699 initialized");
        } else {
            ESP_LOGW(TAG, "NFC GT23SC6699 init failed");
        }
    }

    void ChargeStatusTick() {
        // 注意：不要在这里根据 DeviceState 变化强制全刷。状态栏文字（说话中/聆听中、
        // 时间、电量、网络）本来就只是局部变化，强制全刷会让每次状态切换都闪一次。
        // 残影由 refresh 任务的「每 N 次局部刷强制全刷一次」兜底。
        int64_t now_ms = esp_timer_get_time() / 1000;
        charge_status_.Tick(now_ms);

        if (power_save_timer_) {
            auto snap = charge_status_.Get();
            power_save_timer_->SetEnabled(!snap.charging);
        }
    }

    uint16_t BatteryGetVoltage() {
        static bool initialized = false;
        static adc_oneshot_unit_handle_t adc_handle = nullptr;
        static adc_cali_handle_t cal_handle = nullptr;

        if (!initialized) {
            adc_oneshot_unit_init_cfg_t init_config = {
                .unit_id = ADC_UNIT_1,
            };
            adc_oneshot_new_unit(&init_config, &adc_handle);

            adc_oneshot_chan_cfg_t ch_config = {
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_3, &ch_config);

            adc_cali_curve_fitting_config_t cali_config = {
                .unit_id = ADC_UNIT_1,
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            if (adc_cali_create_scheme_curve_fitting(&cali_config, &cal_handle) == ESP_OK) {
                initialized = true;
            }
        }

        if (initialized) {
            int raw_value = 0;
            int raw_voltage = 0;
            int voltage = 0;
            adc_oneshot_read(adc_handle, ADC_CHANNEL_3, &raw_value);
            adc_cali_raw_to_voltage(cal_handle, raw_value, &raw_voltage);
            voltage = raw_voltage * 2;
            return (uint16_t)voltage;
        }

        return 0;
    }

    uint8_t BatteryGetPercent() {
        int voltage = 0;
        for (uint8_t i = 0; i < 10; i++) {
            voltage += BatteryGetVoltage();
        }
        voltage /= 10;
        int percent = (-1 * voltage * voltage + 9016 * voltage - 19189000) / 10000;
        percent = (percent > 100) ? 100 : (percent < 0) ? 0 : percent;
        return (uint8_t)percent;
    }

public:
    CustomBoard()
        : boot_button_(BOOT_BUTTON_GPIO),
          up_button_(TODO_UP_BUTTON_GPIO),
          down_button_(TODO_DOWN_BUTTON_GPIO) {
        // deep sleep 唤醒后电源 GPIO 可能仍处于 hold 状态，先解除以便重新配置
        gpio_hold_dis(EPD_PWR_PIN);
        gpio_hold_dis(EPD_RST_PIN);
        gpio_hold_dis(VBAT_PWR_PIN);
        gpio_hold_dis(Audio_PWR_PIN);
        gpio_hold_dis(Audio_AMP_PIN);
        // 在最早期检测双按钮切换OTA分区
        CheckDualButtonForOtaSwitch();
        
        Power_Init();
        InitializeI2c();
        InitializeButtons();
        InitializePowerSaveTimer();
        InitializeTools();
        InitializeRtc();
        InitializeNfc();
        InitializeLcdDisplay();
    }

    virtual AudioCodec *GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            i2c_bus_, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display *GetDisplay() override {
        return display_;
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        if (powered_off_) {
            // 已关机：外设断电后电池读数不可靠，报告充电态避免误报"电量低"
            level = 100;
            charging = true;
            discharging = false;
            return true;
        }
        ChargeStatusTick();
        auto snap = charge_status_.Get();
        charging = snap.charging;
        discharging = !charging && snap.power_present;
        level = (int)BatteryGetPercent();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            if (power_save_timer_) {
                power_save_timer_->WakeUp();
            }
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(CustomBoard);
