/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/*  WiFi softAP & station Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_net_stack.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#if IP_NAPT
#include "lwip/lwip_napt.h"
#endif
#include "lwip/err.h"
#include "lwip/sys.h"

#include "esp_http_client.h"

// #include "cJSON.h"

#include "esp_http_server.h"
#include "led_strip_encoder.h"
#include "little-lamp.h"
#include "index_html.h"

/* The examples use WiFi configuration that you can set via project configuration menu.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_ESP_WIFI_STA_SSID "mywifissid"
*/

/* STA Configuration */
#define EXAMPLE_ESP_WIFI_STA_SSID           CONFIG_ESP_WIFI_REMOTE_AP_SSID
#define EXAMPLE_ESP_WIFI_STA_PASSWD         CONFIG_ESP_WIFI_REMOTE_AP_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY           CONFIG_ESP_MAXIMUM_STA_RETRY

#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WAPI_PSK
#endif

/* AP Configuration */
#define EXAMPLE_ESP_WIFI_AP_SSID            CONFIG_ESP_WIFI_AP_SSID
#define EXAMPLE_ESP_WIFI_AP_PASSWD          CONFIG_ESP_WIFI_AP_PASSWORD
#define EXAMPLE_ESP_WIFI_CHANNEL            CONFIG_ESP_WIFI_AP_CHANNEL
#define EXAMPLE_MAX_STA_CONN                CONFIG_ESP_MAX_STA_CONN_AP


/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define LEDC_TIMER_A              LEDC_TIMER_0
#define LEDC_TIMER_B              LEDC_TIMER_1
#define LEDC_TIMER_C              LEDC_TIMER_2
#define LEDC_TIMER_D              LEDC_TIMER_3
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_OUTPUT_IO_A          (4) // Define the output GPIO
#define LEDC_OUTPUT_IO_B          (3) // Define the output GPIO
#define LEDC_OUTPUT_IO_C          (5) // Define the output GPIO
#define LEDC_OUTPUT_IO_D          (6) // Define the output GPIO
#define LEDC_CHANNEL_A            LEDC_CHANNEL_0
#define LEDC_CHANNEL_B            LEDC_CHANNEL_1
#define LEDC_CHANNEL_C            LEDC_CHANNEL_2
#define LEDC_CHANNEL_D            LEDC_CHANNEL_3
#define LEDC_DUTY_RES           LEDC_TIMER_11_BIT // Set duty resolution to x bits
#define LEDC_MAX_DUTY           (2048 - 1) //(512-1)
// Set duty to 5%. ((2 ** 13) - 1) * 5% = 409
#define LEDC_DUTY               (LEDC_MAX_DUTY * 11 / 100)
#define LEDC_FREQUENCY          (20000) // Frequency in Hertz. Set frequency at xx kHz

#define RMT_LED_STRIP_RESOLUTION_HZ 10000000 // 10MHz resolution, 1 tick = 0.1us (led strip needs a high resolution)
#define RMT_LED_STRIP_GPIO_NUM      7

#define EXAMPLE_LED_NUMBERS         4

static const char *TAG_AP = "WiFi SoftAP";
static const char *TAG_STA = "WiFi Sta";
static const char *TAG = "esp-little-lamp";
static char json_response[1024];

static uint8_t led_strip_pixels[EXAMPLE_LED_NUMBERS * 3];
static lamp_info_t lamp_info = {
    .mode = LAMP_MODE_NORMAL,
    .brightnessA = 11,
    .brightnessB = 11,
    .brightness = 0,
    .warm = 0,
    .color = 0
};

static int s_retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;
httpd_handle_t httpd = NULL;

rmt_channel_handle_t led_chan = NULL;
rmt_encoder_handle_t led_encoder = NULL;
rmt_tx_channel_config_t tx_chan_config = {
    .clk_src = RMT_CLK_SRC_DEFAULT, // select source clock
    .gpio_num = RMT_LED_STRIP_GPIO_NUM,
    .mem_block_symbols = 64, // increase the block size can make the LED less flickering
    .resolution_hz = RMT_LED_STRIP_RESOLUTION_HZ,
    .trans_queue_depth = 4, // set the number of transactions that can be pending in the background
};
rmt_transmit_config_t tx_config = {
    .loop_count = 0, // no transfer loop
};

/* FreeRTOS event group to signal when we are connected/disconnected */
static EventGroupHandle_t s_wifi_event_group;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
        ESP_LOGI(TAG_AP, "Station "MACSTR" joined, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
        ESP_LOGI(TAG_AP, "Station "MACSTR" left, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG_STA, "Station started");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG_STA, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* Initialize soft AP */
esp_netif_t *wifi_init_softap(void)
{
    esp_netif_t *esp_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_AP_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_AP_SSID),
            .channel = EXAMPLE_ESP_WIFI_CHANNEL,
            .password = EXAMPLE_ESP_WIFI_AP_PASSWD,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    if (strlen(EXAMPLE_ESP_WIFI_AP_PASSWD) == 0) {
        wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));

    ESP_LOGI(TAG_AP, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
             EXAMPLE_ESP_WIFI_AP_SSID, EXAMPLE_ESP_WIFI_AP_PASSWD, EXAMPLE_ESP_WIFI_CHANNEL);

    return esp_netif_ap;
}

/* Initialize wifi station */
esp_netif_t *wifi_init_sta(void)
{
    esp_netif_t *esp_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_config_t wifi_sta_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_STA_SSID,
            .password = EXAMPLE_ESP_WIFI_STA_PASSWD,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .failure_retry_cnt = EXAMPLE_ESP_MAXIMUM_RETRY,
            /* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (pasword len => 8).
             * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
             * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
            * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
             */
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config) );

    ESP_LOGI(TAG_STA, "wifi_init_sta finished.");

    return esp_netif_sta;
}

/////////

/**
 * @brief Simple helper function, converting HSV color space to RGB color space
 *
 * Wiki: https://en.wikipedia.org/wiki/HSL_and_HSV
 *
 */
void led_strip_hsv2rgb(uint32_t h, uint32_t s, uint32_t v, uint32_t *r, uint32_t *g, uint32_t *b)
{
    h %= 360; // h -> [0,360]
    uint32_t rgb_max = v * 2.55f;
    uint32_t rgb_min = rgb_max * (100 - s) / 100.0f;

    uint32_t i = h / 60;
    uint32_t diff = h % 60;

    // RGB adjustment amount by hue
    uint32_t rgb_adj = (rgb_max - rgb_min) * diff / 60;

    switch (i) {
    case 0:
        *r = rgb_max;
        *g = rgb_min + rgb_adj;
        *b = rgb_min;
        break;
    case 1:
        *r = rgb_max - rgb_adj;
        *g = rgb_max;
        *b = rgb_min;
        break;
    case 2:
        *r = rgb_min;
        *g = rgb_max;
        *b = rgb_min + rgb_adj;
        break;
    case 3:
        *r = rgb_min;
        *g = rgb_max - rgb_adj;
        *b = rgb_max;
        break;
    case 4:
        *r = rgb_min + rgb_adj;
        *g = rgb_min;
        *b = rgb_max;
        break;
    default:
        *r = rgb_max;
        *g = rgb_min;
        *b = rgb_max - rgb_adj;
        break;
    }
}

static void example_ledc_a_init(void)
{
    // Prepare and then apply the LEDC PWM timer configuration
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_MODE,
        .timer_num        = LEDC_TIMER_A,
        .duty_resolution  = LEDC_DUTY_RES,
        .freq_hz          = LEDC_FREQUENCY,  // Set output frequency at 5 kHz
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Prepare and then apply the LEDC PWM channel configuration
    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_MODE,
        .channel        = LEDC_CHANNEL_A,
        .timer_sel      = LEDC_TIMER_A,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LEDC_OUTPUT_IO_A,
        .duty           = 0, // Set duty to 0%
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

static void example_ledc_b_init(void)
{
    // Prepare and then apply the LEDC PWM timer configuration
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_MODE,
        .timer_num        = LEDC_TIMER_B,
        .duty_resolution  = LEDC_DUTY_RES,
        .freq_hz          = LEDC_FREQUENCY,  // Set output frequency at 5 kHz
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Prepare and then apply the LEDC PWM channel configuration
    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_MODE,
        .channel        = LEDC_CHANNEL_B,
        .timer_sel      = LEDC_TIMER_B,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LEDC_OUTPUT_IO_B,
        .duty           = 0, // Set duty to 0%
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

// static void example_ledc_c_init(void)
// {
//     // Prepare and then apply the LEDC PWM timer configuration
//     ledc_timer_config_t ledc_timer = {
//         .speed_mode       = LEDC_MODE,
//         .timer_num        = LEDC_TIMER_C,
//         .duty_resolution  = LEDC_DUTY_RES,
//         .freq_hz          = LEDC_FREQUENCY,  // Set output frequency at 5 kHz
//         .clk_cfg          = LEDC_AUTO_CLK
//     };
//     ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

//     // Prepare and then apply the LEDC PWM channel configuration
//     ledc_channel_config_t ledc_channel = {
//         .speed_mode     = LEDC_MODE,
//         .channel        = LEDC_CHANNEL_C,
//         .timer_sel      = LEDC_TIMER_C,
//         .intr_type      = LEDC_INTR_DISABLE,
//         .gpio_num       = LEDC_OUTPUT_IO_C,
//         .duty           = 0, // Set duty to 0%
//         .hpoint         = 0,
//         .flags = {
//             .output_invert = 0
//         }
//     };
//     ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
// }

// static void example_ledc_d_init(void)
// {
//     // Prepare and then apply the LEDC PWM timer configuration
//     ledc_timer_config_t ledc_timer = {
//         .speed_mode       = LEDC_MODE,
//         .timer_num        = LEDC_TIMER_D,
//         .duty_resolution  = LEDC_DUTY_RES,
//         .freq_hz          = LEDC_FREQUENCY,  // Set output frequency at 5 kHz
//         .clk_cfg          = LEDC_AUTO_CLK
//     };
//     ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

//     // Prepare and then apply the LEDC PWM channel configuration
//     ledc_channel_config_t ledc_channel = {
//         .speed_mode     = LEDC_MODE,
//         .channel        = LEDC_CHANNEL_D,
//         .timer_sel      = LEDC_TIMER_D,
//         .intr_type      = LEDC_INTR_DISABLE,
//         .gpio_num       = LEDC_OUTPUT_IO_D,
//         .duty           = 0, // Set duty to 0%
//         .hpoint         = 0,
//         .flags = {
//             .output_invert = 0
//         }
//     };
//     ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
// }


static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)index_html_gz, index_html_gz_len);
}

static esp_err_t info_handler(httpd_req_t *req)
{
    char *p = json_response;
    *p++ = '{';
    p += sprintf(p, "\"mode\":%d,", lamp_info.mode);
    p += sprintf(p, "\"brightnessA\":%d,", lamp_info.brightnessA);
    p += sprintf(p, "\"brightnessB\":%d,", lamp_info.brightnessB);
    p += sprintf(p, "\"brightness\":%d,", lamp_info.brightness);
    p += sprintf(p, "\"warm\":%d,", lamp_info.warm);
    p += sprintf(p, "\"color\":%d,", lamp_info.color);
    *p++ = '}';
    *p++ = 0;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json_response, strlen(json_response));
}

static esp_err_t cmd_handler(httpd_req_t *req)
{
    char *buf;
    size_t buf_len;
    char variable[32] = {
        0,
    };
    char value[32] = {
        0,
    };

    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1)
    {
        buf = (char *)malloc(buf_len);
        if (!buf)
        {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK)
        {
            if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) == ESP_OK &&
                httpd_query_key_value(buf, "val", value, sizeof(value)) == ESP_OK)
            {
            }
            else
            {
                free(buf);
                httpd_resp_send_404(req);
                return ESP_FAIL;
            }
        }
        else
        {
            free(buf);
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }
        free(buf);
    }
    else
    {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    
    int res = 0;
    ESP_LOGI(TAG, "receive Command: %s(%s)\r\n", variable, value);
    if (!strcmp(variable, "set-brightness-a"))
    {
        int val = atoi(value);
        if(val < 0) val = 0;
        if(val > 100) val = 100;
        // ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, LEDC_MAX_DUTY*val/100));
        // ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));
        ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_A, LEDC_MAX_DUTY*val/100, 1000);
        ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_A, LEDC_FADE_NO_WAIT);
    }
    else if (!strcmp(variable, "set-brightness-b"))
    {
        int val = atoi(value);
        if(val < 0) val = 0;
        if(val > 100) val = 100;
        // ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_B, LEDC_MAX_DUTY*val/100));
        // ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_B));
        ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_B, LEDC_MAX_DUTY*val/100, 1000);
        ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_B, LEDC_FADE_NO_WAIT);
    }
    else if (!strcmp(variable, "set-cmd"))
    {
        const char *delim = ",";
        char *v_mode, *v_a, *v_b;
        v_mode = strtok(value, delim);
        v_a = strtok(NULL, delim);
        v_b = strtok(NULL, delim);
        int val_a = atoi(v_a);
        int val_b = atoi(v_b);
        
        if(val_a < 0) val_a = 0;
        if(val_a > 100) val_a = 100;
        if(val_b < 0) val_b = 0;
        if(val_b > 100) val_b = 100;

        ESP_LOGI(TAG, "set-cmd: (%s,%d,%d)\r\n", v_mode, val_a, val_b);

        if (!strcmp(v_mode, "normal"))
        {
            lamp_info.mode = LAMP_MODE_NORMAL;
            lamp_info.brightnessA = val_a;
            lamp_info.brightnessB = val_b;
            ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_A, LEDC_MAX_DUTY*val_a/100, 1000);
            ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_B, LEDC_MAX_DUTY*val_b/100, 1000);
            ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_C, 0, 1000);
            ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_D, 0, 1000);
            ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_A, LEDC_FADE_NO_WAIT);
            ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_B, LEDC_FADE_NO_WAIT);
            ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_C, LEDC_FADE_NO_WAIT);
            ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_D, LEDC_FADE_NO_WAIT);

            memset(led_strip_pixels, 0, sizeof(led_strip_pixels));
            ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config));
            ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
        }
        if (!strcmp(v_mode, "sleep"))
        {
            lamp_info.mode = LAMP_MODE_SLEEP;
            lamp_info.warm = val_a;
            lamp_info.brightness = val_b;
            // ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_A, 0, 1000);
            // ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_B, 0, 1000);
            // ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_C, LEDC_MAX_DUTY*val_a/100, 1000);
            // ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_D, LEDC_MAX_DUTY*val_b/100, 1000);
            // ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_A, LEDC_FADE_NO_WAIT);
            // ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_B, LEDC_FADE_NO_WAIT);
            // ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_C, LEDC_FADE_NO_WAIT);
            // ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_D, LEDC_FADE_NO_WAIT);

            // memset(led_strip_pixels, 0, sizeof(led_strip_pixels));
            // ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config));
            // ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));

            ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_A, 0, 1000);
            ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_B, 0, 1000);
            ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_C, 0, 1000);
            ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_D, 0, 1000);
            ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_A, LEDC_FADE_NO_WAIT);
            ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_B, LEDC_FADE_NO_WAIT);
            ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_C, LEDC_FADE_NO_WAIT);
            ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_D, LEDC_FADE_NO_WAIT);

            // uint32_t v_max = val_b * 2.55f;
            // uint32_t red = v_max;
            // uint32_t green = v_max;
            // uint32_t blue = v_max * ((100 - val_a) / 100.0f);
            
            uint32_t red = 0;
            uint32_t green = 0;
            uint32_t blue = 0;
            uint16_t hue = 218;
            ESP_LOGI(TAG, "hue: (%d)\r\n", hue);
            led_strip_hsv2rgb(hue, 50 + (val_a / 2.0f), val_b, &red, &green, &blue);
            ESP_LOGI(TAG, "rgb=(%ld,%ld,%ld)\r\n", red, green, blue);
            for(int j = 0; j < EXAMPLE_LED_NUMBERS; j++)
            {
                // led_strip_pixels[j * 3 + 0] = green;
                // led_strip_pixels[j * 3 + 1] = red;
                // led_strip_pixels[j * 3 + 2] = blue;
                led_strip_pixels[j * 3 + 0] = green;
                led_strip_pixels[j * 3 + 1] = blue;
                led_strip_pixels[j * 3 + 2] = red;
            }

            ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config));
            ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
        }
        if (!strcmp(v_mode, "color"))
        {
            lamp_info.mode = LAMP_MODE_COLOR;
            lamp_info.color = val_a;
            lamp_info.brightness = val_b;
            ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_A, 0, 1000);
            ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_B, 0, 1000);
            ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_C, 0, 1000);
            ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_D, 0, 1000);
            ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_A, LEDC_FADE_NO_WAIT);
            ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_B, LEDC_FADE_NO_WAIT);
            ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_C, LEDC_FADE_NO_WAIT);
            ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_D, LEDC_FADE_NO_WAIT);

            uint32_t red = 0;
            uint32_t green = 0;
            uint32_t blue = 0;
            uint16_t hue = (uint16_t) (360.0 / 100 * val_a) + 280;
            ESP_LOGI(TAG, "hue: (%d)\r\n", hue);
            led_strip_hsv2rgb(hue, 100, val_b, &red, &green, &blue);
            ESP_LOGI(TAG, "rgb=(%ld,%ld,%ld)\r\n", red, green, blue);
            for(int j = 0; j < EXAMPLE_LED_NUMBERS; j++)
            {
                led_strip_pixels[j * 3 + 0] = green;
                led_strip_pixels[j * 3 + 1] = blue;
                led_strip_pixels[j * 3 + 2] = red;
            }

            ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, sizeof(led_strip_pixels), &tx_config));
            ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
        }
    }
    else
    {
        res = -1;
    }

    if (res)
    {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static void mc_init_httpd()
{
    ESP_LOGI(TAG, "init HTTP server...");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL};

    httpd_uri_t info_uri = {
        .uri = "/info",
        .method = HTTP_GET,
        .handler = info_handler,
        .user_ctx = NULL};

    httpd_uri_t cmd_uri = {
        .uri = "/control",
        .method = HTTP_GET,
        .handler = cmd_handler,
        .user_ctx = NULL};


    ESP_LOGI(TAG, "Starting web server on port: '%d'\n", config.server_port);
    if (httpd_start(&httpd, &config) == ESP_OK)
    {
        httpd_register_uri_handler(httpd, &index_uri);
        httpd_register_uri_handler(httpd, &info_uri);
        httpd_register_uri_handler(httpd, &cmd_uri);
    }
}

// void example_blink()
// {
//     while(true)
//     {
//         ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_C, LEDC_DUTY, 1000);
//         ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_D, LEDC_DUTY, 1000);
//         ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_C, LEDC_FADE_NO_WAIT);
//         ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_D, LEDC_FADE_NO_WAIT);
//         vTaskDelay(2000 / portTICK_PERIOD_MS);
//         ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_C, 0, 1000);
//         ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_D, 0, 1000);
//         ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_C, LEDC_FADE_NO_WAIT);
//         ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_D, LEDC_FADE_NO_WAIT);
//         vTaskDelay(2000 / portTICK_PERIOD_MS);
//     }
// }

void app_main(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    example_ledc_a_init();
    example_ledc_b_init();
    // example_ledc_c_init();
    // example_ledc_d_init();

    // PWM使用硬件渐变
    ledc_fade_func_install(0);

    ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_A, LEDC_DUTY, 1000);
    ledc_set_fade_with_time(LEDC_MODE, LEDC_CHANNEL_B, LEDC_DUTY, 1000);
    ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_A, LEDC_FADE_NO_WAIT);
    ledc_fade_start(LEDC_MODE, LEDC_CHANNEL_B, LEDC_FADE_NO_WAIT);

    // xTaskCreate(example_blink, "blink-task", 4096, NULL, 1, NULL);
    
    ESP_LOGI(TAG, "Create RMT TX channel");
    
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_chan));

    ESP_LOGI(TAG, "Install led strip encoder");
    
    led_strip_encoder_config_t encoder_config = {
        .resolution = RMT_LED_STRIP_RESOLUTION_HZ,
    };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&encoder_config, &led_encoder));

    ESP_LOGI(TAG, "Enable RMT TX channel");
    ESP_ERROR_CHECK(rmt_enable(led_chan));

    /* Initialize event group */
    s_wifi_event_group = xEventGroupCreate();

    /* Register Event handler */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                    ESP_EVENT_ANY_ID,
                    &wifi_event_handler,
                    NULL,
                    NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                    IP_EVENT_STA_GOT_IP,
                    &wifi_event_handler,
                    NULL,
                    NULL));

    /*Initialize WiFi */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* Initialize AP */
    ESP_LOGI(TAG_AP, "ESP_WIFI_MODE_AP");
    esp_netif_t *esp_netif_ap = wifi_init_softap();

    /* Initialize STA */
    ESP_LOGI(TAG_STA, "ESP_WIFI_MODE_STA");
    esp_netif_t *esp_netif_sta = wifi_init_sta();

    /* Start WiFi */
    ESP_ERROR_CHECK(esp_wifi_start() );

    /*
     * Wait until either the connection is established (WIFI_CONNECTED_BIT) or
     * connection failed for the maximum number of re-tries (WIFI_FAIL_BIT).
     * The bits are set by event_handler() (see above)
     */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned,
     * hence we can test which event actually happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG_STA, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_STA_SSID, EXAMPLE_ESP_WIFI_STA_PASSWD);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG_STA, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_STA_SSID, EXAMPLE_ESP_WIFI_STA_PASSWD);
    } else {
        ESP_LOGE(TAG_STA, "UNEXPECTED EVENT");
        return;
    }

    /* Set sta as the default interface */
    esp_netif_set_default_netif(esp_netif_sta);

    /* Enable napt on the AP netif */
    if (esp_netif_napt_enable(esp_netif_ap) != ESP_OK) {
        ESP_LOGE(TAG_STA, "NAPT not enabled on the netif: %p", esp_netif_ap);
    }

    mc_init_httpd();
}
