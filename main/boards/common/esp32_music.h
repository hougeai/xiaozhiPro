#ifndef ESP32_MUSIC_H
#define ESP32_MUSIC_H

#include <cstdint>
#include <string>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <map>        // 用到了map
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// MP3解码器支持
extern "C" {
#include "mp3dec.h"
}

// 音频数据块结构
struct AudioChunk {
    uint8_t* data;
    size_t size;
    bool owned;
    
    AudioChunk() : data(nullptr), size(0) {}
    AudioChunk(uint8_t* d, size_t s, bool o = true) : data(d), size(s), owned(o) {}
};

class Esp32Music {
private:
    enum AudioFormat {
        FORMAT_UNKNOWN,
        FORMAT_MP3,
        FORMAT_WAV,
        FORMAT_FLAC,
        FORMAT_OGG
    } detected_format_ = FORMAT_UNKNOWN;
    std::string last_downloaded_data_;
    std::string current_music_url_;
    std::string current_song_name_;
    int current_song_id_ = 0;
    int current_quota_ = 0;
    bool song_name_displayed_;
    
    // 歌词相关
    std::string current_lyric_url_;
    std::vector<std::pair<int, std::string>> lyrics_;  // 时间戳和歌词文本
    std::mutex lyrics_mutex_;  // 保护lyrics_数组的互斥锁
    int current_lyric_index_ = -1;
    
    TaskHandle_t download_task_handle_ = nullptr;
    TaskHandle_t play_task_handle_ = nullptr;
    TaskHandle_t lyric_update_task_handle_ = nullptr;
    std::atomic<bool> download_task_finished_{false};
    std::atomic<bool> play_task_finished_{false};
    std::atomic<bool> lyric_task_finished_{false};

    std::string streaming_url_;
    std::atomic<bool> is_playing_{false};
    std::atomic<bool> is_downloading_{false};
    std::atomic<bool> force_stop_{false};
    uint32_t current_play_time_ms_;  // 当前播放时间(毫秒)
    int total_frames_decoded_;      // 已解码的帧数
    size_t total_downloaded_ = 0; // 已下载的字节数

    // 音频缓冲区
    std::queue<AudioChunk> audio_buffer_;
    std::mutex buffer_mutex_;
    std::condition_variable buffer_cv_;
    size_t buffer_size_ = 0;
    static constexpr size_t MAX_BUFFER_SIZE = 64 * 1024;  // 256KB缓冲区（降低以减少brownout风险）
    static constexpr size_t MIN_BUFFER_SIZE = 16 * 1024;   // 32KB最小播放缓冲（降低以减少brownout风险）
    static constexpr size_t AUDIO_BUFFER_SIZE = 8192;
    // MP3解码器相关
    HMP3Decoder mp3_decoder_;
    MP3FrameInfo mp3_frame_info_;
    bool mp3_decoder_initialized_ = false;
    
    // 音频流相关
    void DownloadAudioStream(const std::string& music_url);
    void PlayAudioStream();
    void ClearAudioBuffer();
    bool InitializeMp3Decoder();
    void CleanupMp3Decoder();
    std::vector<int16_t> ResampleAudio(const int16_t* input, int input_samples, 
                                       int input_rate, int output_rate); // 音频重采样函数
    
    // 歌词相关私有方法
    bool DownloadLyrics(const std::string& lyric_url);
    bool ParseLyrics(const std::string& lyric_content);
    void UpdateLyricDisplay(uint32_t current_time_ms);
    void LyricUpdateTask();
    
    // ID3标签处理
    size_t SkipId3Tag(uint8_t* data, size_t size);

    // 播放历史记录
    void SendPlayHistory(int song_id);

    // 歌单循环播放相关
    std::vector<std::map<std::string, std::string>> music_collection_;  // 存储歌单
    int current_song_index_ = 0;  // 当前播放歌曲索引
    std::atomic<bool> is_loop_playing_{false};  // 是否正在循环播放
    TaskHandle_t loop_play_task_handle_ = nullptr;  // 循环播放任务句柄
    void LoopPlayTask();

    // 固定大小的单声道转换缓冲区
    static constexpr size_t MAX_MONO_SAMPLES = 1152;
    int16_t mono_buffer_[MAX_MONO_SAMPLES];

public:
    Esp32Music();
    ~Esp32Music();

    std::string SinglePlay(const std::string& song_name, const std::string& artist_name);
  
    // 流式播放相关方法
    bool StartStreaming(const std::string& music_url);
    bool StopStreaming();  // 停止流式播放
    size_t GetBufferSize() const { return buffer_size_; }
    
    std::string LoopPlay(const std::string& mode = "loop");  // 循环播放歌单
    bool StopLoopPlay();  // 停止循环播放
    void ResetLoop();  // 重置歌单
    std::string PlayNextSong(); // 播放下一首歌曲
};

#endif
