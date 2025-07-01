#include "afe_wake_word.h"
#include "application.h"

#include <esp_log.h>
#include <model_path.h>
#include <arpa/inet.h>
#include <sstream>

//增加头文件所需文件
#include <esp_afe_sr_models.h>
#include <esp_nsn_models.h>
#include "esp_mn_speech_commands.h"
#include "esp_mn_models.h"
#include "esp_mn_iface.h"


#define DETECTION_RUNNING_EVENT 1

#define TAG "AfeWakeWord"

#define ESP_MN_PREFIX "mn"

TaskHandle_t command_detection_task_ = nullptr;  
esp_mn_iface_t *multinet;   
model_iface_data_t *model_data;


AfeWakeWord::AfeWakeWord()
    : afe_data_(nullptr),
      wake_word_pcm_(),
      wake_word_opus_() {

    event_group_ = xEventGroupCreate();
}

AfeWakeWord::~AfeWakeWord() {
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }

    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
    }

    vEventGroupDelete(event_group_);
}

void AfeWakeWord::Initialize(AudioCodec *codec)
{
    codec_ = codec;                                 
    int ref_num = codec_->input_reference() ? 1 : 0; 
 
    srmodel_list_t *models = esp_srmodel_init("model"); // 初始化语音识别模型列表，从"model"目录加载
 
    // 遍历所有加载的模型
    for (int i = 0; i < models->num; i++)
    {                                                            
        // ESP_LOGI(TAG, "Model %d: %s", i, models->model_name[i]); // 删除模型打印

        if (strstr(models->model_name[i], ESP_MN_PREFIX) != NULL)   //此处 ESP_MN_PREFIX 为 "mn" 也就是新加的模型库
        {
            // 获取 multinet 模型句柄
            multinet = esp_mn_handle_from_name(models->model_name[i]);
            if (!multinet)
            {
                // ESP_LOGI("MULTINET", "failed to create multinet handle");
                continue;
            }
 
            //加载模型数据
            model_data = multinet->create(models->model_name[i], 6000);
            if (!model_data)
            {
                // ESP_LOGI("MULTINET", "failed to create model_data handle");
                continue;
            }
        }
        if (model_data)
        {
            //此处使用拼音+空格的方式添加想要自定义的唤醒词
            esp_mn_commands_clear();
            esp_mn_commands_add(1, "ni hao mo si");
            esp_mn_commands_add(2, "mo si");
            esp_mn_commands_add(3, "xiao nuo");
            esp_mn_commands_update();
        }
        esp_mn_active_commands_print();  //只保留这行打印

        if (strstr(models->model_name[i], ESP_WN_PREFIX) != NULL)
        {                                                                    
            wakenet_model_ = models->model_name[i];                         
            auto words = esp_srmodel_get_wake_words(models, wakenet_model_); 
            // split by ";" to get all wake words
            std::stringstream ss(words); 
            std::string word;
            while (std::getline(ss, word, ';'))
            {
                wake_words_.push_back(word);
            }
        }
    }
 
    std::string input_format; 
    for (int i = 0; i < codec_->input_channels() - ref_num; i++)
    {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++)
    {
        input_format.push_back('R');
    }
    afe_config_t *afe_config = afe_config_init(input_format.c_str(), models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    afe_config->aec_init = codec_->input_reference();
    afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;
    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = 1;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
 
    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);
 
    xTaskCreate([](void *arg)
                {
        auto this_ = (AfeWakeWord*)arg;
        this_->AudioDetectionTask();
        vTaskDelete(NULL); }, "audio_detection", 4096, this, 3, nullptr);
 
    //新增一个任务
    xTaskCreate([](void *arg)
                {
        auto this_ = (AfeWakeWord*)arg;
        this_->CommandDetectionTask();
        vTaskDelete(NULL); }, "command_detection", 4096, this, 5, &command_detection_task_);
}

void AfeWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

void AfeWakeWord::StartDetection() {
    xEventGroupSetBits(event_group_, DETECTION_RUNNING_EVENT);
}

void AfeWakeWord::StopDetection() {
    xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
}

bool AfeWakeWord::IsDetectionRunning() {
    return xEventGroupGetBits(event_group_) & DETECTION_RUNNING_EVENT;
}

void AfeWakeWord::Feed(const std::vector<int16_t>& data) {
    if (afe_data_ == nullptr) {
        return;
    }
    afe_iface_->feed(afe_data_, data.data());
}

size_t AfeWakeWord::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_) * codec_->input_channels();
}

void AfeWakeWord::AudioDetectionTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    // ESP_LOGI(TAG, "Audio detection task started, feed size: %d fetch size: %d",
    //     feed_size, fetch_size);

    while (true) {
        xEventGroupWaitBits(event_group_, DETECTION_RUNNING_EVENT, pdFALSE, pdTRUE, portMAX_DELAY);

        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            continue;;
        }

        // Store the wake word data for voice recognition, like who is speaking
        StoreWakeWordData(res->data, res->data_size / sizeof(int16_t));

        if (res->wakeup_state == WAKENET_DETECTED) {
            // 不要立即停止检测，让命令词检测继续工作
            // StopDetection();
            last_detected_wake_word_ = wake_words_[res->wake_word_index - 1];

            if (wake_word_detected_callback_) {
                wake_word_detected_callback_(last_detected_wake_word_);
            }
        }
        // 通知命令词检测任务
        else if (res->data && model_data)
        {
            xTaskNotifyGive(command_detection_task_);
        }
    }
}

void AfeWakeWord::StoreWakeWordData(const int16_t* data, size_t samples) {
    // store audio data to wake_word_pcm_
    wake_word_pcm_.emplace_back(std::vector<int16_t>(data, data + samples));
    // keep about 2 seconds of data, detect duration is 30ms (sample_rate == 16000, chunksize == 512)
    while (wake_word_pcm_.size() > 2000 / 30) {
        wake_word_pcm_.pop_front();
    }
}

void AfeWakeWord::EncodeWakeWordData() {
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ = (StackType_t*)heap_caps_malloc(4096 * 8, MALLOC_CAP_SPIRAM);
    }
    wake_word_encode_task_ = xTaskCreateStatic([](void* arg) {
        auto this_ = (AfeWakeWord*)arg;
        {
            auto start_time = esp_timer_get_time();
            auto encoder = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
            encoder->SetComplexity(0); // 0 is the fastest

            int packets = 0;
            for (auto& pcm: this_->wake_word_pcm_) {
                encoder->Encode(std::move(pcm), [this_](std::vector<uint8_t>&& opus) {
                    std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                    this_->wake_word_opus_.emplace_back(std::move(opus));
                    this_->wake_word_cv_.notify_all();
                });
                packets++;
            }
            this_->wake_word_pcm_.clear();

            auto end_time = esp_timer_get_time();
            ESP_LOGI(TAG, "Encode wake word opus %d packets in %ld ms", packets, (long)((end_time - start_time) / 1000));

            std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
            this_->wake_word_opus_.push_back(std::vector<uint8_t>());
            this_->wake_word_cv_.notify_all();
        }
        vTaskDelete(NULL);
    }, "encode_detect_packets", 4096 * 8, this, 2, wake_word_encode_task_stack_, &wake_word_encode_task_buffer_);
}

bool AfeWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() {
        return !wake_word_opus_.empty();
    });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}

void AfeWakeWord::CommandDetectionTask()
{
    // ESP_LOGI(TAG, "Command detection task started");
    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); //阻塞 收到通知后再跑
 
        esp_mn_state_t mn_state;
        esp_mn_results_t *mn_result;
 
        // 从AudioDetectionTask获取最新的音频数据
        // 这里需要从AFE获取音频数据进行命令词检测
        if (model_data && afe_data_)
        {
            // 获取音频数据 - 使用阻塞方式获取
            auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
            if (res && res->data && res->ret_value == ESP_OK)
            {
                // ESP_LOGI(TAG, "Processing audio data for command detection, data_size: %d", res->data_size);
                mn_state = multinet->detect(model_data, res->data);
                //判断是否检测到命令词
                if (mn_state == ESP_MN_STATE_DETECTING)
                {
                    // ESP_LOGI(TAG, "Command detection: detecting...");
                    continue;
                }
                else if (mn_state == ESP_MN_STATE_DETECTED)
                {
                    mn_result = multinet->get_results(model_data);
                    //此处id就是你添加命令词的id 注意 第一个是0 了，在此处自己可以写个回调函数任意处理 也可以控制其他设备 也可以当作唤醒词处理
                    if (mn_result != nullptr && mn_result->num > 0)
                    {
                        int command_id = mn_result->phrase_id[0];
                        // ESP_LOGI(TAG, "Command detected: id=%d", command_id);
                        if (command_id == 0)
                        {
                            // ESP_LOGI(TAG, "Invoking: 你好MOSS");
                            Application::GetInstance().WakeWordInvoke("你好MOSS");
                        }
                        else if (command_id == 1)
                        {
                            // ESP_LOGI(TAG, "Invoking: MOSS");
                            Application::GetInstance().WakeWordInvoke("MOSS");
                        }
                        else if (command_id == 2)
                        {
                            // ESP_LOGI(TAG, "Invoking: 忆梦");
                            Application::GetInstance().WakeWordInvoke("忆梦");
                        }
                    }
                }
                // else
                // {
                //     ESP_LOGI(TAG, "Command detection state: %d", mn_state);
                // }
            }
            // else
            // {
            //     ESP_LOGW(TAG, "Failed to get audio data for command detection");
            // }
        }
        // else
        // {
        //     ESP_LOGW(TAG, "model_data or afe_data_ is null");
        // }
    }
}