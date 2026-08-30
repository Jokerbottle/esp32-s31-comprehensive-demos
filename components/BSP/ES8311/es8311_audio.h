#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 ES8311 音频功放硬件（I2S + I2C + ES8311 Codec + PA）
 *
 * 完成以下工作：
 *  1. 初始化 I2S 标准模式发送通道（连接 Codec 的 DAC 输入）；
 *  2. 初始化 I2C 主机（连接 ES8311，地址 0x30）；
 *  3. 创建 esp_codec_dev 设备并初始化 ES8311。
 *
 * 调用本函数前需先初始化 NVS / WiFi 等无关，但需保证硬件引脚未被占用。
 *
 * @return esp_err_t ESP_OK 表示初始化成功
 */
esp_err_t bsp_es8311_init(void);

/**
 * @brief 打开音频通路
 *
 * 打开 Codec 设备、设置输出音量（默认 BOARD_AUDIO_OUTPUT_VOLUME），
 * 并使能 NS4150B 功放（PA 引脚拉高），此后即可通过 bsp_es8311_write 写入 PCM 数据。
 *
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t bsp_es8311_open(void);

/**
 * @brief 关闭音频通路
 *
 * 关闭 Codec 设备并禁用功放（PA 引脚拉低），避免静音时喇叭有底噪。
 *
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t bsp_es8311_close(void);

/**
 * @brief 向 Codec 写入 PCM 音频数据（播放）
 *
 * @param[in] pcm   指向 16bit 交织立体声 PCM 数据的缓冲区
 * @param[in] bytes 数据字节数（应为帧大小的整数倍）
 * @return esp_err_t ESP_OK 表示写入成功；未打开时返回 ESP_ERR_INVALID_STATE
 */
esp_err_t bsp_es8311_write(const uint8_t *pcm, int bytes);

/**
 * @brief 设置输出音量
 *
 * @param[in] vol 音量值 0~100
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t bsp_es8311_set_volume(int vol);

/**
 * @brief 读取当前输出音量
 *
 * @param[out] vol 用于存放音量值 0~100 的指针
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t bsp_es8311_get_volume(int *vol);

/**
 * @brief 显式控制功放（PA）使能
 *
 * @param[in] on true 使能（拉高 PA 引脚），false 关闭（拉低）
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t bsp_es8311_set_pa(bool on);

/**
 * @brief 音频诊断输出（PA 电平 / I2S 累计字节数 / ES8311 关键寄存器）
 *
 * 便于通过串口确认硬件链路是否正常，建议在状态心跳中周期调用。
 */
void bsp_es8311_diag(void);

#ifdef __cplusplus
}
#endif
