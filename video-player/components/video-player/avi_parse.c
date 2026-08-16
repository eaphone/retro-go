#include <rg_system.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "avi_parse.h"

#define FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define AVI_STREAM_NONE UINT8_MAX

static inline uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static long padded_chunk_end(long data_offset, uint32_t size)
{
    return data_offset + (long)size + (size & 1U);
}

static int seek_to(FILE *file, long offset)
{
    return offset >= 0 && fseek(file, offset, SEEK_SET) == 0 ? 0 : -1;
}

static void parse_stream_list(FILE *file, long list_end, uint8_t stream_index, avi_info_t *info)
{
    uint32_t stream_type = 0;
    bool selected_video = false;
    bool selected_audio = false;

    while (ftell(file) >= 0 && ftell(file) + 8 <= list_end) {
        uint8_t header[8];
        if (fread(header, 1, sizeof(header), file) != sizeof(header)) break;

        uint32_t id = read_le32(header);
        uint32_t size = read_le32(header + 4);
        long data_offset = ftell(file);
        long next_offset = padded_chunk_end(data_offset, size);
        if (next_offset < data_offset || next_offset > list_end + 1) break;

        if (id == FOURCC('s', 't', 'r', 'h') && size >= 56) {
            uint8_t strh[56];
            if (fread(strh, 1, sizeof(strh), file) != sizeof(strh)) break;
            stream_type = read_le32(strh);

            if (stream_type == FOURCC('v', 'i', 'd', 's') &&
                info->video_stream_index == AVI_STREAM_NONE) {
                uint32_t scale = read_le32(strh + 20);
                uint32_t rate = read_le32(strh + 24);
                info->video_stream_index = stream_index;
                info->fps_numerator = rate;
                info->fps_denominator = scale;
                selected_video = true;
                RG_LOGI("Video stream %u: %lu/%lu fps", (unsigned)stream_index,
                        (unsigned long)rate, (unsigned long)scale);
            } else if (stream_type == FOURCC('a', 'u', 'd', 's') &&
                       info->audio_stream_index == AVI_STREAM_NONE) {
                info->audio_stream_index = stream_index;
                info->audio.total_samples = read_le32(strh + 32);
                info->has_audio = true;
                selected_audio = true;
                RG_LOGI("Audio stream %u found", (unsigned)stream_index);
            }
        } else if (id == FOURCC('s', 't', 'r', 'f')) {
            if (stream_type == FOURCC('v', 'i', 'd', 's') && selected_video && size >= 20) {
                uint8_t bitmap_info[20];
                if (fread(bitmap_info, 1, sizeof(bitmap_info), file) != sizeof(bitmap_info)) break;
                uint32_t compression = read_le32(bitmap_info + 16);
                if (compression == FOURCC('M', 'J', 'P', 'G')) {
                    RG_LOGI("MJPEG compression detected");
                } else {
                    RG_LOGW("Video compression 0x%08lX is not MJPEG", (unsigned long)compression);
                }
            } else if (stream_type == FOURCC('a', 'u', 'd', 's') && selected_audio && size >= 16) {
                uint8_t wave_format[16];
                if (fread(wave_format, 1, sizeof(wave_format), file) != sizeof(wave_format)) break;
                info->audio.format_tag = read_le16(wave_format);
                info->audio.channels = read_le16(wave_format + 2);
                info->audio.sample_rate = read_le32(wave_format + 4);
                info->audio.avg_bytes_per_sec = read_le32(wave_format + 8);
                info->audio.block_align = read_le16(wave_format + 12);
                info->audio.bits_per_sample = read_le16(wave_format + 14);
                RG_LOGI("Audio format: tag=%u, %lu Hz, %u channel(s), %u-bit, avg=%lu B/s, align=%u",
                        (unsigned)info->audio.format_tag, (unsigned long)info->audio.sample_rate,
                        (unsigned)info->audio.channels, (unsigned)info->audio.bits_per_sample,
                        (unsigned long)info->audio.avg_bytes_per_sec,
                        (unsigned)info->audio.block_align);
            }
        }

        if (seek_to(file, next_offset) != 0) break;
    }
}

static void parse_header_list(FILE *file, long list_end, avi_info_t *info)
{
    uint8_t stream_index = 0;

    while (ftell(file) >= 0 && ftell(file) + 8 <= list_end) {
        uint8_t header[8];
        if (fread(header, 1, sizeof(header), file) != sizeof(header)) break;

        uint32_t id = read_le32(header);
        uint32_t size = read_le32(header + 4);
        long data_offset = ftell(file);
        long next_offset = padded_chunk_end(data_offset, size);
        if (next_offset < data_offset || next_offset > list_end + 1) break;

        if (id == FOURCC('a', 'v', 'i', 'h') && size >= 40) {
            uint8_t avih[40];
            if (fread(avih, 1, sizeof(avih), file) != sizeof(avih)) break;
            info->microsec_per_frame = read_le32(avih);
            info->max_bytes_per_sec = read_le32(avih + 4);
            info->total_frames = read_le32(avih + 16);
            info->width = read_le32(avih + 32);
            info->height = read_le32(avih + 36);
        } else if (id == FOURCC('L', 'I', 'S', 'T') && size >= 4) {
            uint8_t list_type_data[4];
            if (fread(list_type_data, 1, sizeof(list_type_data), file) != sizeof(list_type_data)) break;
            if (read_le32(list_type_data) == FOURCC('s', 't', 'r', 'l')) {
                parse_stream_list(file, data_offset + size, stream_index++, info);
            }
        }

        if (seek_to(file, next_offset) != 0) break;
    }
}

static int avi_parse_info(const char *filepath, avi_info_t *info)
{
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        RG_LOGE("Failed to open file: %s", filepath);
        return -1;
    }

    memset(info, 0, sizeof(*info));
    info->video_stream_index = AVI_STREAM_NONE;
    info->audio_stream_index = AVI_STREAM_NONE;

    uint8_t riff_header[12];
    if (fread(riff_header, 1, sizeof(riff_header), file) != sizeof(riff_header) ||
        read_le32(riff_header) != FOURCC('R', 'I', 'F', 'F') ||
        read_le32(riff_header + 8) != FOURCC('A', 'V', 'I', ' ')) {
        RG_LOGE("Not a valid AVI file");
        fclose(file);
        return -1;
    }

    long riff_end = 8L + read_le32(riff_header + 4);
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    long file_end = ftell(file);
    if (riff_end > file_end) riff_end = file_end;
    if (seek_to(file, 12) != 0) {
        fclose(file);
        return -1;
    }

    while (ftell(file) >= 0 && ftell(file) + 8 <= riff_end) {
        uint8_t header[8];
        if (fread(header, 1, sizeof(header), file) != sizeof(header)) break;

        uint32_t id = read_le32(header);
        uint32_t size = read_le32(header + 4);
        long data_offset = ftell(file);
        long next_offset = padded_chunk_end(data_offset, size);
        if (next_offset < data_offset || next_offset > riff_end + 1) break;

        if (id == FOURCC('L', 'I', 'S', 'T') && size >= 4) {
            uint8_t list_type_data[4];
            if (fread(list_type_data, 1, sizeof(list_type_data), file) != sizeof(list_type_data)) break;
            uint32_t list_type = read_le32(list_type_data);
            if (list_type == FOURCC('h', 'd', 'r', 'l')) {
                parse_header_list(file, data_offset + size, info);
            } else if (list_type == FOURCC('m', 'o', 'v', 'i')) {
                info->movi_offset = (uint32_t)(data_offset + 4);
                info->movi_size = size - 4;
            }
        }

        if (seek_to(file, next_offset) != 0) break;
    }

    fclose(file);

    if (!info->microsec_per_frame && info->fps_numerator && info->fps_denominator) {
        info->microsec_per_frame = (uint32_t)
            ((1000000ULL * info->fps_denominator) / info->fps_numerator);
    }

    if (!info->width || !info->height || !info->total_frames || !info->movi_offset ||
        info->video_stream_index == AVI_STREAM_NONE) {
        RG_LOGE("Incomplete AVI header: %lux%lu, frames=%lu, movi=%lu, video_stream=%u",
                (unsigned long)info->width, (unsigned long)info->height,
                (unsigned long)info->total_frames, (unsigned long)info->movi_offset,
                (unsigned)info->video_stream_index);
        return -1;
    }

    return 0;
}

static uint32_t stream_chunk_id(uint8_t stream_index, char type0, char type1)
{
    return FOURCC('0' + ((stream_index / 10) % 10), '0' + (stream_index % 10), type0, type1);
}

// Scans linearly through movi, descending into optional LIST 'rec ' chunks.
static int find_next_stream_chunk(FILE *file, uint32_t movi_end, uint8_t stream_index,
                                  bool video, uint32_t *out_offset, uint32_t *out_size)
{
    uint32_t wanted0 = stream_chunk_id(stream_index, video ? 'd' : 'w', video ? 'c' : 'b');
    uint32_t wanted1 = video ? stream_chunk_id(stream_index, 'd', 'b') : wanted0;

    while (ftell(file) >= 0 && (uint32_t)ftell(file) + 8 <= movi_end) {
        uint8_t header[8];
        if (fread(header, 1, sizeof(header), file) != sizeof(header)) return -1;

        uint32_t id = read_le32(header);
        uint32_t size = read_le32(header + 4);
        long data_offset = ftell(file);
        long next_offset = padded_chunk_end(data_offset, size);
        if (next_offset < data_offset || (uint32_t)next_offset > movi_end + 1U) return -1;

        if ((id == wanted0 || id == wanted1) && size > 0) {
            *out_offset = (uint32_t)data_offset;
            *out_size = size;
            return 0;
        }

        if ((id == FOURCC('L', 'I', 'S', 'T') || id == FOURCC('R', 'I', 'F', 'F')) && size >= 4) {
            // Skip only the list type and continue into its child chunks.
            if (seek_to(file, data_offset + 4) != 0) return -1;
        } else if (seek_to(file, next_offset) != 0) {
            return -1;
        }
    }

    return -1;
}

static int prime_stream(FILE *file, const avi_player_t *player, uint8_t stream_index,
                        bool video, uint32_t *offset, uint32_t *size)
{
    if (seek_to(file, player->movi_data_start) != 0) return -1;
    return find_next_stream_chunk(file, player->movi_data_end, stream_index,
                                  video, offset, size);
}

int avi_player_init(avi_player_t *player, const char *filepath)
{
    if (!player || !filepath) return -1;
    memset(player, 0, sizeof(*player));

    if (avi_parse_info(filepath, &player->info) != 0) return -1;

    player->movi_data_start = player->info.movi_offset;
    player->movi_data_end = player->movi_data_start + player->info.movi_size;
    if (player->movi_data_end < player->movi_data_start) return -1;

    player->video_file = fopen(filepath, "rb");
    if (!player->video_file) return -1;

    if (prime_stream(player->video_file, player, player->info.video_stream_index, true,
                     &player->next_frame_offset, &player->next_frame_size) != 0) {
        RG_LOGE("No MJPEG frames found in AVI movi list");
        avi_player_close(player);
        return -1;
    }

    avi_audio_info_t *audio = &player->info.audio;
    bool pcm_supported = audio->format_tag == AVI_AUDIO_FORMAT_PCM &&
        audio->sample_rate && audio->channels && audio->channels <= 2 && audio->block_align &&
        (audio->bits_per_sample == 8 || audio->bits_per_sample == 16 ||
         audio->bits_per_sample == 24 || audio->bits_per_sample == 32) &&
        audio->block_align >= audio->channels * (audio->bits_per_sample / 8U);
    bool mp3_supported = (audio->format_tag == AVI_AUDIO_FORMAT_MPEG ||
                          audio->format_tag == AVI_AUDIO_FORMAT_MP3) &&
        audio->sample_rate && audio->channels && audio->channels <= 2;
    if (player->info.has_audio && !pcm_supported && !mp3_supported) {
        RG_LOGW("Unsupported AVI audio tag=%u; PCM and MPEG Layer I/II/III are supported; playing silently",
                (unsigned)audio->format_tag);
        player->info.has_audio = false;
    }

    if (player->info.has_audio) {
        player->audio_file = fopen(filepath, "rb");
        if (!player->audio_file ||
            prime_stream(player->audio_file, player, player->info.audio_stream_index, false,
                         &player->audio_next_offset, &player->audio_next_size) != 0) {
            RG_LOGW("No audio chunks found; playing silently");
            if (player->audio_file) fclose(player->audio_file);
            player->audio_file = NULL;
            player->info.has_audio = false;
        }
    }

    const char *audio_name = !player->info.has_audio ? "none" :
        (player->info.audio.format_tag == AVI_AUDIO_FORMAT_PCM ? "PCM" : "MPEG audio");
    RG_LOGI("AVI ready: %lux%lu, %lu frames, %.2f fps, audio=%s",
            (unsigned long)player->info.width, (unsigned long)player->info.height,
            (unsigned long)player->info.total_frames,
            player->info.microsec_per_frame ? 1000000.0f / player->info.microsec_per_frame : 0.0f,
            audio_name);
    return 0;
}

static int read_current_chunk(FILE *file, uint32_t current_offset, uint32_t current_size,
                              uint32_t movi_end, uint8_t stream_index, bool video,
                              uint32_t *next_offset, uint32_t *next_size,
                              uint8_t **out_data, uint32_t *out_size)
{
    uint8_t *data = malloc(current_size);
    if (!data) {
        RG_LOGE("Failed to allocate %lu-byte AVI chunk", (unsigned long)current_size);
        return -1;
    }

    if (seek_to(file, current_offset) != 0 || fread(data, 1, current_size, file) != current_size) {
        free(data);
        return -1;
    }

    long scan_offset = padded_chunk_end(current_offset, current_size);
    *next_offset = 0;
    *next_size = 0;
    if (seek_to(file, scan_offset) == 0) {
        (void)find_next_stream_chunk(file, movi_end, stream_index, video, next_offset, next_size);
    }

    *out_data = data;
    *out_size = current_size;
    return 0;
}

int avi_player_get_next_frame(avi_player_t *player, uint8_t **out_data, uint32_t *out_size)
{
    if (!player || !player->video_file || !out_data || !out_size) return -1;
    if (!player->next_frame_offset || player->current_frame >= player->info.total_frames) return 1;

    int result = read_current_chunk(player->video_file,
                                    player->next_frame_offset, player->next_frame_size,
                                    player->movi_data_end, player->info.video_stream_index, true,
                                    &player->next_frame_offset, &player->next_frame_size,
                                    out_data, out_size);
    if (result == 0) player->current_frame++;
    return result;
}

int avi_player_get_next_audio(avi_player_t *player, uint8_t **out_data, uint32_t *out_size)
{
    if (!player || !player->audio_file || !out_data || !out_size || !player->info.has_audio) return -1;
    if (!player->audio_next_offset) return 1;

    return read_current_chunk(player->audio_file,
                              player->audio_next_offset, player->audio_next_size,
                              player->movi_data_end, player->info.audio_stream_index, false,
                              &player->audio_next_offset, &player->audio_next_size,
                              out_data, out_size);
}

void avi_player_close(avi_player_t *player)
{
    if (!player) return;
    if (player->video_file) fclose(player->video_file);
    if (player->audio_file) fclose(player->audio_file);
    memset(player, 0, sizeof(*player));
}

const avi_info_t *avi_player_get_info(avi_player_t *player)
{
    return player ? &player->info : NULL;
}

int avi_player_has_more_frames(avi_player_t *player)
{
    return player && player->next_frame_offset && player->current_frame < player->info.total_frames;
}

uint32_t avi_player_get_current_frame(avi_player_t *player)
{
    return player ? player->current_frame : 0;
}

int avi_player_seek_to_frame(avi_player_t *player, uint32_t frame_index)
{
    if (!player || !player->video_file || !player->info.total_frames) return -1;

    if (frame_index >= player->info.total_frames) {
        frame_index = player->info.total_frames - 1;
    }

    if (seek_to(player->video_file, player->movi_data_start) != 0) return -1;

    uint32_t frame_offset = 0;
    uint32_t frame_size = 0;
    for (uint32_t index = 0; index <= frame_index; ++index) {
        if (find_next_stream_chunk(player->video_file, player->movi_data_end,
                                   player->info.video_stream_index, true,
                                   &frame_offset, &frame_size) != 0) {
            return -1;
        }

        if (index < frame_index &&
            seek_to(player->video_file, padded_chunk_end(frame_offset, frame_size)) != 0) {
            return -1;
        }
    }

    player->next_frame_offset = frame_offset;
    player->next_frame_size = frame_size;
    player->current_frame = frame_index;

    // AVI media chunks are normally interleaved in playback order. Select the
    // audio chunk nearest to the target video chunk so playback can resume
    // without scanning or decoding from the beginning of the file.
    if (player->info.has_audio && player->audio_file) {
        uint32_t audio_offset = 0;
        uint32_t audio_size = 0;
        uint32_t previous_offset = 0;
        uint32_t previous_size = 0;

        if (seek_to(player->audio_file, player->movi_data_start) != 0) return -1;
        while (find_next_stream_chunk(player->audio_file, player->movi_data_end,
                                      player->info.audio_stream_index, false,
                                      &audio_offset, &audio_size) == 0) {
            if (audio_offset >= frame_offset) break;
            previous_offset = audio_offset;
            previous_size = audio_size;
            if (seek_to(player->audio_file,
                        padded_chunk_end(audio_offset, audio_size)) != 0) {
                audio_offset = 0;
                audio_size = 0;
                break;
            }
            audio_offset = 0;
            audio_size = 0;
        }

        if (!audio_offset && previous_offset) {
            audio_offset = previous_offset;
            audio_size = previous_size;
        }
        player->audio_next_offset = audio_offset;
        player->audio_next_size = audio_size;
    }

    RG_LOGI("Seeked to frame %lu/%lu", (unsigned long)frame_index,
            (unsigned long)player->info.total_frames);
    return 0;
}
