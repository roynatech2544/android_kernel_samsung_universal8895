#ifndef _SEC_AUDIO_DEBUG_H
#define _SEC_AUDIO_DEBUG_H

#ifdef CONFIG_SND_SOC_SAMSUNG_AUDIO
void sec_audio_log(int level, const char *fmt, ...);
int alloc_sec_audio_log(int buffer_len);
void free_sec_audio_log(void);
#else
inline void sec_audio_log(int level, const char *fmt, ...)
{
}

inline int alloc_sec_audio_log(int buffer_len)
{
	return -EACCES;
}

inline void free_sec_audio_log(void)
{
}
#endif

#define adev_err(fmt, arg...) sec_audio_log(3, fmt, ##arg)
#define adev_info(fmt, arg...) sec_audio_log(6, fmt, ##arg)
#define adev_dbg(fmt, arg...) sec_audio_log(7, fmt, ##arg)
#endif
