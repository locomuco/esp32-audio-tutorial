/* Record WAV file to SD Card

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "esp_http_client.h"
#include "sdkconfig.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "board.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "wav_encoder.h"
#include "esp_peripherals.h"
#include "periph_button.h"
#include "periph_wifi.h"
#include "filter_resample.h"
#include "audio_sonic.h"
#include "raw_stream.h"

#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "rec_eng_helper.h"


static const char *TAG = "REC_RAW_HTTP";
static const char *EVENT_TAG = "asr_event";

typedef enum {
    WAKE_UP = 1,
    OPEN_THE_LIGHT,
    CLOSE_THE_LIGHT,
    VOLUME_INCREASE,
    VOLUME_DOWN,
    PLAY,
    PAUSE,
    MUTE,
    PLAY_LOCAL_MUSIC,
} asr_event_t;

esp_err_t _http_stream_event_handle(http_stream_event_msg_t *msg)
{
    esp_http_client_handle_t http = (esp_http_client_handle_t)msg->http_client;
    char len_buf[16];
    static int total_write = 0;

    if (msg->event_id == HTTP_STREAM_PRE_REQUEST) {
        // set header
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_PRE_REQUEST, lenght=%d", msg->buffer_len);
        esp_http_client_set_header(http, "x-audio-sample-rates", "16000");
        esp_http_client_set_header(http, "x-audio-bits", "16");
        esp_http_client_set_header(http, "x-audio-channel", "2");
        total_write = 0;
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_ON_REQUEST) {
        // write data
        int wlen = sprintf(len_buf, "%x\r\n", msg->buffer_len);
        if (esp_http_client_write(http, len_buf, wlen) <= 0) {
            return ESP_FAIL;
        }
        if (esp_http_client_write(http, msg->buffer, msg->buffer_len) <= 0) {
            return ESP_FAIL;
        }
        if (esp_http_client_write(http, "\r\n", 2) <= 0) {
            return ESP_FAIL;
        }
        total_write += msg->buffer_len;
        printf("\033[A\33[2K\rTotal bytes written: %d\n", total_write);
        return msg->buffer_len;
    }

    if (msg->event_id == HTTP_STREAM_POST_REQUEST) {
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_POST_REQUEST, write end chunked marker");
        if (esp_http_client_write(http, "0\r\n\r\n", 5) <= 0) {
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_FINISH_REQUEST) {
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_FINISH_REQUEST");
        char *buf = calloc(1, 64);
        assert(buf);
        int read_len = esp_http_client_read(http, buf, 64);
        if (read_len <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        buf[read_len] = 0;
        ESP_LOGI(TAG, "Got HTTP Response = %s", (char *)buf);
        free(buf);
        return ESP_OK;
    }
    return ESP_OK;
}

#define SAMPLE_RATE         16000
//#define CHANNEL             1
#define CHANNEL             2
#define BITS                16

#define SONIC_PITCH         1.4f
#define SONIC_SPEED         2.0f

static audio_element_handle_t create_sonic()
{
    sonic_cfg_t sonic_cfg = DEFAULT_SONIC_CONFIG();
    sonic_cfg.sonic_info.samplerate = SAMPLE_RATE;
    sonic_cfg.sonic_info.channel = CHANNEL;
    sonic_cfg.sonic_info.resample_linear_interpolate = 1;
    return sonic_init(&sonic_cfg);
}

void app_main(void)
{
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t http_stream_writer, i2s_stream_reader;

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    esp_log_level_set(EVENT_TAG, ESP_LOG_INFO);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    tcpip_adapter_init();

    ESP_LOGI(TAG, "[ 1 ] Initialize Button Peripheral & Connect to wifi network");
    // Initialize peripherals management
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

    periph_wifi_cfg_t wifi_cfg = {
        .ssid = CONFIG_WIFI_SSID,
        .password = CONFIG_WIFI_PASSWORD,
    };
    esp_periph_handle_t wifi_handle = periph_wifi_init(&wifi_cfg);

    // Initialize Button peripheral
    periph_button_cfg_t btn_cfg = {
        .gpio_mask = GPIO_SEL_36 | GPIO_SEL_39, //REC BTN & MODE BTN
    };
    esp_periph_handle_t button_handle = periph_button_init(&btn_cfg);

    // Start wifi & button peripheral
    esp_periph_start(set, button_handle);
    esp_periph_start(set, wifi_handle);
    periph_wifi_wait_for_connected(wifi_handle, portMAX_DELAY);

    ESP_LOGI(TAG, "Initialize SR handle");
    esp_wn_iface_t *wakenet;
    model_coeff_getter_t *model_coeff_getter;
    model_iface_data_t *model_data;

    get_wakenet_iface(&wakenet);
    get_wakenet_coeff(&model_coeff_getter);
    model_data = wakenet->create(model_coeff_getter, DET_MODE_90);
    int num = wakenet->get_word_num(model_data);
    for (int i = 1; i <= num; i++) {
        char *name = wakenet->get_word_name(model_data, i);
        ESP_LOGI(TAG, "keywords: %s (index = %d)", name, i);
    }
    float threshold = wakenet->get_det_threshold(model_data, 1);
    int sample_rate = wakenet->get_samp_rate(model_data);
    int audio_chunksize = wakenet->get_samp_chunksize(model_data);
    ESP_LOGI(EVENT_TAG, "keywords_num = %d, threshold = %f, sample_rate = %d, chunksize = %d, sizeof_uint16 = %d", num, threshold, sample_rate, audio_chunksize, sizeof(int16_t));
    int16_t *buff = (int16_t *)malloc(audio_chunksize * sizeof(short));
    if (NULL == buff) {
        ESP_LOGE(EVENT_TAG, "Memory allocation failed!");
        wakenet->destroy(model_data);
        model_data = NULL;
        return;
    }


    ESP_LOGI(TAG, "[ 2 ] Start codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "[3.0] Create audio pipeline for recording");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    ESP_LOGI(TAG, "[3.1] Create http stream to post data to server");

    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.type = AUDIO_STREAM_WRITER;
    http_cfg.event_handle = _http_stream_event_handle;
    http_stream_writer = http_stream_init(&http_cfg);

    ESP_LOGI(TAG, "[3.2] Create i2s stream to read audio data from codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_READER;
    i2s_cfg.i2s_config.sample_rate = 16000;
    //i2s_cfg.i2s_config.sample_rate = 48000;
#if defined CONFIG_ESP_LYRAT_MINI_V1_1_BOARD
    i2s_cfg.i2s_port = 1;
#endif
    i2s_stream_reader = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(EVENT_TAG, "[ 2.2 ] Create filter to resample audio data");
    rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    //rsp_cfg.src_rate = 48000;
    rsp_cfg.src_rate = 16000;
    rsp_cfg.src_ch = 2;
    rsp_cfg.dest_rate = 16000;
    rsp_cfg.dest_ch = 1;
    rsp_cfg.type = AUDIO_CODEC_TYPE_ENCODER;
    audio_element_handle_t filter = rsp_filter_init(&rsp_cfg);

    audio_element_handle_t sonic_el = create_sonic();

    raw_stream_cfg_t raw_cfg = {
        .out_rb_size = 8 * 1024,
        .type = AUDIO_STREAM_READER,
    };
    audio_element_handle_t raw_read = raw_stream_init(&raw_cfg);


    ESP_LOGI(TAG, "[3.3] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, i2s_stream_reader, "i2s");
    audio_pipeline_register(pipeline, sonic_el,   "sonic");
    audio_pipeline_register(pipeline, http_stream_writer, "http");
    audio_pipeline_register(pipeline, raw_read, "raw");
    audio_pipeline_register(pipeline, filter, "filter");

    ESP_LOGI(TAG, "[3.4] Link it together [codec_chip]-->i2s_stream->http_stream-->[http_server]");
    //audio_pipeline_link(pipeline, (const char *[]) {"i2s", "http"}, 2);
    audio_pipeline_link(pipeline, (const char *[]) {"i2s", "sonic", "http"}, 3);
    //audio_pipeline_link(pipeline, (const char *[]) {"i2s", "filter", "raw"}, 3);

    sonic_set_pitch_and_speed_info(sonic_el, SONIC_PITCH, 1.0f);

    ESP_LOGI(TAG, "[ 4 ] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[4.1] Listening event from the pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    ESP_LOGI(TAG, "[4.2] Listening event from peripherals");
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    ESP_LOGI(TAG, "[ 5 ] Listen for all pipeline events");
    //audio_pipeline_run(pipeline);
    while (1) {
#if 0
        raw_stream_read(raw_read, (char *)buff, audio_chunksize * sizeof(short));
        int keyword = wakenet->detect(model_data, (int16_t *)buff);
        switch (keyword) {
            case WAKE_UP:
                ESP_LOGI(TAG, "Wake up");
                break;
        }
#else
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }

        if (msg.source_type != PERIPH_ID_BUTTON) {
            continue;
        }

        // It's not REC button
        if ((int)msg.data == GPIO_NUM_39) {
            break;
        }

        // It's not REC button
        if ((int)msg.data != GPIO_NUM_36) {
            continue;
        }

        if (msg.cmd == PERIPH_BUTTON_PRESSED) {
            ESP_LOGI(TAG, "[ * ] Resuming pipeline");
            audio_element_set_uri(http_stream_writer, CONFIG_SERVER_URI);
            audio_pipeline_run(pipeline);
        } else if (msg.cmd == PERIPH_BUTTON_RELEASE || msg.cmd == PERIPH_BUTTON_LONG_RELEASE) {
            ESP_LOGI(TAG, "[ * ] Stop pipeline");
            audio_pipeline_stop(pipeline);
            audio_pipeline_wait_for_stop(pipeline);
            audio_pipeline_terminate(pipeline);
        }
#endif
    }
    ESP_LOGI(TAG, "[ 6 ] Stop audio_pipeline");
    audio_pipeline_terminate(pipeline);

    audio_pipeline_unregister(pipeline, sonic_el);
    audio_pipeline_unregister(pipeline, http_stream_writer);
    audio_pipeline_unregister(pipeline, i2s_stream_reader);

    /* Terminal the pipeline before removing the listener */
    audio_pipeline_remove_listener(pipeline);

    /* Stop all periph before removing the listener */
    esp_periph_set_stop_all(set);
    audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(evt);

    /* Release all resources */
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(http_stream_writer);
    audio_element_deinit(i2s_stream_reader);
    esp_periph_set_destroy(set);
}