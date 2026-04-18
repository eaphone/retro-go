#ifndef _AVI_PARSE_H_
#define _AVI_PARSE_H_

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// AVI 信息结构体
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t total_frames;
    uint32_t fps_numerator;      // Rate (帧率分子)
    uint32_t fps_denominator;    // Scale (帧率分母)
    uint32_t microsec_per_frame; // 微秒每帧
    uint32_t max_bytes_per_sec;
    uint32_t movi_offset;        // movi LIST 块在文件中的偏移
} avi_info_t;

// 播放器上下文
typedef struct {
    FILE* file;
    avi_info_t info;
    uint32_t current_frame;
    uint32_t movi_data_start;    // movi 数据区起始偏移
    uint32_t next_frame_offset;  // 下一帧的偏移（预读）
    uint32_t next_frame_size;    // 下一帧的大小
} avi_player_t;

// 函数声明
int avi_player_init(avi_player_t* player, const char* filepath);
int avi_player_get_next_frame(avi_player_t* player, uint8_t** out_data, uint32_t* out_size);
void avi_player_close(avi_player_t* player);
const avi_info_t* avi_player_get_info(avi_player_t* player);
int avi_player_has_more_frames(avi_player_t* player);
uint32_t avi_player_get_current_frame(avi_player_t* player);
int avi_player_seek_to_frame(avi_player_t* player, uint32_t frame_index);

#ifdef __cplusplus
}
#endif

#endif /* _AVI_PARSE_H_ */