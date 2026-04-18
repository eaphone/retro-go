#include <rg_system.h>
#include "avi_parse.h"

// 读取 32 位小端整数
static inline uint32_t read_le32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

// 读取 16 位小端整数
static inline uint16_t read_le16(const uint8_t* p) {
    return p[0] | (p[1] << 8);
}

// 单独的 AVI 解析函数，不改变文件指针位置
static int avi_parse_info(const char* filepath, avi_info_t* info) {
    FILE* file = fopen(filepath, "rb");
    if (!file) {
        RG_LOGE("Failed to open file: %s", filepath);
        return -1;
    }
    
    memset(info, 0, sizeof(avi_info_t));
    
    // 读取 RIFF 头
    uint8_t riff_header[12];
    if (fread(riff_header, 1, 12, file) != 12) {
        RG_LOGE("Failed to read RIFF header");
        fclose(file);
        return -1;
    }
    
    uint32_t riff_id = read_le32(&riff_header[0]);
    uint32_t file_size = read_le32(&riff_header[4]);
    uint32_t avi_id = read_le32(&riff_header[8]);
    
    if (riff_id != 0x46464952 || avi_id != 0x20495641) {
        RG_LOGE("Not a valid AVI file");
        fclose(file);
        return -1;
    }
    
    RG_LOGI("Valid AVI file, size=%lu", file_size);
    
    // 解析 chunks
    while (ftell(file) < file_size + 8) {
        uint8_t chunk[8];
        if (fread(chunk, 1, 8, file) != 8) break;
        
        uint32_t chunk_id = read_le32(&chunk[0]);
        uint32_t chunk_size = read_le32(&chunk[4]);
        
        if (chunk_id == 0x5453494c) { // 'LIST'
            uint8_t list_type[4];
            if (fread(list_type, 1, 4, file) != 4) break;
            uint32_t type = read_le32(list_type);
            
            if (type == 0x6c726468) { // 'hdrl'
                RG_LOGI("Found hdrl LIST at offset %ld", ftell(file) - 12);
                
                // 解析 hdrl
                uint32_t hdrl_end = ftell(file) + chunk_size - 4;
                while (ftell(file) < hdrl_end) {
                    uint8_t subchunk[8];
                    if (fread(subchunk, 1, 8, file) != 8) break;
                    
                    uint32_t sub_id = read_le32(&subchunk[0]);
                    uint32_t sub_size = read_le32(&subchunk[4]);
                    
                    if (sub_id == 0x68697661) { // 'avih'
                        uint8_t avih_data[56];
                        if (fread(avih_data, 1, 56, file) == 56) {
                            info->microsec_per_frame = read_le32(&avih_data[0]);
                            info->max_bytes_per_sec = read_le32(&avih_data[4]);
                            info->total_frames = read_le32(&avih_data[16]);
                            info->width = read_le32(&avih_data[32]);
                            info->height = read_le32(&avih_data[36]);
                            RG_LOGI("AVIH: %lux%lu, %lu frames, %.2f fps",
                                    info->width, info->height, info->total_frames,
                                    1000000.0f / info->microsec_per_frame);
                        }
                    } 
                    else if (sub_id == 0x6c727473) { // 'strl'
                        uint8_t strl_size[4];
                        if (fread(strl_size, 1, 4, file) != 4) break;
                        uint32_t strl_list_size = read_le32(strl_size);
                        uint32_t strl_end = ftell(file) + strl_list_size - 4;
                        
                        while (ftell(file) < strl_end) {
                            uint8_t str_chunk[8];
                            if (fread(str_chunk, 1, 8, file) != 8) break;
                            
                            uint32_t str_id = read_le32(&str_chunk[0]);
                            uint32_t str_size = read_le32(&str_chunk[4]);
                            
                            if (str_id == 0x68727473) { // 'strh'
                                uint8_t strh_data[56];
                                if (fread(strh_data, 1, 56, file) == 56) {
                                    uint32_t stream_type = read_le32(&strh_data[0]);
                                    if (stream_type == 0x73646976) { // 'vids'
                                        info->fps_numerator = read_le32(&strh_data[20]);   // Rate
                                        info->fps_denominator = read_le32(&strh_data[16]); // Scale
                                        RG_LOGI("Video stream: %lu/%lu fps",
                                                info->fps_numerator, info->fps_denominator);
                                    }
                                }
                            }
                            else if (str_id == 0x66727473) { // 'strf'
                                uint8_t strf_data[40];
                                if (fread(strf_data, 1, 40, file) == 40) {
                                    uint32_t compression = read_le32(&strf_data[16]);
                                    if (compression == 0x47504a4d) {
                                        RG_LOGI("MJPEG compression detected");
                                    }
                                }
                            }
                            else {
                                fseek(file, str_size, SEEK_CUR);
                            }
                        }
                    }
                    else {
                        fseek(file, sub_size, SEEK_CUR);
                    }
                }
            } 
            else if (type == 0x69766f6d) { // 'movi'
                RG_LOGI("Found movi LIST at offset %ld", ftell(file) - 12);
                info->movi_offset = ftell(file) - 12;
                // 找到 movi 后，不需要继续解析了
                fclose(file);
                return 0;
            }
            else {
                fseek(file, chunk_size - 4, SEEK_CUR);
            }
        }
        else {
            fseek(file, chunk_size, SEEK_CUR);
        }
    }
    
    fclose(file);
    return (info->movi_offset != 0) ? 0 : -1;
}

static int find_next_video_frame(FILE* file, uint32_t* out_offset, uint32_t* out_size) {
    int max_attempts = 10000;
    int attempts = 0;
    
    while (attempts++ < max_attempts) {
        long pos = ftell(file);
        
        // 检查是否接近文件末尾
        fseek(file, 0, SEEK_END);
        long file_end = ftell(file);
        fseek(file, pos, SEEK_SET);
        
        if (pos + 8 > file_end) {
            RG_LOGI("Reached end of file at offset %ld", pos);
            return -1;
        }
        
        uint8_t chunk[8];
        if (fread(chunk, 1, 8, file) != 8) {
            RG_LOGE("Failed to read chunk at %ld", pos);
            return -1;
        }

        uint32_t chunk_id = read_le32(&chunk[0]);
        uint32_t chunk_size = read_le32(&chunk[4]);

        RG_LOGI("Raw chunk header at %ld: %02X %02X %02X %02X %02X %02X %02X %02X", 
                pos, chunk[0], chunk[1], chunk[2], chunk[3], 
                chunk[4], chunk[5], chunk[6], chunk[7]);
        RG_LOGI("chunk_id=0x%08lX, chunk_size=%lu", (unsigned long)chunk_id, chunk_size);
        
        // 检查是否是视频帧 (00dc, 01dc, etc.)
        if (chunk_id == 0x63643030 ||  // '00dc'
            chunk_id == 0x63643031 ||  // '01dc'
            chunk_id == 0x63643032) {  // '02dc'
            *out_offset = ftell(file);  // 当前是数据开始位置
            *out_size = chunk_size;
            RG_LOGI("Found video frame at offset %lu, size=%lu, id=0x%08lX", 
                    *out_offset, *out_size, (unsigned long)chunk_id);
            return 0;
        }
        
        // 跳过非视频块
        if (fseek(file, chunk_size, SEEK_CUR) != 0) {
            RG_LOGE("Failed to seek past chunk at %ld, size=%lu", pos, chunk_size);
            return -1;
        }
        if (chunk_size & 1) {
            fseek(file, 1, SEEK_CUR);  // 对齐到偶数边界
        }
    }
    
    RG_LOGE("No video frame found after %d attempts", attempts);
    return -1;
}

int avi_player_init(avi_player_t* player, const char* filepath) {
    if (!player || !filepath) return -1;
    
    memset(player, 0, sizeof(avi_player_t));
    
    // 先解析 AVI 信息
    if (avi_parse_info(filepath, &player->info) != 0) {
        RG_LOGE("Failed to parse AVI info");
        return -1;
    }
    
    if (player->info.width == 0 || player->info.total_frames == 0 || player->info.movi_offset == 0) {
        RG_LOGE("Invalid AVI file: width=%lu, frames=%lu, movi_offset=%lu", 
                player->info.width, player->info.total_frames, player->info.movi_offset);
        return -1;
    }
    
    // 打开文件用于读取帧数据
    player->file = fopen(filepath, "rb");
    if (!player->file) {
        RG_LOGE("Failed to open AVI file: %s", filepath);
        return -1;
    }
    
    // 定位到 movi LIST 块
    if (fseek(player->file, player->info.movi_offset, SEEK_SET) != 0) {
        RG_LOGE("Failed to seek to movi offset %lu", player->info.movi_offset);
        fclose(player->file);
        player->file = NULL;
        return -1;
    }
    
    // 读取并验证 LIST 头
    uint8_t list_header[12];
    if (fread(list_header, 1, 12, player->file) != 12) {
        RG_LOGE("Failed to read LIST header at offset %lu", player->info.movi_offset);
        fclose(player->file);
        player->file = NULL;
        return -1;
    }
    
    uint32_t list_id = read_le32(&list_header[0]);
    uint32_t list_size = read_le32(&list_header[4]);
    uint32_t list_type = read_le32(&list_header[8]);
    
    RG_LOGI("movi LIST: ID=0x%08lX, Size=%lu, Type=0x%08lX", 
            (unsigned long)list_id, list_size, (unsigned long)list_type);
    
    if (list_id != 0x5453494c) {  // 'LIST'
        RG_LOGE("Invalid LIST ID: 0x%08lX (expected 0x5453494c)", (unsigned long)list_id);
        fclose(player->file);
        player->file = NULL;
        return -1;
    }
    
    if (list_type != 0x69766f6d) {  // 'movi'
        RG_LOGE("Invalid LIST type: 0x%08lX (expected 0x69766f6d)", (unsigned long)list_type);
        fclose(player->file);
        player->file = NULL;
        return -1;
    }
    
    // movi 数据开始位置
    player->movi_data_start = ftell(player->file);
    RG_LOGI("movi data starts at offset %lu, list_size=%lu", player->movi_data_start, list_size);
    
    // 查找第一帧并保存其位置
    if (find_next_video_frame(player->file, &player->next_frame_offset, &player->next_frame_size) != 0) {
        RG_LOGE("No video frame found in movi chunk");
        fclose(player->file);
        player->file = NULL;
        return -1;
    }
    
    player->current_frame = 0;
    
    RG_LOGI("AVI player initialized: %lu x %lu, %lu frames, %.2f fps", 
            player->info.width, player->info.height, player->info.total_frames,
            1000000.0f / player->info.microsec_per_frame);
    
    return 0;
}

int avi_player_get_next_frame(avi_player_t* player, uint8_t** out_data, uint32_t* out_size) {
    if (!player || !player->file || !out_data || !out_size) return -1;
    if (player->current_frame >= player->info.total_frames) {
        RG_LOGI("End of video reached");
        return 1;
    }
    
    // 使用保存的下一帧位置
    uint32_t frame_offset = player->next_frame_offset;
    uint32_t frame_size = player->next_frame_size;
    
    if (frame_offset == 0) {
        RG_LOGE("No frame offset available");
        return -1;
    }
    
    // 读取帧数据
    uint8_t* frame_data = (uint8_t*)malloc(frame_size);
    if (!frame_data) {
        RG_LOGE("Failed to allocate memory: %lu bytes", frame_size);
        return -1;
    }
    
    if (fseek(player->file, frame_offset, SEEK_SET) != 0) {
        RG_LOGE("Failed to seek to frame offset %lu", frame_offset);
        free(frame_data);
        return -1;
    }
    
    if (fread(frame_data, 1, frame_size, player->file) != frame_size) {
        RG_LOGE("Failed to read frame data");
        free(frame_data);
        return -1;
    }
    
    // 定位到下一帧的位置
    long next_pos = frame_offset + frame_size;
    if (frame_size & 1) next_pos++;
    
    if (fseek(player->file, next_pos, SEEK_SET) != 0) {
        RG_LOGE("Failed to seek to next frame position");
        free(frame_data);
        return -1;
    }
    
    // 查找下一帧
    uint32_t next_offset, next_size;
    if (find_next_video_frame(player->file, &next_offset, &next_size) == 0) {
        player->next_frame_offset = next_offset;
        player->next_frame_size = next_size;
    } else {
        // 没有更多帧了
        player->next_frame_offset = 0;
        player->next_frame_size = 0;
    }
    
    *out_data = frame_data;
    *out_size = frame_size;
    player->current_frame++;
    
    RG_LOGI("Frame %lu read, size=%lu, next offset=%lu", 
            player->current_frame, frame_size, player->next_frame_offset);
    
    return 0;
}

void avi_player_close(avi_player_t* player) {
    if (!player) return;
    if (player->file) {
        fclose(player->file);
        player->file = NULL;
    }
    memset(player, 0, sizeof(avi_player_t));
}

const avi_info_t* avi_player_get_info(avi_player_t* player) {
    return player ? &player->info : NULL;
}

int avi_player_has_more_frames(avi_player_t* player) {
    return player ? (player->current_frame < player->info.total_frames) : 0;
}

uint32_t avi_player_get_current_frame(avi_player_t* player) {
    return player ? player->current_frame : 0;
}

int avi_player_seek_to_frame(avi_player_t* player, uint32_t frame_index) {
    // TODO: Implement seeking
    (void)player;
    (void)frame_index;
    RG_LOGW("Seek not implemented yet");
    return -1;
}