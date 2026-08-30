/*
 * es8311_audio.c - ESP32-S31-Function-CoreBoard-1 板载 ES8311 功放驱动（BSP 模块）
 *
 * 音频链路（仅播放，不涉及 USB / 解码部分）：
 *   ESP32 I2S TX (GPIO56) -> ES8311 DSDIN (DAC 输入)
 *   ES8311 差分输出 OUTP/OUTN -> NS4150B PA (GPIO57 高电平使能) -> 喇叭
 *
 * ⚠ 关键引脚：I2S DOUT=56 / DIN=54。二者接反会导致数据发到错误引脚而完全无声，
 *   即便 ES8311 所有寄存器状态均正常。来源：官方板定义 esp_boards
 *   board_peripherals.yaml（IO56 -> I2S_DSDIN，IO54 -> I2S_ASDOUT）。
 *
 * 引脚映射（官方板）：
 *   GPIO50 I2C_SCL      GPIO51 I2C_SDA（ES8311 8bit 地址 0x30 / 7bit 0x18，400kHz）
 *   GPIO52 I2S_MCLK     GPIO53 I2S_SCLK(BCLK)  GPIO55 I2S_LRCK(WS)
 *   GPIO56 I2S_DOUT（播放 -> Codec DAC 输入）  GPIO54 I2S_DIN（录音，本模块未使用）
 *   GPIO57 PA_CTRL（NS4150B 功放使能，高有效）
 */

#include "es8311_audio.h"

#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

static const char *TAG = "bsp_es8311";

#define BSP_I2C_PORT  I2C_NUM_0
#define BSP_I2S_PORT  I2S_NUM_0

/* ---------- 板级引脚定义（修改前请核对官方原理图） ---------- */
#define BSP_GPIO_I2C_SCL  50
#define BSP_GPIO_I2C_SDA  51
#define BSP_GPIO_I2S_MCLK 52
#define BSP_GPIO_I2S_SCLK 53
#define BSP_GPIO_I2S_DOUT 56 /* I2S 数据输出 -> ES8311 DSDIN（DAC 输入） */
#define BSP_GPIO_I2S_LRCK 55 /* I2S 帧时钟 / WS */
#define BSP_GPIO_I2S_DIN  54 /* I2S 数据输入 <- ES8311 ASDOUT（ADC 输出，未使用） */
#define BSP_GPIO_PA_CTRL  57 /* NS4150B 功放使能，高电平有效 */

/* ---------- 输出音频格式（与播放端一致） ---------- */
#define BSP_AUDIO_SAMPLE_RATE 44100
#define BSP_AUDIO_CHANNELS    2
#define BSP_AUDIO_BITS        16

/* 音量 0~100；80 ≈ ES8311 REG32=0xB2（约 -10dB），可按需调整 */
#define BSP_AUDIO_OUTPUT_VOLUME 40

#define BSP_I2C_CLK_HZ 400000

static esp_codec_dev_handle_t s_codec_dev = NULL;            /* esp_codec_dev 顶层句柄 */
static const audio_codec_gpio_if_t *s_gpio_if = NULL;        /* 提供给 Codec 的 GPIO 抽象（PA 控制） */
static const audio_codec_ctrl_if_t *s_ctrl_if = NULL;        /* I2C 控制接口（用于寄存器回读诊断） */
static i2s_chan_handle_t s_i2s_tx_handle = NULL;             /* 喂给 Codec DAC 的 I2S TX 通道 */
static i2c_master_bus_handle_t s_i2c_bus = NULL;             /* ES8311 的 I2C 主机总线 */
static bool s_opened = false;                                /* Codec 是否已打开（音频通路使能） */
static uint64_t s_write_bytes = 0;                          /* 累计写入 PCM 字节数（诊断用） */

/* 初始化 I2S TX 通道（标准 Philips/I2S 模式），后续由 esp_codec_dev_open 重配置为指定采样率 */
static esp_err_t bsp_i2s_init(void)
{
    i2s_chan_config_t chan_cfg = {
        .id = BSP_I2S_PORT,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear = true,
        .allow_pd = false,
        .intr_priority = 0,
        .tx_destination = I2S_DESTINATION_DMA,
        .rx_destination = I2S_DESTINATION_DMA,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx_handle, NULL), TAG,
                        "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BSP_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_GPIO_I2S_MCLK,
            .bclk = BSP_GPIO_I2S_SCLK,
            .ws = BSP_GPIO_I2S_LRCK,
            .dout = BSP_GPIO_I2S_DOUT,
            .din = BSP_GPIO_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx_handle, &std_cfg), TAG,
                        "i2s_channel_init_std_mode failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx_handle), TAG,
                        "i2s_channel_enable failed");
    return ESP_OK;
}

/* 初始化 I2C 主机总线。ES8311 8bit 地址 0x30（7bit 0x18） */
static esp_err_t bsp_i2c_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BSP_I2C_PORT,
        .sda_io_num = BSP_GPIO_I2C_SDA,
        .scl_io_num = BSP_GPIO_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
        .flags.allow_pd = false,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG,
                        "i2c_new_master_bus failed");
    return ESP_OK;
}

static esp_err_t bsp_codec_init(void)
{
    /* 控制接口：通过 I2C 访问 ES8311（地址 0x30，400kHz） */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
        .clock_speed_hz = BSP_I2C_CLK_HZ,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl failed");
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_ctrl_if = ctrl_if;

    /* GPIO 抽象（供 Codec 的 PA/复位控制使用） */
    s_gpio_if = audio_codec_new_gpio();
    if (s_gpio_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_gpio failed");
        return ESP_ERR_NO_MEM;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = BSP_I2S_PORT,
        .rx_handle = NULL,
        .tx_handle = s_i2s_tx_handle,
        .clk_src = 0,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (data_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_i2s_data failed");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* ES8311 Codec 配置（esp_codec_dev v2.0.0-beta1 结构体） */
    es8311_codec_cfg_t codec_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = s_gpio_if,
        .sys_cfg = {
            .is_master = false,   /* I2S 时钟由 ESP32 提供（主模式） */
            .no_mclk = false,     /* 使用 MCLK 引脚（GPIO52） */
        },
        .adc_cfg = {
            .digital_mic = false, /* 模拟麦克风路径，本模块未使用 */
            .label = NULL,
        },
        .dac_cfg = {
            /* 必须为 true：开启内部基准信号（REG44=0x58，"ADCL + DACR"）。
             * esp_codec_dev v2.0.0-beta1 默认 false（REG44=0x08），
             * 会导致 DAC 模拟级无声，即使其余寄存器均正常。 */
            .ref_enable = true,
            .ref_dac_ch = 0,
            .real_adc_data_ch = 0,
        },
        .pa_cfg = {
            /* PA 由本文件 bsp_es8311_set_pa() 显式控制，故 pa_pin=-1，
             * 避免驱动重复驱动 GPIO57（gpio conflict 告警）。 */
            .pa_pin = -1,
            .pa_active_low = false,
            .hw_gain = { 0 },
        },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&codec_cfg);
    if (codec_if == NULL) {
        ESP_LOGE(TAG, "es8311_codec_new failed");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* 顶层 Codec 设备：仅输出，绑定 codec_if + data_if */
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec_dev = esp_codec_dev_new(&dev_cfg);
    if (s_codec_dev == NULL) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "ES8311 codec dev initialized");
    return ESP_OK;
}

esp_err_t bsp_es8311_init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2s_init(), TAG, "bsp_i2s_init failed");
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "bsp_i2c_init failed");
    ESP_RETURN_ON_ERROR(bsp_codec_init(), TAG, "bsp_codec_init failed");
    return ESP_OK;
}

/* 驱动 NS4150B PA 使能引脚。
 * 使用 GPIO_MODE_INPUT_OUTPUT（而非纯 OUTPUT），以便后续 gpio_get_level()
 * 能真实读回引脚电平——纯 OUTPUT 模式输入缓冲被禁用，读取值恒为 0。 */
esp_err_t bsp_es8311_set_pa(bool on)
{
    gpio_config_t io = {
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pin_bit_mask = BIT64(BSP_GPIO_PA_CTRL),
        .pull_down_en = false,
        .pull_up_en = false,
    };
    gpio_config(&io);
    esp_err_t err = gpio_set_level(BSP_GPIO_PA_CTRL, on ? 1 : 0);
    ESP_LOGI(TAG, "PA gpio%d %s (rc=%d, level=%d)", BSP_GPIO_PA_CTRL,
             on ? "ON" : "OFF", err, gpio_get_level(BSP_GPIO_PA_CTRL));
    return err;
}

esp_err_t bsp_es8311_open(void)
{
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = BSP_AUDIO_SAMPLE_RATE,
        .channel = BSP_AUDIO_CHANNELS,
        .bits_per_sample = BSP_AUDIO_BITS,
        .mclk_multiple = 256,
    };
    if (esp_codec_dev_open(s_codec_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed");
        return ESP_FAIL;
    }
    /* 设置输出音量（0~100），随后使能 PA 让声音可闻 */
    if (esp_codec_dev_set_out_vol(s_codec_dev, BSP_AUDIO_OUTPUT_VOLUME) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Failed to set output volume, continue");
    }
    bsp_es8311_set_pa(true);
    s_opened = true;
    ESP_LOGI(TAG, "Codec opened, volume %d%%", BSP_AUDIO_OUTPUT_VOLUME);
    return ESP_OK;
}

esp_err_t bsp_es8311_close(void)
{
    if (s_opened) {
        esp_codec_dev_close(s_codec_dev);
        bsp_es8311_set_pa(false); /* 断电前先静音 */
        s_opened = false;
    }
    return ESP_OK;
}

esp_err_t bsp_es8311_write(const uint8_t *pcm, int bytes)
{
    if (!s_opened) {
        ESP_LOGW(TAG, "write before codec opened");
        return ESP_ERR_INVALID_STATE;
    }
    int ret = esp_codec_dev_write(s_codec_dev, (uint8_t *)pcm, bytes);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_write fail, ret=%d size=%d", ret, bytes);
        return ESP_FAIL;
    }
    s_write_bytes += bytes;
    return ESP_OK;
}

esp_err_t bsp_es8311_set_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    if (esp_codec_dev_set_out_vol(s_codec_dev, vol) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t bsp_es8311_get_volume(int *vol)
{
    if (vol == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (esp_codec_dev_get_out_vol(s_codec_dev, vol) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void bsp_es8311_diag(void)
{
    if (!s_opened) {
        ESP_LOGI(TAG, "diag: codec not opened");
        return;
    }
    int lvl = gpio_get_level(BSP_GPIO_PA_CTRL);
    gpio_set_level(BSP_GPIO_PA_CTRL, 0);
    int lvl0 = gpio_get_level(BSP_GPIO_PA_CTRL);
    gpio_set_level(BSP_GPIO_PA_CTRL, 1);
    int lvl1 = gpio_get_level(BSP_GPIO_PA_CTRL);
    ESP_LOGI(TAG, "diag: PA gpio%d level=%d, toggle low=%d high=%d, i2s bytes=%llu",
             BSP_GPIO_PA_CTRL, lvl, lvl0, lvl1, (unsigned long long)s_write_bytes);
    if (s_ctrl_if) {
        /* 关键 ES8311 寄存器：0D/0E 电源，12 DAC 使能，31 静音，32 音量，44 DAC 基准 */
        static const uint8_t regs[] = {0x00, 0x01, 0x02, 0x05, 0x09, 0x0D, 0x0E, 0x12,
                                       0x31, 0x32, 0x37, 0x44, 0x45};
        char buf[128];
        int n = 0;
        for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
            int v = 0;
            if (s_ctrl_if->read_reg(s_ctrl_if, regs[i], 1, &v, 1) == ESP_CODEC_DEV_OK) {
                n += snprintf(buf + n, sizeof(buf) - n, " %02X=%02X", regs[i], v & 0xFF);
            }
        }
        ESP_LOGI(TAG, "diag: es8311 regs:%s", buf);
    }
}
