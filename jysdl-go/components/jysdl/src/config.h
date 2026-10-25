

// 全局配置

#ifndef __CONFIG_H
#define __CONFIG_H


// 运行时文件名（相对于工作目录）

#define CONFIG_FILE "config.lua"
#define DEBUG_FILE "debug.log"
#define ERROR_FILE "error.log"


#if defined(_MSC_VER)
// 高精度时钟（仅x86调试用，正式版本不启用）
    // #define HIGH_PRECISION_CLOCK
    #define CPU_FREQUENCY 2000    // CPU频率(MHz)
#endif

#ifdef __cplusplus
#define EXTERN_C_BEGIN extern "C" {
#else
#define EXTERN_C_BEGIN
#endif

#ifdef __cplusplus
#define EXTERN_C_END }
#else
#define EXTERN_C_END
#endif

#endif


