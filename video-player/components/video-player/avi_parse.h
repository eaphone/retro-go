#ifndef _AVI_PARSE_H_
#define _AVI_PARSE_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AVI_AUDIO_FORMAT_PCM 0x0001
#define AVI_AUDIO_FORMAT_MPEG 0x0050
#define AVI_AUDIO_FORMAT_MP3 0x0055

// Audio stream information from the AVI WAVEFORMATEX header.
typedef struct {
    uint16_t format_tag;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t avg_bytes_per_sec;
    uint32_t block_align;
    uint32_t total_samples;
} avi_audio_info_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t total_frames;
    uint32_t fps_numerator;
    uint32_t fps_denominator;
    uint32_t microsec_per_frame;
    uint32_t max_bytes_per_sec;
    uint32_t movi_offset;
    uint32_t movi_size;
    uint8_t video_stream_index;
    uint8_t audio_stream_index;
    avi_audio_info_t audio;
    bool has_audio;
} avi_info_t;

typedef struct {
    // Video and audio use separate handles so they can seek independently
    // through the interleaved movi list.
    FILE *video_file;
    FILE *audio_file;
    avi_info_t info;
    uint32_t current_frame;
    uint32_t movi_data_start;
    uint32_t movi_data_end;
    uint32_t next_frame_offset;
    uint32_t next_frame_size;
    uint32_t audio_next_offset;
    uint32_t audio_next_size;
} avi_player_t;

int avi_player_init(avi_player_t *player, const char *filepath);
int avi_player_get_next_frame(avi_player_t *player, uint8_t **out_data, uint32_t *out_size);
int avi_player_get_next_audio(avi_player_t *player, uint8_t **out_data, uint32_t *out_size);
void avi_player_close(avi_player_t *player);
const avi_info_t *avi_player_get_info(avi_player_t *player);
int avi_player_has_more_frames(avi_player_t *player);
uint32_t avi_player_get_current_frame(avi_player_t *player);
int avi_player_seek_to_frame(avi_player_t *player, uint32_t frame_index);

#ifdef __cplusplus
}
#endif

#endif /* _AVI_PARSE_H_ */
