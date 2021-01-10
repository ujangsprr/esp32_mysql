#include <stdio.h> 
#include <stdlib.h> 
#include <string.h>
#include "freertos/FreeRTOS.h" 
#include "freertos/task.h" 
#include "freertos/event_groups.h" 
#include "esp_system.h"
#include "esp_wifi.h" 
#include "driver/gpio.h" 
#include "driver/adc.h" 
#include "esp_event_loop.h" 
#include "esp_log.h" 
#include "sdkconfig.h" 
#include "nvs_flash.h" 
#include "esp_http_client.h" 
#include "lwip/err.h" 
#include "esp_sntp.h"
#include "esp_adc_cal.h" 

#define LED_RED 18
#define LED_GREEN 19
#define LED_BLUE 21
#define MAX_HTTP_RECV_BUFFER 512
#define WIFI_SSID "Wifi Rusak" 
#define WIFI_PASS "rusakcok"
#define URL "http://192.168.0.105/monitoring/connect.php"

int val_lm, val_opamp;
float mv_lm, mv_opamp, cel;
char strftime_buf[32], led[6], request[300], device_time[48];

static void initialize_sntp(void); 
static void initialise_wifi(void);
static esp_err_t event_handler(void *ctx, system_event_t *event); 
esp_err_t _http_event_handler(esp_http_client_event_t *evt); 
static void trigger_http_request(const char *url);
static void http_request_task(void *pvParameters); 
static void get_time(void);
static void sensor_process(void);

static EventGroupHandle_t wifi_event_group;

const int CONNECTED_BIT = BIT0; 
static const char *TAG = "esp32";

#if CONFIG_IDF_TARGET_ESP32
static void check_efuse(void)
{
    if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_TP) == ESP_OK) { 
        printf("eFuse Two Point: Supported\n");
    } else {
        printf("eFuse Two Point: NOT supported\n");
    }

    if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_VREF) == ESP_OK) { 
        printf("eFuse Vref: Supported\n");
    } else {
        printf("eFuse Vref: NOT supported\n");
    }
}

static void print_char_val_type(esp_adc_cal_value_t val_type)
{
    if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP) { 
        printf("Characterized using Two Point Value\n");
    } else if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) { 
        printf("Characterized using eFuse Vref\n");
    } else {
        printf("Characterized using Default Vref\n");
    }
}
#endif

static void get_time(void){ 
    setenv("TZ", "UTC", 1);
    time_t now;
    struct tm timeinfo; 
    time(&now);  
    localtime_r(&now, &timeinfo);
    
    if (timeinfo.tm_year < (2016 - 1900)) {
        ESP_LOGI(TAG, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET) { 
        ESP_LOGI(TAG, "Waiting for system time to be set... "); 
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    time(&now);  
    localtime_r(&now, &timeinfo);
        time(&now);
    }
    tzset();
    localtime_r(&now, &timeinfo); 
    time_t t = now + (3600 * 7 ); 
    struct tm timeinfo_utc7; 
    localtime_r(&t , &timeinfo_utc7);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo_utc7 );

    int y = 0;
    for (int i = 0; strftime_buf[i] != NULL; i++){ 
        if (strftime_buf[i] == ' '){
            device_time[y] = '%';
            device_time[y + 1] = '2';
            device_time[y + 2] = '0';
            y += 3; 
        }
        else
        {
            device_time[y] = strftime_buf[i]; 
            y++;
        }
    }
}

static void initialise_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) ); 
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); 
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) ); 
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) ); ESP_ERROR_CHECK( esp_wifi_start() );
}

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP"); 
    sntp_setoperatingmode(SNTP_OPMODE_POLL); 
    sntp_setservername(0, "pool.ntp.org"); 
    sntp_init();
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) { 
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect(); 
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT); 
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        esp_wifi_connect(); xEventGroupClearBits(wifi_event_group, CONNECTED_BIT); 
        break;
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) { 
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s",evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len); 
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // Write out data
                // printf("%.s", evt->data_len, (char)evt->data);
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
    }
    return ESP_OK;
}

static void trigger_http_request(const char *url)
{
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,false, true, portMAX_DELAY); 
    ESP_LOGI(TAG, "Connected to AP");

    esp_http_client_config_t config = {
    .url = url,
    .event_handler = _http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_err_t err = esp_http_client_perform(client); 
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d", esp_http_client_get_status_code(client), esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err)); 
    }
    esp_http_client_cleanup(client);
}

static void http_request_task(void *pvParameters)
{
    while (1) {
        get_time();
        sensor_process();
            
        #if CONFIG_IDF_TARGET_ESP32
        if (cel > 37){
            gpio_set_level(LED_RED, 1);
            gpio_set_level(LED_GREEN, 0);
            gpio_set_level(LED_BLUE, 0);
            strcpy(led, "Red");
        }
        else if (cel >= 30 && cel <= 37){
            gpio_set_level(LED_RED, 0);
            gpio_set_level(LED_GREEN, 1);
            gpio_set_level(LED_BLUE, 0);
            strcpy(led, "Green");
        }
        else if (cel < 30){
            gpio_set_level(LED_RED, 0);
            gpio_set_level(LED_GREEN, 0);
            gpio_set_level(LED_BLUE, 1);
            strcpy(led, "Blue");
        }
        #endif

        sprintf(request,"%s?&ADC=%d&LM741=%.2f&LM35=%.2f&Temperature=%.2f&StatusLED='%s'",URL,val_opamp,mv_opamp,mv_lm,cel,led);
        printf("%s\n", request);
        trigger_http_request(request); 
        vTaskDelay(60000/portTICK_PERIOD_MS);
    }
}

static void sensor_process(void)
{
    val_opamp = adc1_get_raw(ADC1_CHANNEL_0);
    val_lm = val_opamp / 2;
    mv_lm = (val_lm / 4096.0) * 1100;
    mv_opamp = (val_opamp / 4096.0) * 1100;
    cel = mv_lm / 10;

    printf("LM35  -->  Raw : %d   Voltage : %.2f mv\n", val_lm, mv_lm);
    printf("LM741 -->  Raw : %d   Voltage : %.2f mv\n", val_opamp, mv_opamp);
    printf("Temp  -->  %.2f°C \n", cel);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); 
        ret = nvs_flash_init();
    } 
    ESP_ERROR_CHECK(ret);

    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_0,ADC_ATTEN_DB_0);

    gpio_reset_pin(LED_RED);
    gpio_reset_pin(LED_GREEN);
    gpio_reset_pin(LED_BLUE);

    gpio_set_direction(LED_RED, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_GREEN, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_BLUE, GPIO_MODE_OUTPUT);

    initialise_wifi(); 
    initialize_sntp();

    xTaskCreate(&http_request_task, "http_request_task", 10240, NULL, 5, NULL);
}