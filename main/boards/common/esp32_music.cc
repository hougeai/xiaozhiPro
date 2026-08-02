#include "esp32_music.h"
#include "board.h"
#include "audio/audio_codec.h"
#include "application.h"
#include "protocols/protocol.h"
#include "display/display.h"
#include "system_info.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_pthread.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <cstring>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cctype>  // 为isdigit函数
#include <memory>
#include <utility>
#include <thread>   // 为线程ID比较
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Esp32Music"


// URL编码函数
static std::string url_encode(const std::string& str) {
    std::string encoded;
    char hex[4];
    
    for (size_t i = 0; i < str.length(); i++) {
        unsigned char c = str[i];
        
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += '+';  // 空格编码为'+'或'%20'
        } else {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}

Esp32Music::Esp32Music() {
    ESP_LOGI(TAG, "Music player initialized");
    InitializeMp3Decoder();
}

Esp32Music::~Esp32Music() {
    ESP_LOGI(TAG, "Destroying music player - stopping all operations");
    std::lock_guard<std::recursive_mutex> lock(streaming_mutex_);
    StopLoopPlay();
    // 等待循环播放任务结束（带超时）
    if (loop_play_task_handle_ != nullptr) {
        int timeout_count = 0;
        const int MAX_TIMEOUT = 30; // 3秒超时
        while (eTaskGetState(loop_play_task_handle_) != eDeleted && timeout_count < MAX_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(100));
            timeout_count++;
        }
        if (timeout_count >= MAX_TIMEOUT) {
            ESP_LOGW(TAG, "Loop play task cleanup timeout, force deleting");
            vTaskDelete(loop_play_task_handle_);
        }
    }
    StopStreaming();
    vTaskDelay(pdMS_TO_TICKS(100)); // 等待一小段时间确保所有任务都完全停止
    CleanupMp3Decoder();
    ESP_LOGI(TAG, "Music player destroyed successfully");
}

std::string Esp32Music::SinglePlay(const std::string& song_name, const std::string& artist_name) {
    ESP_LOGI(TAG, "Starting to get music details for: %s", song_name.c_str());
    
    // 保存歌名用于后续显示
    current_song_name_ = song_name;
    
    // 第一步：请求stream_pcm接口获取音频信息
    // std::string base_url = "http://www.xiaozhishop.xyz:5005/stream_pcm";
    std::string base_url = std::string(CONFIG_SERVER_URL) + "/device_music";
    std::string full_url = base_url + "?song=" + url_encode(song_name) + "&singer=" + url_encode(artist_name);
    
    // ESP_LOGI(TAG, "Request URL: %s", full_url.c_str());
    
    // 使用Board提供的HTTP客户端
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    
    // 设置基本请求头
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());
    http->SetTimeout(10000); // 设置超时时间
    // 打开GET连接
    if (!http->Open("GET", full_url)) {
        ESP_LOGE(TAG, "Failed to connect to music API");
        return "连接音乐服务器失败";
    }
    
    // 检查响应状态码
    int status_code = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
        cJSON* root = cJSON_Parse(response.c_str());
        if (root) {
            cJSON* msg = cJSON_GetObjectItem(root, "msg");
            std::string error_msg = msg ? msg->valuestring : "Unknown error";
            cJSON_Delete(root);
            return error_msg;  // 返回具体错误信息
        }
        return "音乐服务器响应失败";
    }
    // ESP_LOGI(TAG, "Music API GET Status = %d, content_length = %d", status_code, response.length());
    // ESP_LOGI(TAG, "Music API raw response: %s", response.c_str());  // 调试用：生产环境避免打印完整响应体
    if (!response.empty()) {
        // 解析响应JSON以提取音频URL
        cJSON* response_json = cJSON_Parse(response.c_str());
        if (response_json) {
            // 首先检查响应是否包含data字段
            cJSON* data_obj = cJSON_GetObjectItem(response_json, "data");
            // 检查data对象是否存在
            if (!data_obj) {
                ESP_LOGE(TAG, "Invalid response: missing data field");
                cJSON_Delete(response_json);
                return "音乐服务器返回数据无效";
            }
            // 提取关键信息
            cJSON* song_id = cJSON_GetObjectItem(data_obj, "song_id"); 
            cJSON* singer = cJSON_GetObjectItem(data_obj, "singer");
            cJSON* song = cJSON_GetObjectItem(data_obj, "song");
            cJSON* audio_url = cJSON_GetObjectItem(data_obj, "audio_url");
            cJSON* lyric_url = cJSON_GetObjectItem(data_obj, "lyric_url");
            // 安全打印日志
            if (singer && song && song_id) {
                ESP_LOGI(TAG, "Singer: %s Song: %s song_id: %d", 
                    singer->valuestring ? singer->valuestring : "unknown",
                    song->valuestring ? song->valuestring : "unknown",
                    song_id->valueint);
            }
            
            // 检查audio_url是否有效
            if (cJSON_IsString(audio_url) && audio_url->valuestring && strlen(audio_url->valuestring) > 0) {
                // ESP_LOGI(TAG, "Resolved audio_url: %s", audio_url->valuestring);
                current_music_url_ = audio_url->valuestring;
                current_song_id_ = song_id ? song_id->valueint : 0; 
                song_name_displayed_ = false;  // 重置歌名显示标志
                if (!StartStreaming(current_music_url_)) {
                    ESP_LOGE(TAG, "Failed to start streaming playback");
                    return "音乐连接流式播放失败";
                }
                // 处理歌词URL（异步下载，不阻塞起播）
                if (cJSON_IsString(lyric_url) && lyric_url->valuestring && strlen(lyric_url->valuestring) > 0) {
                    current_lyric_url_ = lyric_url->valuestring;
                    ESP_LOGI(TAG, "Loading lyrics for: %s", song_name.c_str());
                    xTaskCreate(
                        [](void* param) {
                            auto* args = static_cast<std::pair<Esp32Music*, std::string>*>(param);
                            Esp32Music* self = args->first;
                            const std::string& url = args->second;
                            // 方案A：错峰下载歌词，避免与音频 HTTPS 握手并发抢占 CPU/WiFi。
                            // 等音频下载达到安全水位（256KB）或已开始播放，再发起歌词请求；
                            // 设置最大等待上限，确保弱网下歌词仍会出现。
                            constexpr size_t kLyricSafeThreshold = 256 * 1024;
                            constexpr uint32_t kLyricMaxWaitMs = 8000;
                            uint32_t waited = 0;
                            while (waited < kLyricMaxWaitMs) {
                                if (!self->is_playing_.load() && !self->is_downloading_.load()) {
                                    break;  // 播放已停止，无需再下
                                }
                                if (self->total_downloaded_ >= kLyricSafeThreshold) {
                                    break;  // 音频已稳定，可以错峰发起歌词连接
                                }
                                vTaskDelay(pdMS_TO_TICKS(200));
                                waited += 200;
                            }
                            if (self->is_playing_.load() || self->is_downloading_.load()) {
                                ESP_LOGI(TAG, "Lyric download deferred %u ms, starting now (downloaded=%u)",
                                         waited, (unsigned)self->total_downloaded_);
                                self->DownloadLyrics(url);
                            }
                            delete args;
                            vTaskDelete(nullptr);
                        },
                        "lyric_dl", 4096,
                        new std::pair<Esp32Music*, std::string>(this, current_lyric_url_), 4, nullptr);
                } else {
                    ESP_LOGW(TAG, "No lyric URL found for this song");
                }
                cJSON_Delete(response_json);
                return "";
            } else {
                // audio_url为空或无效
                ESP_LOGE(TAG, "Failed to find music: 没有找到歌曲 '%s'", song_name.c_str());
                cJSON_Delete(response_json);
                return "未找到匹配的歌曲";
            }
        } else {
            ESP_LOGE(TAG, "Failed to parse JSON response");
        }
    } else {
        ESP_LOGE(TAG, "Empty response from music API");
    }
    return "音乐服务器无响应";
}

// 开始流式播放
bool Esp32Music::StartStreaming(const std::string& music_url) {
    // 加锁保护整段临界区，避免并发调用 StartStreaming/StopStreaming 时状态交错
    std::lock_guard<std::recursive_mutex> lock(streaming_mutex_);

    // 停止之前的播放和下载（StopStreaming 内部已等待旧任务删除，无需额外持锁 sleep）
    StopStreaming();

    force_stop_ = false;
    streaming_url_ = music_url;
    current_lyric_index_ = -1;
    total_downloaded_ = 0;  // 重置已下载字节数
    // 重置任务完成标志
    download_task_finished_ = false;
    play_task_finished_ = false;
    lyric_task_finished_ = false;

    // 关键：在创建任务前立即置位，确保任务一旦被调度运行就能通过
    // DownloadAudioStream/PlayAudioStream 的 while 条件（依赖 is_downloading_ && is_playing_），
    // 不会被任何 StopStreaming 的清理（置 false）抢先导致任务秒退。
    is_downloading_ = true;
    is_playing_ = true;

    // 音乐播放为持续高带宽场景：将 WiFi/整机切到高性能模式，避免进入 modem-sleep
    // 后收包不连续导致音频缓冲被抽干、播放卡顿。播放停止时由 StopStreaming 恢复。
    {
        auto& board = Board::GetInstance();
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        ESP_LOGI(TAG, "Music streaming: set power save level = PERFORMANCE");
    }

    // 创建下载和播放任务
    BaseType_t download_task_ret = xTaskCreate([](void* arg) {
        Esp32Music* music = static_cast<Esp32Music*>(arg);
        music->DownloadAudioStream(music->streaming_url_);
        music->download_task_finished_ = true;
        vTaskDelete(NULL);
    }, "MusicDownload", 8192, this, 5, &download_task_handle_);
    
    if (download_task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create download task");
        StopStreaming();  // 统一复位所有标志并清理（此时尚未创建其它任务）
        return false;
    }

    BaseType_t play_task_ret = xTaskCreate([](void* arg) {
        Esp32Music* music = static_cast<Esp32Music*>(arg);
        music->PlayAudioStream();
        music->play_task_finished_ = true; // 标记任务完成
        vTaskDelete(NULL);
    }, "MusicPlay", 8192, this, 5, &play_task_handle_);

    if (play_task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create play task");
        StopStreaming();  // 清理已创建的 download 任务
        return false;
    }

    BaseType_t lyric_task_ret = xTaskCreate([](void* arg) {
        Esp32Music* music = static_cast<Esp32Music*>(arg);
        music->LyricUpdateTask();
        music->lyric_task_finished_ = true;
        vTaskDelete(NULL);
    }, "LyricUpdate", 2048, this, 5, &lyric_update_task_handle_);

    if (lyric_task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create lyric update task");
        StopStreaming();  // 清理已创建的 download/play 任务
        return false;
    }

    // ESP_LOGI(TAG, "Streaming tasks started from: %s", music_url.c_str());
    ESP_LOGI(TAG, "Streaming tasks started");
    return true;
}

// 停止流式播放
bool Esp32Music::StopStreaming() {
    // 加锁保护临界区（recursive_mutex，允许 StartStreaming 持锁内重入调用）
    std::lock_guard<std::recursive_mutex> lock(streaming_mutex_);
    ESP_LOGI(TAG, "Stopping music streaming");
    // 1. 先发出停止信号
    is_downloading_ = false;
    is_playing_ = false;
    force_stop_ = true;

    // 2. 通知等待的线程
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    // 等待任务结束
    if (download_task_finished_) {
        ESP_LOGI(TAG, "Download task is already finished");
    } else if (download_task_handle_ == nullptr) {
        ESP_LOGI(TAG, "No previous download task to stop (first start or already cleaned up)");
    } else if (download_task_handle_ != nullptr) {
        ESP_LOGI(TAG, "Waiting for music download task to finish");
        while (eTaskGetState(download_task_handle_) != eDeleted) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    download_task_handle_ = nullptr;
    ESP_LOGI(TAG, "Audio download task finished");
    if (play_task_finished_) {
        ESP_LOGI(TAG, "Play task is already finished");
    } else if (play_task_handle_ != nullptr) {
        ESP_LOGI(TAG, "Waiting for play task to finish");
        while (eTaskGetState(play_task_handle_) != eDeleted) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    play_task_handle_ = nullptr;
    ESP_LOGI(TAG, "Audio playing task finished");

    if (lyric_task_finished_) {
        ESP_LOGI(TAG, "Lyric task is already finished");
    } else if (lyric_update_task_handle_ != nullptr) {
        ESP_LOGI(TAG, "Waiting for lyric task to finish");
        while (eTaskGetState(lyric_update_task_handle_) != eDeleted) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    lyric_update_task_handle_ = nullptr;
    ESP_LOGI(TAG, "Lyric update task finished");
    // 清理缓冲区
    ClearAudioBuffer();
    // 播放结束，恢复低功耗模式，让系统空闲时正常进入 modem-sleep 省电
    {
        auto& board = Board::GetInstance();
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        ESP_LOGI(TAG, "Music streaming stopped, restored power save level = LOW_POWER");
    }
    ESP_LOGI(TAG, "Music streaming stopped");
    return true;
}

// mbedTLS 连接层硬错误码：连接一旦失败（如握手阶段被 RST），重试也不会成功，
// 应快速放弃而不是空转 20 次。
// -0x0050 = MBEDTLS_ERR_NET_CONN_RESET（对端/中间设备直接 RST，典型如 OSS+WAF 按出口 IP 拦截）
// -0x004C = MBEDTLS_ERR_NET_RECV_FAILED（握手未完成对端即关闭）
// 0x801A  = ESP_ERR_ESP_TLS_CANNOT_CONNECT（TLS 整体建立失败）
static constexpr int kHardFailConnReset   = -0x0050;
static constexpr int kHardFailRecvFailed  = -0x004C;
static constexpr int kHardFailTlsConnect  = 0x801A;

// 判断是否为连接层硬失败（无需重试）
static bool IsConnectionHardFail(int err) {
    return err == kHardFailConnReset ||
           err == kHardFailRecvFailed ||
           err == kHardFailTlsConnect;
}

// 流式下载音频数据
void Esp32Music::DownloadAudioStream(const std::string& music_url) {
    auto network = Board::GetInstance().GetNetwork();
    // 添加重试机制
    const int max_retries = 20;
    int retry_count = 0;
    bool download_success = false;
    int  content_length   = -1;          // 服务器给出的总长度
    int  resume_from      = total_downloaded_; // 下次续传起点
    while (retry_count < max_retries && !download_success && is_downloading_ && is_playing_) {
        ESP_LOGI(TAG, "DownloadAudioStream target URL: %s", music_url.c_str());
        auto http = network->CreateHttp(0);
        // 设置浏览器风格请求头，规避部分 OSS/WAF 对非浏览器 UA 的拦截
        http->SetHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
        http->SetHeader("Accept", "*/*");
        http->SetHeader("Accept-Language", "zh-CN,zh;q=0.9");
        http->SetHeader("Connection", "close");
        http->SetTimeout(90000); // 设置超时时间为20秒
        // 支持断点续传
        std::string range_hdr = "bytes=" + std::to_string(resume_from) + "-";
        http->SetHeader("Range", range_hdr);
        ESP_LOGI(TAG, "Retry %d/%d  Range: %s", retry_count + 1, max_retries, range_hdr.c_str());
        if (!http->Open("GET", music_url)) {
            int last_err = http->GetLastError();
            ESP_LOGE(TAG, "Failed to connect to music stream URL (attempt %d/%d), last_err=0x%04X",
                     retry_count + 1, max_retries, (unsigned)last_err & 0xFFFF);
            http->Close();
            // 连接层硬失败（如 TLS 握手被 RST）：重试必失败，直接放弃
            if (IsConnectionHardFail(last_err)) {
                ESP_LOGE(TAG, "Connection-layer hard failure (0x%04X), giving up immediately",
                         (unsigned)last_err & 0xFFFF);
                download_success = false;
                break;
            }
            retry_count++;
            if (retry_count < max_retries) {
                vTaskDelay(pdMS_TO_TICKS(1000 * retry_count)); // 递增延迟重试
            }
            continue;
        }
        int status_code = http->GetStatusCode();
        if (status_code != 200 && status_code != 206) {  // 206 for partial content
            ESP_LOGE(TAG, "HTTP GET failed with status code: %d (attempt %d/%d)", status_code, retry_count + 1, max_retries);
            http->Close();
            // 4xx 属于客户端/资源层面的永久错误（如 403 未备案拦截、404 不存在），
            // 重试必然失败，直接放弃；仅 5xx 服务器临时错误才值得重试。
            if (status_code >= 400 && status_code < 500) {
                ESP_LOGE(TAG, "HTTP %d is a permanent client/resource error, giving up immediately", status_code);
                download_success = false;
                break;
            }
            retry_count++;
            if (retry_count < max_retries) {
                vTaskDelay(pdMS_TO_TICKS(1000 * retry_count)); // 递增延迟重试
            }
            continue;
        }
        if (retry_count == 0) {
            content_length = http->GetBodyLength();
            ESP_LOGI(TAG, "Started downloading audio stream, status: %d content-length: %d", status_code, content_length);
        } else {
            ESP_LOGI(TAG, "Resumed downloading audio stream, status: %d", status_code);
        }
        // 分块读取音频数据
        const size_t chunk_size = 4096;  // 4KB每块
        char buffer[chunk_size]; // 循环使用的临时缓冲区
        // 只有当下载和播放都处于活动状态时才继续，如果用户停止播放||下载错误，都停止下载
        while (is_downloading_ && is_playing_) {
            int bytes_read = http->Read(buffer, chunk_size);
            if (bytes_read < 0) {
                // 读取出错（连接被对端关闭/超时），视为异常断开，尝试断点续传
                resume_from = total_downloaded_;
                ESP_LOGW(TAG, "Read error (bytes_read=%d), will retry from %d/%d",
                        bytes_read, total_downloaded_, content_length);
                break;
            } else if (bytes_read == 0) {
                // 读到 0 字节通常意味着服务器已发送完整个文件（正常 EOF）。
                // 不依赖 content_length 精确匹配，避免 OSS 不返回/算错长度时
                // 把"正常结束"误判为"中断"而反复 Range 续传、重复播放。
                ESP_LOGI(TAG, "Download complete (EOF): %d bytes", total_downloaded_);
                download_success = true;
                break;
            }
            // 只在第一次读取时检测文件类型
            if (total_downloaded_ == 0 && bytes_read >= 4) {
                if (memcmp(buffer, "ID3", 3) == 0) {
                    ESP_LOGI(TAG, "Detected MP3 file with ID3 tag");
                    detected_format_ = FORMAT_MP3; // 含有id3头部，后面需要skip
                } else if (buffer[0] == 0xFF && (buffer[1] & 0xE0) == 0xE0) {
                    ESP_LOGI(TAG, "Detected MP3 file header");
                    detected_format_ = FORMAT_MP3; // 纯MP3数据，后面不需要skip
                } else if (memcmp(buffer, "RIFF", 4) == 0) {
                    ESP_LOGI(TAG, "Detected WAV file");
                    detected_format_ = FORMAT_WAV;
                } else if (memcmp(buffer, "fLaC", 4) == 0) {
                    ESP_LOGI(TAG, "Detected FLAC file");
                    detected_format_ = FORMAT_FLAC;
                } else if (memcmp(buffer, "OggS", 4) == 0) {
                    ESP_LOGI(TAG, "Detected OGG file");
                    detected_format_ = FORMAT_OGG;
                } else {
                    ESP_LOGI(TAG, "Unknown audio format, first 4 bytes: %02X %02X %02X %02X", 
                            (unsigned char)buffer[0], (unsigned char)buffer[1], 
                            (unsigned char)buffer[2], (unsigned char)buffer[3]);
                }
                if (detected_format_ != FORMAT_MP3) {
                    is_downloading_ = false;  // 停止下载
                    break;
                }
            }
            
            // 为每个数据块分配独立的内存
            uint8_t* chunk_data = (uint8_t*)heap_caps_malloc(bytes_read, MALLOC_CAP_SPIRAM);
            if (!chunk_data) {
                ESP_LOGE(TAG, "Failed to allocate memory for audio chunk");
                break;
            }
            memcpy(chunk_data, buffer, bytes_read); // 复制到独立内存
            
            // 等待缓冲区有空间
            {
                std::unique_lock<std::mutex> lock(buffer_mutex_); // 获取互斥锁，lock_guard只能加锁解锁不能中途释放，unique_lock可以随时解锁
                // buffer_cv_.wait(lock, 条件函数); 如果条件为假，会阻塞等待直到其它地方notify重新检查条件，条件为真，立即执行；is_downloading_为false时，wait也会立即返回 不会阻塞
                buffer_cv_.wait(lock, [this] { return buffer_size_ < MAX_BUFFER_SIZE || !is_downloading_; });
                
                if (is_downloading_) {
                    audio_buffer_.push(AudioChunk(chunk_data, bytes_read, true)); // 放入缓冲区
                    buffer_size_ += bytes_read;
                    total_downloaded_ += bytes_read;
                    buffer_cv_.notify_one(); // 只唤醒一个线程，all 是唤醒所有线程；假设缓冲区之前已满，只有下载线程在等待，假设缓冲区之前为空，只有播放线程在等待
                    if (total_downloaded_ % (128 * 1024) == 0) {  // 每256KB打印一次进度
                        ESP_LOGI(TAG, "Downloaded %d bytes, buffer size: %d", total_downloaded_, buffer_size_);
                    }
                } else {
                    heap_caps_free(chunk_data);
                    break;
                }
            }
        }
        http->Close();
        // 如果下载未成功且还需要重试
        if (!download_success && is_downloading_ && is_playing_ && retry_count < max_retries - 1) {
            ESP_LOGW(TAG, "Download interrupted, retrying in %d seconds...", retry_count + 1);
            // vTaskDelay(pdMS_TO_TICKS(1000 * (retry_count + 1)));
            vTaskDelay(pdMS_TO_TICKS(500));
        } 
        retry_count++;
    }

    is_downloading_ = false;
    
    // 通知播放线程下载完成
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    if (download_success) {
        ESP_LOGI(TAG, "Audio stream download finished successfully");
    } else {
        ESP_LOGE(TAG, "Audio stream download failed after %d attempts", max_retries);
    }
}

// 流式播放音频数据
void Esp32Music::PlayAudioStream() {
    ESP_LOGI(TAG, "Starting audio stream playback");
    
    // 初始化时间跟踪变量
    current_play_time_ms_ = 0;
    total_frames_decoded_ = 0;
    
    // 等待缓冲区有足够数据 or 下载完成且缓冲区不为空，开始执行下面流程
    {
        std::unique_lock<std::mutex> lock(buffer_mutex_);
        buffer_cv_.wait(lock, [this] { 
            return buffer_size_ >= MIN_BUFFER_SIZE || (!is_downloading_ && !audio_buffer_.empty()); 
        });
    }
    
    ESP_LOGI(TAG, "Starting playback with buffer size: %d", buffer_size_);
    
    size_t total_played = 0;
    uint8_t* mp3_input_buffer = nullptr;
    uint8_t* read_ptr = nullptr; // 指针，标记读到哪里了，用于消费数据
    int bytes_left = 0; // 剩余字节数，从 read_ptr 开始还有多少字节可读

    // 分配一块AUDIO_BUFFER_SIZE字节的内存，从左到右填充和消费
    mp3_input_buffer = (uint8_t*)heap_caps_malloc(AUDIO_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        if (!mp3_input_buffer) {
            ESP_LOGE(TAG, "Failed to allocate MP3 input buffer");
            is_playing_ = false;
            return;
        }
    
    // 标记是否已经处理过ID3标签
    bool id3_processed = false;

    // 获取目标采样率
    auto codec = Board::GetInstance().GetAudioCodec();
    int target_sample_rate = codec->output_sample_rate();
    auto& app = Application::GetInstance();
    // 设置音乐播放状态为true
    app.music_playing_ = true;
    // 断开与服务端的连接，确保音频通道关闭
    app.Schedule([&app]() {
        Protocol* protocol = app.GetProtocol();
        if (protocol && protocol->IsAudioChannelOpened()) {
            protocol->CloseAudioChannel();
        }
        ESP_LOGI(TAG, "Playing Music, closing audio channel");
    });
    
    vTaskDelay(pdMS_TO_TICKS(300));

    while (true) {
        // 先检查播放状态
        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            if (!is_playing_) {
                ESP_LOGI(TAG, "Playback stopped by StopStreaming");
                break;
            }
        }
        // 设备状态检查通过，显示当前播放的歌名
        if (!song_name_displayed_ && !current_song_name_.empty()) {
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();
            if (display) {
                std::string formatted_song_name = "《" + current_song_name_ + "》播放中...";
                display->SetStatus(formatted_song_name.c_str());
                ESP_LOGI(TAG, "Displaying song name: %s", formatted_song_name.c_str());
                song_name_displayed_ = true;
            }
        }
        
        if (bytes_left < 4096) {  // 保持至少4KB数据用于解码
            AudioChunk chunk;
            {
                std::unique_lock<std::mutex> lock(buffer_mutex_);
                if (audio_buffer_.empty()) {
                    if (!is_downloading_) {
                        ESP_LOGI(TAG, "Playback finished, total played: %d bytes", total_played);
                        break; // 下载完成且缓冲区为空，播放结束
                    }
                    // 等待下载新数据
                    buffer_cv_.wait(lock, [this] { return !audio_buffer_.empty() || !is_downloading_; });
                }
                if (!is_playing_) {
                    ESP_LOGI(TAG, "Playback stopped while waiting for buffer");
                    break;
                }
                if (audio_buffer_.empty() && !is_downloading_) {
                    break; // 缓冲区为空且下载完成，播放结束
                } 
                chunk = std::move(audio_buffer_.front());
                audio_buffer_.pop(); // 只移除，不返回元素值
                buffer_size_ -= chunk.size;                
                buffer_cv_.notify_one(); // 通知下载线程缓冲区有空间
            }
            
            // 把上次解码后剩余的数据移动到缓冲区开头
            if (bytes_left > 0 && read_ptr != mp3_input_buffer) {
                memmove(mp3_input_buffer, read_ptr, bytes_left);
            }
            
            // 检查mp3缓冲区可用空间
            size_t space_available = AUDIO_BUFFER_SIZE - bytes_left; //
            size_t copy_size = std::min(chunk.size, space_available);
            // 剩余数据后面追加新的chunk数据
            if (chunk.data != nullptr && chunk.size > 0 && chunk.owned) {
                memcpy(mp3_input_buffer + bytes_left, chunk.data, copy_size);
                bytes_left += copy_size;
                read_ptr = mp3_input_buffer;
            }
            // 检查并跳过ID3标签（仅在开始时处理一次）
            if (!id3_processed && bytes_left >= 10) {
                size_t id3_skip = SkipId3Tag(read_ptr, bytes_left);
                if (id3_skip > 0) {
                    read_ptr += id3_skip;
                    bytes_left -= id3_skip;
                    ESP_LOGI(TAG, "Skipped ID3 tag: %u bytes", (unsigned int)id3_skip);
                }
                id3_processed = true;
            }
            if (chunk.owned && chunk.data) {
                heap_caps_free(chunk.data);
                chunk.owned = false;
            }
            chunk.data = nullptr;
        }
        
        // 寻找下一个有效的MP3帧开始位置：MP3文件由一个个"帧"组成，每个帧都以特定的"同步字"开头（通常是0xFF）
        int sync_offset = MP3FindSyncWord(read_ptr, bytes_left);
        if (sync_offset < 0) {
            ESP_LOGW(TAG, "No MP3 sync word found, skipping %d bytes", bytes_left);
            bytes_left = 0;
            continue;
        } else {
            read_ptr += sync_offset;
            bytes_left -= sync_offset;
        }
        
        // 解码MP3帧
        int16_t pcm_buffer[2304]; // MP3编码有固定的帧结构：单声道每帧=1152个样本，立体声1152 × 2 = 2304
        int decode_result = MP3Decode(mp3_decoder_, &read_ptr, &bytes_left, pcm_buffer, 0);
        // MP3Decode会自动更新read_ptr和bytes_left，read_ptr会指向下一帧的开始，bytes_left会减去已消费的字节数
        if (decode_result == 0) {
            // 解码成功，获取帧信息
            MP3GetLastFrameInfo(mp3_decoder_, &mp3_frame_info_);
            total_frames_decoded_++;
            
            // 基本的帧信息有效性检查，防止除零错误
            if (mp3_frame_info_.samprate == 0 || mp3_frame_info_.nChans == 0) {
                ESP_LOGW(TAG, "Invalid frame info: rate=%d, channels=%d, skipping", 
                        mp3_frame_info_.samprate, mp3_frame_info_.nChans);
                continue;
            }
            
            // 计算当前帧的持续时间(毫秒)
            int frame_duration_ms = (mp3_frame_info_.outputSamps * 1000) / 
                                  (mp3_frame_info_.samprate * mp3_frame_info_.nChans);
            // 更新当前播放时间
            current_play_time_ms_ += frame_duration_ms;

            // 将PCM数据发送到Application的音频解码队列
            if (mp3_frame_info_.outputSamps > 0) {
                int16_t* final_pcm_data = pcm_buffer;
                int final_sample_count = mp3_frame_info_.outputSamps;

                // 如果是双通道，转换为单通道混合
                if (mp3_frame_info_.nChans == 2) {
                    // 双通道转单通道：将左右声道混合
                    int stereo_samples = mp3_frame_info_.outputSamps;  // 包含左右声道的总样本数
                    int mono_samples = stereo_samples / 2;  // 单声道样本数                    
                    // 安全检查
                    if (mono_samples > MAX_MONO_SAMPLES) {
                        ESP_LOGE(TAG, "Mono samples overflow: %d > %d", mono_samples, MAX_MONO_SAMPLES);
                        continue;
                    }
                    for (int i = 0; i < mono_samples; ++i) {
                        // 混合左右声道 (L + R) / 2
                        int left = pcm_buffer[i * 2];      // 左声道
                        int right = pcm_buffer[i * 2 + 1]; // 右声道
                        mono_buffer_[i] = (int16_t)((left + right) / 2); // 混合
                    }
                    final_pcm_data = mono_buffer_;
                    final_sample_count = mono_samples;
                }
                // 等待队列消化
                while (app.GetAudioService().GetPlayQueueLength() >= 200) {
                    // ESP_LOGW(TAG, "Audio queue (%d frames), delaying...", app.GetAudioService().GetPlayQueueLength());
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
                // 如果需要，将采样率转换为指定的目标采样率
                std::vector<int16_t> resampled_pcm = ResampleAudio(
                    final_pcm_data, 
                    final_sample_count, 
                    mp3_frame_info_.samprate, 
                    target_sample_rate
                );
                
                // 比如final_sample_count=1152个样本，sizeof(int16_t)：每个样本占2字节；
                size_t pcm_size_bytes = resampled_pcm.size() * sizeof(int16_t);        
                // 发送到Application的音频解码队列
                app.GetAudioService().PlayExternalPcmData(std::move(resampled_pcm), current_play_time_ms_);
                total_played += pcm_size_bytes;
                // 打印播放进度
                if (total_played % (512 * 1024) == 0) {
                    ESP_LOGI(TAG, "Played %d bytes, buffer size: %d", total_played, buffer_size_);
                }
                vTaskDelay(1);   // 给调度器一点时间，避免 watchdog, 1 tick (~1ms)
            }
        } else {
            // 解码失败：MP3Decode 在无法解码时不会推进 read_ptr/bytes_left。
            // 这里主动寻找下一个同步字重新对齐，而不是盲目 +1 跳过导致持续乱码。
            ESP_LOGW(TAG, "MP3 decode failed with error: %d, re-syncing", decode_result);
            int sync = MP3FindSyncWord(read_ptr, bytes_left);
            if (sync > 0) {
                read_ptr += sync;
                bytes_left -= sync;
            } else {
                // 当前缓冲区内已无同步字，丢弃剩余字节等待新数据
                bytes_left = 0;
            }
        }
    }
    
    // 清理
    if (mp3_input_buffer) {
        heap_caps_free(mp3_input_buffer);
        mp3_input_buffer = nullptr;
    }
    
    // 播放结束时进行基本清理，但不调用StopStreaming避免线程自我等待
    ESP_LOGI(TAG, "Audio stream playback finished, total played: %d bytes", total_played);
    // 停止播放标志
    is_playing_ = false;
    if (current_song_id_ > 0) {
        ESP_LOGI(TAG, "Audio stream playback finished, sending currend_song_id %d", current_song_id_);
        SendPlayHistory(current_song_id_);
        current_song_id_ = 0;
    }
    app.music_playing_ = false;
    app.FlushConversationQueue();
    ESP_LOGI(TAG, "Music playback finished, set DeviceStateIdle");
}

// 清空音频缓冲区
void Esp32Music::ClearAudioBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    while (!audio_buffer_.empty()) {
        AudioChunk chunk = audio_buffer_.front();
        audio_buffer_.pop();
        if (chunk.owned && chunk.data) {
            heap_caps_free(chunk.data);
        }
    }
    
    buffer_size_ = 0;
    ESP_LOGI(TAG, "Audio buffer cleared");
}

// 初始化MP3解码器
bool Esp32Music::InitializeMp3Decoder() {
    mp3_decoder_ = MP3InitDecoder();
    if (mp3_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
        mp3_decoder_initialized_ = false;
        return false;
    }
    
    mp3_decoder_initialized_ = true;
    ESP_LOGI(TAG, "MP3 decoder initialized successfully");
    return true;
}

// 清理MP3解码器
void Esp32Music::CleanupMp3Decoder() {
    if (mp3_decoder_ != nullptr) {
        MP3FreeDecoder(mp3_decoder_);
        mp3_decoder_ = nullptr;
    }
    mp3_decoder_initialized_ = false;
    ESP_LOGI(TAG, "MP3 decoder cleaned up");
}


// 跳过MP3文件开头的ID3标签
size_t Esp32Music::SkipId3Tag(uint8_t* data, size_t size) {
    if (!data || size < 10) {
        return 0;
    }
    
    // 检查ID3v2标签头 "ID3"
    if (memcmp(data, "ID3", 3) != 0) {
        return 0;
    }
    
    // 计算标签大小（synchsafe integer格式）
    uint32_t tag_size = ((uint32_t)(data[6] & 0x7F) << 21) |
                        ((uint32_t)(data[7] & 0x7F) << 14) |
                        ((uint32_t)(data[8] & 0x7F) << 7)  |
                        ((uint32_t)(data[9] & 0x7F));
    
    // ID3v2头部(10字节) + 标签内容
    size_t total_skip = 10 + tag_size;
    
    // 确保不超过可用数据大小
    if (total_skip > size) {
        total_skip = size;
    }
    
    ESP_LOGI(TAG, "Found ID3v2 tag, skipping %u bytes", (unsigned int)total_skip);
    return total_skip;
}

// 下载歌词
bool Esp32Music::DownloadLyrics(const std::string& lyric_url) {
    // ESP_LOGI(TAG, "Downloading lyrics from: %s", lyric_url.c_str());
    
    if (lyric_url.empty()) {
        return false;
    }
    
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    if (!http) {
        ESP_LOGW(TAG, "Failed to create HTTP client for lyrics");
        return false;
    }
    
    http->SetHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    http->SetHeader("Accept", "text/plain, */*;q=0.8");
    http->SetHeader("Accept-Language", "zh-CN,zh;q=0.9");
    
    if (!http->Open("GET", lyric_url)) {
        ESP_LOGW(TAG, "Failed to connect to lyric URL");
        return false;
    }
    
    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGW(TAG, "Lyric download failed with status: %d", status_code);
        http->Close();
        return false;
    }
    
    std::string lyric_content = http->ReadAll();
    http->Close();
    
    if (lyric_content.empty()) {
        ESP_LOGW(TAG, "Downloaded lyrics are empty");
        return false;
    }
    
    ESP_LOGI(TAG, "Lyrics downloaded successfully, size: %d bytes", lyric_content.length());
    return ParseLyrics(lyric_content);
}

// 解析歌词
bool Esp32Music::ParseLyrics(const std::string& lyric_content) {
    ESP_LOGI(TAG, "Parsing lyrics content");
    
    // 使用锁保护lyrics_数组访问
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    lyrics_.clear();
    // 按行分割歌词内容
    std::istringstream stream(lyric_content);
    std::string line;
    
    while (std::getline(stream, line)) {
        // 去除行尾的回车符
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        
        // 跳过空行
        if (line.empty()) {
            continue;
        }
        
        // 解析LRC格式: [mm:ss.xx]歌词文本
        if (line.length() > 10 && line[0] == '[') {
            size_t close_bracket = line.find(']');
            if (close_bracket != std::string::npos) {
                std::string tag_or_time = line.substr(1, close_bracket - 1);
                std::string content = line.substr(close_bracket + 1);
                
                // 检查是否是元数据标签而不是时间戳
                // 元数据标签通常是 [ti:标题], [ar:艺术家], [al:专辑] 等
                size_t colon_pos = tag_or_time.find(':');
                if (colon_pos != std::string::npos) {
                    std::string left_part = tag_or_time.substr(0, colon_pos);
                    
                    // 检查冒号左边是否是时间（数字）
                    bool is_time_format = true;
                    for (char c : left_part) {
                        if (!isdigit(c)) {
                            is_time_format = false;
                            break;
                        }
                    }
                    
                    // 如果不是时间格式，跳过这一行（元数据标签）
                    if (!is_time_format) {
                        continue;
                    }
                    
                    // 是时间格式，解析时间戳
                    try {
                        int minutes = std::stoi(tag_or_time.substr(0, colon_pos));
                        float seconds = std::stof(tag_or_time.substr(colon_pos + 1));
                        int timestamp_ms = minutes * 60 * 1000 + (int)(seconds * 1000);
                        
                        // 安全处理歌词文本，确保UTF-8编码正确
                        std::string safe_lyric_text;
                        if (!content.empty()) {
                            // 创建安全副本并验证字符串
                            safe_lyric_text = content;
                            // 确保字符串以null结尾
                            safe_lyric_text.shrink_to_fit();
                        }
                        
                        lyrics_.push_back(std::make_pair(timestamp_ms, safe_lyric_text));
                        
                        if (!safe_lyric_text.empty()) {
                            // 限制日志输出长度，避免中文字符截断问题
                            size_t log_len = std::min(safe_lyric_text.length(), size_t(50));
                            std::string log_text = safe_lyric_text.substr(0, log_len);
                            ESP_LOGI(TAG, "Parsed lyric: [%d ms] %s", timestamp_ms, log_text.c_str());
                        }
                    } catch (const std::exception& e) {
                        ESP_LOGW(TAG, "Failed to parse time: %s", tag_or_time.c_str());
                    }
                }
            }
        }
    }
    
    // 按时间戳排序
    std::sort(lyrics_.begin(), lyrics_.end());
    
    ESP_LOGI(TAG, "Parsed %d lyric lines", lyrics_.size());
    return !lyrics_.empty();
}

// 更新歌词显示
void Esp32Music::UpdateLyricDisplay(uint32_t current_time_ms) {
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    
    if (lyrics_.empty()) {
        return;
    }
    
    // 查找当前应该显示的歌词
    int new_lyric_index = -1;
    // 从当前歌词索引开始查找，提高效率
    int start_index = (current_lyric_index_ >= 0) ? current_lyric_index_ : 0;
    
    // 正向查找：找到最后一个时间戳小于等于当前时间的歌词
    for (int i = start_index; i < (int)lyrics_.size(); i++) {
        if (lyrics_[i].first <= current_time_ms) {
            new_lyric_index = i;
        } else {
            break;  // 时间戳已超过当前时间
        }
    }
    
    // 如果歌词索引发生变化，更新显示
    if (new_lyric_index != current_lyric_index_) {
        current_lyric_index_ = new_lyric_index;
        ESP_LOGI(TAG, "[IN]  current_lyric_index_=%d  time=%lu", current_lyric_index_, current_time_ms);
        
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display) {
            std::string lyric_text = "";
            if (current_lyric_index_ >= 0 && current_lyric_index_ < (int)lyrics_.size()) {
                lyric_text = lyrics_[current_lyric_index_].second;
            }
            // 显示歌词
            display->SetChatMessage("system", lyric_text.c_str());
        }
    }
}

void Esp32Music::LyricUpdateTask() {
    auto& audio_service = Application::GetInstance().GetAudioService();
    ESP_LOGI(TAG, "Lyric update task started, %d %d", (int)is_playing_.load(), (int)audio_service.IsIdle());
    while (is_playing_ || !audio_service.IsIdle()) {
        if (force_stop_) {
            break;
        }
        uint32_t current_time_ms = audio_service.GetCurrentPlayTimestamp();
        UpdateLyricDisplay(current_time_ms);
        // 控制刷新频率（1000ms：降低 LVGL 重绘触发次数，缓解 SPI 屏 flush 压力）
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Audio stream Lyric update stopped");
}

// 音频重采样函数
std::vector<int16_t> Esp32Music::ResampleAudio(const int16_t* input, int input_samples, 
                                               int input_rate, int output_rate) {
    // 如果采样率相同，直接返回
    if (input_rate == output_rate) {
        return std::vector<int16_t>(input, input + input_samples);
    }
    
    // 计算重采样比例
    float ratio = static_cast<float>(output_rate) / input_rate;
    int output_samples = static_cast<int>(input_samples * ratio + 0.5f);
    
    std::vector<int16_t> output;
    output.reserve(output_samples);
    
    // 线性插值重采样
    for (int i = 0; i < output_samples; i++) {
        float src_index = i / ratio;
        int index1 = static_cast<int>(src_index);
        int index2 = std::min(index1 + 1, input_samples - 1);
        float fraction = src_index - index1;
        
        if (index1 < input_samples) {
            int16_t sample1 = input[index1];
            int16_t sample2 = input[index2];
            int16_t interpolated = static_cast<int16_t>(sample1 + fraction * (sample2 - sample1));
            output.push_back(interpolated);
        } else {
            output.push_back(0);
        }
    }
    // ESP_LOGI(TAG, "Resampled audio from %d Hz to %d Hz: %d -> %d samples", input_rate, output_rate, input_samples, output_samples);
    
    return output;
}

// 上传播放历史
void Esp32Music::SendPlayHistory(int song_id) {
    ESP_LOGI(TAG, "Queueing play history for song ID: %d", song_id);

    // 直接传递 song_id，先转换为 void*
    xTaskCreate(
        [](void* pvParameter) {
            int song_id = reinterpret_cast<intptr_t>(pvParameter);  // 取回 song_id
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(1);
            if (!http) {
                ESP_LOGE(TAG, "Failed to create HTTP client for play history");
                vTaskDelete(nullptr);
                return;
            }

            http->SetHeader("Content-Type", "application/json");
            http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());

            cJSON* root = cJSON_CreateObject();
            cJSON_AddNumberToObject(root, "song_id", song_id);
            char* json_str = cJSON_PrintUnformatted(root);
            std::string data(json_str);
            cJSON_free(json_str);
            cJSON_Delete(root);

            http->SetContent(std::move(data));

            std::string url = std::string(CONFIG_SERVER_URL) + "/device_music_record";

            const int max_retries = 3;
            int retry_count = 0;
            bool success = false;

            while (retry_count < max_retries && !success) {
                if (http->Open("POST", url)) {
                    int status_code = http->GetStatusCode();
                    if (status_code == 200) {
                        http->ReadAll();  // 消费响应体，避免 Close 时底层报 "expected N bytes" 提前断连告警
                        ESP_LOGI(TAG, "Successfully sent play history for song ID: %d", song_id);
                        success = true;
                    } else {
                        ESP_LOGE(TAG, "Failed to send play history, status code: %d", status_code);
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to connect to play history API (attempt %d/%d)",
                             retry_count + 1, max_retries);
                }

                if (!success && (retry_count + 1) < max_retries) {
                    vTaskDelay(pdMS_TO_TICKS(1000 * (retry_count + 1)));
                }
                retry_count++;
            }

            http->Close();
            vTaskDelete(nullptr);
        },
        "SendPlayHistoryTask",
        4096,
        reinterpret_cast<void*>(song_id),  // 直接传 song_id
        tskIDLE_PRIORITY + 1, // 比空闲任务高一个优先级，用于不需要高优先级的后台任务
        nullptr
    );
}


// 循环播放
std::string Esp32Music::LoopPlay(const std::string& mode) {
    ESP_LOGI(TAG, "Starting loop play from music collection");

    // 第一阶段：持锁仅做"停止旧循环 + 判断是否有缓存歌单"。
    // 注意：严禁在持锁期间做同步网络请求或 vTaskDelay，否则会阻塞
    // PlayNextSong/StopLoopPlay/MCP 等并发调用，甚至卡死语音控制。
    bool need_fetch = false;
    {
        std::lock_guard<std::recursive_mutex> lock(streaming_mutex_);
        if (is_loop_playing_) {
            StopLoopPlay();
            // 注意：StopLoopPlay 内部已等待旧任务删除，这里不再持锁 sleep
        }
        // 在持锁状态下读取 music_collection_，避免与 ResetLoop 的并发写产生 data race
        need_fetch = music_collection_.empty();
    }

    // 第二阶段：锁外拉取歌单（耗时网络操作，不能持锁）
    if (need_fetch) {
        std::string base_url = std::string(CONFIG_SERVER_URL) + "/device_music_collection";
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);

        http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
        http->SetHeader("Accept", "application/json");
        http->SetHeader("Device-ID", SystemInfo::GetMacAddress().c_str());

        if (!http->Open("GET", base_url)) {
            ESP_LOGE(TAG, "Failed to connect to music collection API");
            return "Failed to connect to music collection API";
        }

        int status_code = http->GetStatusCode();
        std::string response = http->ReadAll();
        cJSON* root = cJSON_Parse(response.c_str());
        http->Close();
        if (status_code != 200) {
            if (root) {
                cJSON* msg = cJSON_GetObjectItem(root, "msg");
                std::string error_msg = msg ? msg->valuestring : "Unknown error";
                cJSON_Delete(root);
                return error_msg;
            }
            return "Failed to get music collection";
        }

        cJSON* data_obj = cJSON_GetObjectItem(root, "data");
        cJSON* quota_obj = cJSON_GetObjectItem(data_obj, "quota");
        int quota = quota_obj ? quota_obj->valueint : -1; // -1 表示无限制
        cJSON* data_array = cJSON_GetObjectItem(data_obj, "collections");

        std::vector<std::map<std::string, std::string>> collection;
        int array_size = cJSON_GetArraySize(data_array);
        for (int i = 0; i < array_size; i++) {
            cJSON* song_item = cJSON_GetArrayItem(data_array, i);
            std::map<std::string, std::string> song_info;

            cJSON* song_id = cJSON_GetObjectItem(song_item, "song_id");
            cJSON* song = cJSON_GetObjectItem(song_item, "song");
            cJSON* singer = cJSON_GetObjectItem(song_item, "singer");
            cJSON* audio_url = cJSON_GetObjectItem(song_item, "audio_url");
            cJSON* lyric_url = cJSON_GetObjectItem(song_item, "lyric_url");

            if (cJSON_IsString(song) && cJSON_IsString(singer) && cJSON_IsString(audio_url)) {
                song_info["song_id"] = std::to_string(song_id ? song_id->valueint : 0);
                song_info["song"] = song->valuestring;
                song_info["singer"] = singer->valuestring;
                song_info["audio_url"] = audio_url->valuestring;
                song_info["lyric_url"] = cJSON_IsString(lyric_url) ? lyric_url->valuestring : "";
                collection.push_back(song_info);
            }
        }
        cJSON_Delete(root);
        if (collection.empty()) {
            ESP_LOGW(TAG, "Music collection is empty");
            return "当前设备歌单为空";
        }

        // 写回共享状态需在锁保护下
        {
            std::lock_guard<std::recursive_mutex> lock(streaming_mutex_);
            music_collection_ = std::move(collection);
            current_song_index_ = 0;
            current_quota_ = quota;
            ESP_LOGI(TAG, "Loaded %d songs in collection, quota: %d", music_collection_.size(), (int)current_quota_.load());
        }
    } else {
        size_t cached_size = 0;
        int cached_idx = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(streaming_mutex_);
            cached_size = music_collection_.size();
            cached_idx = current_song_index_.load();
        }
        ESP_LOGI(TAG, "Using cached music collection with %d songs, resuming from index %d",
                 cached_size, cached_idx);
    }

    // 第三阶段：再次持锁，做配额校验与启动任务（此时不再有网络操作）
    {
        std::lock_guard<std::recursive_mutex> lock(streaming_mutex_);
        if (current_quota_ == 0) {
            ESP_LOGW(TAG, "Quota exhausted, cannot start loop play");
            return "今天歌曲播放配额已用完";
        }
        // 是否随机播放
        if (mode == "random") {
            for (int i = music_collection_.size() - 1; i > music_collection_.size()/2; i--) {
                int j = esp_random() % (i + 1);
                std::swap(music_collection_[i], music_collection_[j]);
            }
            ESP_LOGI(TAG, "Music collection shuffled for random play");
        }

        // 启动循环播放任务
        is_loop_playing_ = true;

        xTaskCreate([](void* param) {
            static_cast<Esp32Music*>(param)->LoopPlayTask();
        }, "loop_play_task", 4096, this, 5, &loop_play_task_handle_);
    }

    return "";
}

// 循环播放任务
void Esp32Music::LoopPlayTask() {
    ESP_LOGI(TAG, "Loop play task started");
    // current_quota_ == -1 表示无限制（服务端未设配额）；>0 时表示剩余可播放数量
    // 注意：循环条件对 music_collection_ 的读取必须在锁保护下进行，否则与 ResetLoop
    // （持锁 clear）构成 data race（未定义行为）。故此处仅用 atomic 标志做前置判断，
    // 真正的集合非空判断放到下方持锁块内。
    while (is_loop_playing_ && current_quota_ != 0) {
        // 持锁保护对 music_collection_ / current_song_index_ 的访问，避免与 ResetLoop
        // （持锁 clear）并发导致 vector 越界或 data race。仅拷贝本首歌所需字段后立即解锁，
        // 不把 StartStreaming 包在锁内，避免持锁时间过长阻塞其它调用。
        std::string song_url, lyric_url, song_name, singer_name;
        int song_id = 0;
        size_t next_index = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(streaming_mutex_);
            if (music_collection_.empty()) {
                break;  // 歌单被并发清空，安全退出
            }
            if (current_quota_ > 0) {
                current_quota_--;  // 有限配额时递减；-1（无限制）不递减
            }
            size_t idx = current_song_index_ % music_collection_.size();
            auto& current_song = music_collection_[idx];
            song_url   = current_song["audio_url"];
            lyric_url  = current_song["lyric_url"];
            song_name  = current_song["song"];
            singer_name = current_song["singer"];
            try {
                song_id = std::stoi(current_song["song_id"]);
            } catch (const std::exception& e) {
                song_id = 0;
            }
            next_index = (idx + 1) % music_collection_.size();
            ESP_LOGI(TAG, "Playing song %d/%d: %s - %s quota: %d",
                     idx + 1, music_collection_.size(),
                     singer_name.c_str(), song_name.c_str(), (int)current_quota_.load());
        }

        // 设置当前歌曲信息（这些成员仅本任务写、歌词任务/播放任务读，非共享容器无需持锁）
        current_song_name_ = song_name;
        current_music_url_ = song_url;
        current_lyric_url_ = lyric_url;
        current_song_id_ = song_id;

        // 开始播放当前歌曲
        if (StartStreaming(current_music_url_)) {
            // 下载歌词（如果有）—— 异步执行，避免在网络异常时阻塞循环播放任务
            if (!current_lyric_url_.empty()) {
                auto* lyric_args = new std::pair<Esp32Music*, std::string>(this, current_lyric_url_);
                xTaskCreate(
                    [](void* param) {
                        auto* args = static_cast<std::pair<Esp32Music*, std::string>*>(param);
                        if (!args->first->DownloadLyrics(args->second)) {
                            ESP_LOGW(TAG, "Failed to download lyrics for current song");
                        }
                        delete args;
                        vTaskDelete(nullptr);
                    },
                    "loop_lyric_dl", 4096, lyric_args, 4, nullptr);
            }

            // 等待当前歌曲播放完成
            auto& audio_service = Application::GetInstance().GetAudioService();
            while ((!play_task_finished_ || !audio_service.IsIdle()) && is_loop_playing_) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        } else {
            ESP_LOGE(TAG, "Failed to start streaming for song: %s", song_name.c_str());
        }

        // 如果还在循环播放模式，切换到下一首
        if (is_loop_playing_) {
            current_song_index_ = next_index;
            vTaskDelay(pdMS_TO_TICKS(500)); // 短暂延迟
        }
    }
    ESP_LOGI(TAG, "Loop play task finished, with quota: %d", (int)current_quota_.load());
    loop_play_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

bool Esp32Music::StopLoopPlay() {
    std::lock_guard<std::recursive_mutex> lock(streaming_mutex_);
    ESP_LOGI(TAG, "Stopping loop play");
    is_loop_playing_ = false;
    
    // 停止当前播放
    StopStreaming();
    
    if (current_song_id_ > 0) {
        ESP_LOGI(TAG, "Sending currend_song_id %d", current_song_id_);
        SendPlayHistory(current_song_id_);
        current_song_id_ = 0;
    }
    // 给app发送播放完成消息
    auto& app = Application::GetInstance();
    app.music_playing_ = false;
    app.FlushConversationQueue();

    // music_collection_.clear(); // 不清空，下次接着播放
    // current_song_index_ = 0;
    ESP_LOGI(TAG, "Loop play stopped, will resume from song %d next time", (int)current_song_index_.load() + 1);
    return true;
}

void Esp32Music::ResetLoop() {
    std::lock_guard<std::recursive_mutex> lock(streaming_mutex_);
    music_collection_.clear();
    current_song_index_ = 0;
    ESP_LOGI(TAG, "Music collection reset, will fetch from server next time");
}

// 播放下一首歌曲（异步执行，避免阻塞调用方任务，如 MCP 接收任务）
std::string Esp32Music::PlayNextSong() {
    std::unique_lock<std::recursive_mutex> lock(streaming_mutex_);
    ESP_LOGI(TAG, "Playing next song (async)");

    // 前置校验（同步、轻量），不触发网络请求，可在调用方任务内安全执行
    if (music_collection_.empty()) {
        ESP_LOGW(TAG, "Music collection is empty");
        return "当前设备歌单为空";
    }
    // 与 LoopPlay 语义一致：仅 current_quota_ == 0 表示今日已用完；
    // -1 表示无限制（应放行），避免无限配额下误报"配额用完"
    if (current_quota_ == 0) {
        ESP_LOGW(TAG, "Quota exhausted");
        return "今天歌曲播放配额已用完";
    }

    // 在持锁状态下完成索引自增，避免与 LoopPlay/LoopPlayTask 并发读写 current_song_index_
    // 造成竞态；同时用 size() 保护，防止 music_collection_ 被并发清空后取模除零。
    size_t collection_size = music_collection_.size();
    if (collection_size == 0) {
        ESP_LOGW(TAG, "Music collection became empty, abort");
        return "当前设备歌单为空";
    }
    current_song_index_ = (current_song_index_.load() + 1) % collection_size;

    // 校验通过并安全更新索引后，释放锁再创建后台任务，避免持锁跨线程调用 LoopPlay
    lock.unlock();

    // LoopPlay 内部会做同步歌单请求，放进后台任务执行，避免阻塞调用方
    auto* self = this;
    xTaskCreate(
        [](void* param) {
            auto* music = static_cast<Esp32Music*>(param);
            music->LoopPlay("loop");
            vTaskDelete(nullptr);
        },
        "music_next_song", 8192, self, 5, nullptr);

    return "";
}