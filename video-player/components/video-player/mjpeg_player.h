#ifndef _MJPEG_PLAYER_H_
#define _MJPEG_PLAYER_H_

#include <stdint.h>
#include <stdbool.h>

/**
 * 播放 MJPEG 视频
 * @param filepath AVI 文件路径
 * @param framebuffer 帧缓冲区 (RGB565 格式)
 * @param screen_width 屏幕宽度
 * @param screen_height 屏幕高度
 * @return 0 成功, -1 失败
 */
int mjpeg_play(const char *filepath, int screen_width, int screen_height);

#endif /* _MJPEG_PLAYER_H_ */