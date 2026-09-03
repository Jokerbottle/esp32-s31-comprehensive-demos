/*
 * player_ctrl.h - 播放控制器（接收 web WebSocket 指令，驱动 audio_player）
 *
 * 职责：
 *  - 解析来自 ws_server 的播放/暂停/停止/音量指令并驱动 audio_player；
 *  - 状态机：IDLE -> PREPARING -> PLAYING <-> PAUSED -> IDLE，无歌曲时 IDLE 待命；
 *  - 向 web 反馈事件：状态变化 / 自然播完 / 播放失败 / 播放进度 / 系统负载；
 *  - 停滞看门狗：进度 30 秒无推进强制跳过并上报失败（由 web 决定跳下一首）。
 *
 * 播放协议（web -> s31，JSON 文本帧）：
 *   {"cmd":"play","id":"kw_123","alt":["mg_x","kg_y"]}
 *                                  播放指定歌曲；alt 为同一首歌的备选源编号
 *                                  （≤4 个），失败自动依次切换以提高成功率
 *   {"cmd":"stop"}                 停止
 *   {"cmd":"pause"} / {"cmd":"resume"}
 *   {"cmd":"volume","value":40}    设置音量（s31 存 NVS）
 *   {"cmd":"hello"}                web 接入后触发（s31 应答 hello 帧同步状态）
 *   {"cmd":"ping"}                 心率探测（回复 pong）
 *
 * 事件（s31 -> web，JSON 文本帧）：
 *   {"evt":"hello","volume":40,"state":"idle","id":""}      hello 指令的应答
 *   {"evt":"state","state":"preparing|playing|paused|idle","id":"..."}
 *   {"evt":"finished","id":"..."}                            自然播完（web 发下一首）
 *   {"evt":"error","id":"...","msg":"..."}                   全部候选源失败（web 跳过）
 *   {"evt":"progress","id":"...","pos":1234,"dur":56789}     每秒一次
 *   {"evt":"volume","value":40}                              音量回执
 *   {"evt":"sys","cpu":12,"heap":123456,"rssi":-50}          每 5 秒
 *   {"evt":"log","text":"..."}                               过程信息（如自动换源）
 *   {"evt":"pong"}
 */
#pragma once

#include "esp_err.h"
#include "audio_player.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 绑定播放器实例（在 audio_player_init 之后、start 之前调用）
 */
esp_err_t player_ctrl_init(audio_player_t *player);

/**
 * @brief 创建播放控制任务并进入 IDLE 待命（等 web 指令）
 */
esp_err_t player_ctrl_start(void);

/**
 * @brief ws_server 收到客户端文本帧后调用（任意任务上下文，内部快速返回）
 *
 * @param[in] text JSON 文本（函数内部复制，调用后可释放）
 */
void player_ctrl_handle_message(const char *text);

/**
 * @brief ws_server 有新客户端接入时调用（发送 hello 帧同步当前状态）
 */
void player_ctrl_on_client(void);

/**
 * @brief 构造 hello JSON 帧（内容：当前音量 / 状态 / 曲目 id）
 *
 * @param[out] buf  输出缓冲区
 * @param[in]  size 缓冲区大小
 * @return 写入的字符串长度
 */
int player_ctrl_build_hello(char *buf, int size);

/**
 * @brief 读取 CPU 占用率（0~100；未启用运行时统计时返回 -1）
 */
int player_ctrl_get_cpu_load(void);

#ifdef __cplusplus
}
#endif
