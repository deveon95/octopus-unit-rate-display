/* Octopus Tracker Tariff Display for ESP32 / ESP-IDF
 *
 * Copyright (c) 2023 Nick Schollar
 * Licenced under MIT Licence
 *
 * Displays today's gas and electricity price for the tariff code specified in menuconfig
 * on two 3-digit common anode 7-segment displays. The tariff code is specific to your region.
 *
 * This program fetches the data from the server using a public API once per hour.
 * A certificate file is needed to connect to the server because HTTPS is mandatory.
 *
 * Wi-Fi SSID and password are specified in menuconfig.
 *
 * Self-diagnostics:
 * No dashes or decimal points: Program not running or displays connected incorrectly
 *   Check display wiring.
 * No dashes (only decimal points): Not connected to wifi
 *   Check SSID and password.
 * One dash on the display: Connected to wifi, time not synchronised yet
 *   Time is taken from HTTP response header, so this suggests unable to connect to internet.
 * Two dashes on the display: Time synchronised, prices not obtained yet
 *   API may have changed. Try tariff URL in a browser.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "time.h"
#include "driver/gpio.h"
#include "driver/timer.h"
#include "driver/adc.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "esp_http_client.h" 
#include "esp_tls.h" 
#include "cJSON.h"

#define SR_DELAY_US 1
#define NUM_OF_ANODES 16
#define ANODES_IN_USE 0b0000000000111111
//#define ANODES_IN_USE 0b0011111100111111

#define pin_segAR 14
#define pin_segBR 21
#define pin_segCR 47
#define pin_segDR 48
#define pin_segER 35
#define pin_segFR 36
#define pin_segGR 37
#define pin_segDPR 38
#define pin_segAL 4
#define pin_segBL 5
#define pin_segCL 6
#define pin_segDL 7
#define pin_segEL 17
#define pin_segFL 18
#define pin_segGL 8
#define pin_segDPL 13

#define pin_SOE 9
#define pin_SLAT 10
#define pin_SDAT 11
#define pin_SCK 12

#define pin_BUTTON2 0
#define pin_BUTTON3 2
#define pin_BUTTON4 3

#define TIMER_DIVIDER         (16)  //  Hardware timer clock divider
#define TIMER_SCALE           ((TIMER_BASE_CLK / 10000)/ TIMER_DIVIDER)  // convert counter value to seconds

typedef struct {
    int timer_group;
    int timer_idx;
    int alarm_interval;
    bool auto_reload;
} timer_info_t;

//spi_device_handle_t spihandle;

// Set to True if the time was updated successfully
bool timeSet = false;
bool wifi_connected = false;
// Need to change this to separate ones for gas and elec
bool got_gas_unit_rate = false;
bool got_elec_unit_rate = false;
bool got_gas_tomorrow_unit_rate = false;
bool got_elec_tomorrow_unit_rate = false;
bool got_gas_flex_unit_rate = false;
bool got_elec_flex_unit_rate = false;
bool got_elec_agile_unit_rate = false;
#define TARIFF_TYPE_TRACKER 0
#define TARIFF_TYPE_FLEXIBLE 1
#define TARIFF_TYPE_AGILE 2
#define TARIFF_TYPE_TRACKER_TOMORROW 3

double gas_unit_rate = 0.0;
double elec_unit_rate = 0.0;
double gas_tomorrow_unit_rate = 0.0;
double elec_tomorrow_unit_rate = 0.0;
double gas_flex_unit_rate = 0.0;
double elec_flex_unit_rate = 0.0;
double elec_agile_rates[48];
uint64_t elec_agile_validity = 0;
uint8_t agile_time = 0;

#define FETCHER_WDOG_LIMIT_IN_SECONDS (60*15)

#define NUMBER_OF_BRIGHTNESS_SETTINGS 4
#define BRIGHTNESS_HYSTERESIS 100
// brightness of the display (0 to 3)
uint8_t display_brightness = 3;

#define ADC_FILTER_LENGTH 30
#define ADC_MAX_VALUE 4095
#define ADC_HYSTERESIS 200
static const adc_channel_t channel = ADC_CHANNEL_0;     //GPIO1 for ADC1
static const adc_bits_width_t width = ADC_WIDTH_BIT_12;
static const adc_atten_t atten = ADC_ATTEN_DB_11;
//static const adc_unit_t unit = ADC_UNIT_1;


/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "JSON";
static const char *TAG_FW = "FWD";
static const char *TAG_ADC = "ADC";

static int s_retry_num = 0;

/* Root cert for octopus.energy.com, taken from octopus_energy_root_cert.pem

	 The PEM file was extracted from the output of this command:
	 openssl s_client -showcerts -connect octopus.energy:443 </dev/null >hoge

	 The CA root cert is the last cert given in the chain of certs.

	 To embed it in the app binary, the PEM file is named
	 in the component.mk COMPONENT_EMBED_TXTFILES variable.
*/
extern const char octopus_energy_root_cert_pem_start[] asm("_binary_octopus_energy_root_cert_pem_start");
//extern const char octopus_energy_root_cert_pem_end[]	asm("_binary_octopus_energy_root_cert_pem_end");


esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    struct tm time_struct;
    struct timeval timeval_struct;
    
	static char *output_buffer;  // Buffer to store response of http request from event handler
	static int output_len;		 // Stores number of bytes read
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
			ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            if (strcmp(evt->header_key, "Date") == 0)
            {
                ESP_LOGI(TAG, "Date header found: %s", evt->header_value);
                strptime(evt->header_value, "%a, %d %b %Y %H:%M:%S %Z", &time_struct);
                ESP_LOGI(TAG, "Time struct written: %d-%d-%d %d:%d:%d", time_struct.tm_year, time_struct.tm_mon, time_struct.tm_mday, time_struct.tm_hour, time_struct.tm_min, time_struct.tm_sec);
                timeval_struct.tv_sec = mktime(&time_struct);
                timeval_struct.tv_usec = 0;
                // tv_sec becomes -1 if mktime failed to convert the time struct into a time value
                if (timeval_struct.tv_sec > 0)
                {
                    timeSet = true;
                    ESP_LOGI(TAG, "RTC Seconds Since Epoch: %ld", timeval_struct.tv_sec);
                    ESP_LOGI(TAG, "RTC set, returned: %d", settimeofday(&timeval_struct, NULL));
                }
            }
			break;
		case HTTP_EVENT_ON_DATA:
			ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
			//ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, content_length=%d", esp_http_client_get_content_length(evt->client));
			ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, output_len=%d", output_len);
			// If user_data buffer is configured, copy the response into the buffer
			if (evt->user_data) {
				memcpy(evt->user_data + output_len, evt->data, evt->data_len);
			} else {
				if (output_buffer == NULL && esp_http_client_get_content_length(evt->client) > 0) {
					output_buffer = (char *) malloc(esp_http_client_get_content_length(evt->client));
					output_len = 0;
					if (output_buffer == NULL) {
						ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
						return ESP_FAIL;
					}
				}
				memcpy(output_buffer + output_len, evt->data, evt->data_len);
			}
			output_len += evt->data_len;
			break;
		case HTTP_EVENT_ON_FINISH:
			ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
			if (output_buffer != NULL) {
				// Response is accumulated in output_buffer. Uncomment the below line to print the accumulated response
				// ESP_LOG_BUFFER_HEX(TAG, output_buffer, output_len);
				free(output_buffer);
				output_buffer = NULL;
			}
			output_len = 0;
			break;
		case HTTP_EVENT_DISCONNECTED:
			ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
			int mbedtls_err = 0;
			esp_err_t err = esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);
			if (err != 0) {
				if (output_buffer != NULL) {
					free(output_buffer);
					output_buffer = NULL;
				}
				output_len = 0;
				ESP_LOGE(TAG, "Last esp error code: 0x%x", err);
				ESP_LOGE(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
			}
			break;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
		case HTTP_EVENT_REDIRECT:
			ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
			break;
#endif
	}
	return ESP_OK;
}

static void event_handler(void* arg, esp_event_base_t event_base,
																int32_t event_id, void* event_data)
{
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		if (s_retry_num < CONFIG_ESP_MAXIMUM_RETRY) {
				esp_wifi_connect();
				s_retry_num++;
				ESP_LOGI(TAG, "retry to connect to the AP");
		} else {
				xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
		}
		ESP_LOGI(TAG,"connect to the AP fail");
        wifi_connected = false;
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
		ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
		s_retry_num = 0;
		xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_connected = true;
	}
}

void wifi_init_sta(void)
{
	s_wifi_event_group = xEventGroupCreate();

	ESP_ERROR_CHECK(esp_netif_init());

	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	esp_event_handler_instance_t instance_any_id;
	esp_event_handler_instance_t instance_got_ip;
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
									ESP_EVENT_ANY_ID,
									&event_handler,
									NULL,
									&instance_any_id));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
									IP_EVENT_STA_GOT_IP,
									&event_handler,
									NULL,
									&instance_got_ip));

	wifi_config_t wifi_config = {
		.sta = {
			.ssid = CONFIG_ESP_WIFI_SSID,
			.password = CONFIG_ESP_WIFI_PASSWORD,
			/* Setting a password implies station will connect to all security modes including WEP/WPA.
			 * However these modes are deprecated and not advisable to be used. Incase your Access point
			 * doesn't support WPA2, these mode can be enabled by commenting below line */
			.threshold.authmode = WIFI_AUTH_WPA2_PSK,

			.pmf_cfg = {
				.capable = true,
				.required = false
			},
		},
	};
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
	ESP_ERROR_CHECK(esp_wifi_start() );

	ESP_LOGI(TAG, "wifi_init_sta finished.");

	/* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
	 * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
	EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
		WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
		pdFALSE,
		pdFALSE,
		portMAX_DELAY);

	/* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
	 * happened. */
	if (bits & WIFI_CONNECTED_BIT) {
		ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", CONFIG_ESP_WIFI_SSID, CONFIG_ESP_WIFI_PASSWORD);
	} else if (bits & WIFI_FAIL_BIT) {
		ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s", CONFIG_ESP_WIFI_SSID, CONFIG_ESP_WIFI_PASSWORD);
	} else {
		ESP_LOGE(TAG, "UNEXPECTED EVENT");
	}

	/* The event will not be processed after unregister */
	ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
	ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
	vEventGroupDelete(s_wifi_event_group);
}

char *JSON_Types(int type) {
	if (type == cJSON_Invalid) return ("cJSON_Invalid");
	if (type == cJSON_False) return ("cJSON_False");
	if (type == cJSON_True) return ("cJSON_True");
	if (type == cJSON_NULL) return ("cJSON_NULL");
	if (type == cJSON_Number) return ("cJSON_Number");
	if (type == cJSON_String) return ("cJSON_String");
	if (type == cJSON_Array) return ("cJSON_Array");
	if (type == cJSON_Object) return ("cJSON_Object");
	if (type == cJSON_Raw) return ("cJSON_Raw");
	return NULL;
}


void JSON_Analyze(const cJSON * const root) {
	//ESP_LOGI(TAG, "root->type=%s", JSON_Types(root->type));
	cJSON *current_element = NULL;
	//ESP_LOGI(TAG, "roo->child=%p", root->child);
	//ESP_LOGI(TAG, "roo->next =%p", root->next);
	cJSON_ArrayForEach(current_element, root) {
		//ESP_LOGI(TAG, "type=%s", JSON_Types(current_element->type));
		//ESP_LOGI(TAG, "current_element->string=%p", current_element->string);
		if (current_element->string) {
			const char* string = current_element->string;
			ESP_LOGI(TAG, "[%s]", string);
		}
		if (cJSON_IsInvalid(current_element)) {
			ESP_LOGI(TAG, "Invalid");
		} else if (cJSON_IsFalse(current_element)) {
			ESP_LOGI(TAG, "False");
		} else if (cJSON_IsTrue(current_element)) {
			ESP_LOGI(TAG, "True");
		} else if (cJSON_IsNull(current_element)) {
			ESP_LOGI(TAG, "Null");
		} else if (cJSON_IsNumber(current_element)) {
			int valueint = current_element->valueint;
			double valuedouble = current_element->valuedouble;
			ESP_LOGI(TAG, "int=%d double=%f", valueint, valuedouble);
		} else if (cJSON_IsString(current_element)) {
			const char* valuestring = current_element->valuestring;
			ESP_LOGI(TAG, "%s", valuestring);
		} else if (cJSON_IsArray(current_element)) {
			//ESP_LOGI(TAG, "Array");
			JSON_Analyze(current_element);
		} else if (cJSON_IsObject(current_element)) {
			//ESP_LOGI(TAG, "Object");
			JSON_Analyze(current_element);
		} else if (cJSON_IsRaw(current_element)) {
			ESP_LOGI(TAG, "Raw(Not support)");
		}
	}
}

size_t http_client_content_length(char * url)
{
	ESP_LOGI(TAG, "http_client_content_length url=%s",url);
	size_t content_length;
	
	esp_http_client_config_t config = {
		.url = url,
		.event_handler = _http_event_handler,
		//.user_data = local_response_buffer,			 // Pass address of local buffer to get response
		.cert_pem = octopus_energy_root_cert_pem_start,
	};
	esp_http_client_handle_t client = esp_http_client_init(&config);

	// GET
	esp_err_t err = esp_http_client_perform(client);
	if (err == ESP_OK) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
		ESP_LOGD(TAG, "HTTP GET Status = %d, content_length = %lld",
#else
		ESP_LOGD(TAG, "HTTP GET Status = %d, content_length = %d",
#endif
				esp_http_client_get_status_code(client),
				esp_http_client_get_content_length(client));
		content_length = esp_http_client_get_content_length(client);

	} else {
		ESP_LOGW(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
		content_length = 0;
	}
	esp_http_client_cleanup(client);
	return content_length;
}

esp_err_t http_client_content_get(char * url, char * response_buffer)
{
	ESP_LOGI(TAG, "http_client_content_get url=%s",url);

	esp_http_client_config_t config = {
		.url = url,
		.event_handler = _http_event_handler,
		.user_data = response_buffer,			 // Pass address of local buffer to get response
		.cert_pem = octopus_energy_root_cert_pem_start,
	};
	esp_http_client_handle_t client = esp_http_client_init(&config);
    
    // err = esp_http_client_fetch_headers(client);
    //ESP_LOGI(TAG, "HTTP Client Fetch Headers returned %d", err);

	// GET
	esp_err_t err = esp_http_client_perform(client);
	if (err == ESP_OK) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
		ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %lld",
#else
		ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d",
#endif
				esp_http_client_get_status_code(client),
				esp_http_client_get_content_length(client));
		ESP_LOGD(TAG, "\n%s", response_buffer);
	} else {
		ESP_LOGW(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
	}
    
    
	esp_http_client_cleanup(client);
	return err;
}

// Convert a date string into a struct tm
void date_string_to_struct_tm(const char *s, struct tm *time_struct)
{
     sscanf(
        s,
        "%d-%d-%dT%d-%d-%dZ",
        &time_struct->tm_year,
        &time_struct->tm_mon,
        &time_struct->tm_mday,
        &time_struct->tm_hour,
        &time_struct->tm_min,
        &time_struct->tm_sec
    );
    time_struct->tm_year -= 1900;
    time_struct->tm_mon--;
}

// Parse the JSON structure and return the unit rate for the specified date
void parse_object(cJSON *root, time_t time_now, uint8_t tariff_type, double * agile_rates_ref, uint64_t * agile_validity_ref, bool * got_unit_rate_today, double * unit_rate_today, bool * got_unit_rate_tomorrow, double * unit_rate_tomorrow)
{
    double price = 0.0;
    double price_tomorrow = 0.0;
    uint8_t agile_hour = 0;
    cJSON* json_date = NULL;
    cJSON* unit_rate = NULL;
    cJSON* payment_method = NULL;
    cJSON* expiry = NULL;
    
    // Variables for converting date and time from JSON entries
    struct tm entry_date_time_struct;
    time_t entry_date_time = 0;
    // Initialise entry_date_time_struct (very important)
    gmtime_r(&entry_date_time, &entry_date_time_struct);
    
    int i;
    // Convert time_now into a date string
    char time_string[11];
    struct tm time_now_struct;
    // Generate data for time_now_struct from time_now
    gmtime_r(&time_now, &time_now_struct);
    // Generate string for current date from time_now_struct
    strftime(time_string, (sizeof(time_string) / sizeof(char)), "%Y-%m-%d", &time_now_struct);
    // Calculate tomorrow's date too
    time_t time_tomorrow = time_now + 86400;
    // Convert time_tomorrow into a date string
    char time_tomorrow_string[11];
    struct tm time_tomorrow_struct;
    // Generate data for time_tomorrow_struct from time_tomorrow
    gmtime_r(&time_tomorrow, &time_tomorrow_struct);
    // Generate string for tomorrow#s date from time_tomorrow_struct
    strftime(time_tomorrow_string, (sizeof(time_tomorrow_string) / sizeof(char)), "%Y-%m-%d", &time_tomorrow_struct);
    ESP_LOGI(TAG, "date now: %s epoch: %ld", time_string, time_now);
    
    if (tariff_type == TARIFF_TYPE_TRACKER)
    {
        cJSON *item = cJSON_GetObjectItem(root,"results");
        if (item == NULL)
        {
            ESP_LOGE(TAG, "item pointer is NULL");
        }
        else
        {
            ESP_LOGI(TAG, "Array size: %d", cJSON_GetArraySize(item));
            for (i = 0 ; i < cJSON_GetArraySize(item) ; i++)
            {
                cJSON * subitem = cJSON_GetArrayItem(item, i);
                json_date = cJSON_GetObjectItem(subitem, "valid_from");
                unit_rate = cJSON_GetObjectItem(subitem, "value_inc_vat");
                // Convert json_date into struct tm
                date_string_to_struct_tm(json_date->valuestring, &entry_date_time_struct);
                entry_date_time = mktime(&entry_date_time_struct);
                // Print date values
                ESP_LOGI(TAG, "date: %s epoch: %ld unit rate: %f", json_date->valuestring, entry_date_time, unit_rate->valuedouble);
                // Check if the current array entry matches the specified date
                if ((entry_date_time > (time_now - (time_t)86400)) && (entry_date_time <= time_now))
                {
                    price = unit_rate->valuedouble;                        
                    // Check for null pointer then set
                    if (got_unit_rate_today)
                        *got_unit_rate_today = true;
                }
                else if ((entry_date_time > time_now) && (entry_date_time <= (time_now + (time_t)86400)))
                {
                    price_tomorrow = unit_rate->valuedouble;
                    // Check for null pointer then set
                    if (got_unit_rate_tomorrow)
                        *got_unit_rate_tomorrow = true;
                }
            }
        }
    }
    else if (tariff_type == TARIFF_TYPE_FLEXIBLE)
    {
        cJSON *item = cJSON_GetObjectItem(root,"results");
        if (item == NULL)
        {
            ESP_LOGE(TAG, "item pointer is NULL");
        }
        else
        {
            ESP_LOGI(TAG, "Array size: %d", cJSON_GetArraySize(item));
            for (i = 0 ; i < cJSON_GetArraySize(item) ; i++)
            {
                cJSON * subitem = cJSON_GetArrayItem(item, i);
                json_date = cJSON_GetObjectItem(subitem, "valid_from");
                ESP_LOGI(TAG, "valid_from: %s", json_date->valuestring);
                
                // Get 'valid to' date string
                expiry = cJSON_GetObjectItem(subitem, "valid_to");
                
                // Convert json_date into struct tm
                date_string_to_struct_tm(json_date->valuestring, &entry_date_time_struct);
                entry_date_time = mktime(&entry_date_time_struct);
                
                unit_rate = cJSON_GetObjectItem(subitem, "value_inc_vat");
                payment_method = cJSON_GetObjectItem(subitem, "payment_method");
                if (cJSON_IsNull(expiry))
                {
                    ESP_LOGI(TAG, "from: %s to: null unit rate: %f payment method: %s", json_date->valuestring, unit_rate->valuedouble, payment_method->valuestring);
                }
                else
                {
                   ESP_LOGI(TAG, "from: %s to: %s unit rate: %f payment method: %s", json_date->valuestring, expiry->valuestring, unit_rate->valuedouble, payment_method->valuestring);
                }
                
                // Check if the current array entry is in the valid date range.
                // Convert 'valid from' date to epoch and compare with now.
                // Just going to assume that the newest entry is always first, then
                // no need to also test the expiry date
                if (time_now >= entry_date_time && strcmp(payment_method->valuestring, "DIRECT_DEBIT") == 0)
                {
                    price = unit_rate->valuedouble;
                    // Check for null pointer then set
                    if (got_unit_rate_today)
                        *got_unit_rate_today = true;
                    break;
                }
            }
        }
    }
    else if (tariff_type == TARIFF_TYPE_AGILE)
    {
        *agile_validity_ref = 0;
        cJSON *item = cJSON_GetObjectItem(root,"results");
        if (item == NULL)
        {
            ESP_LOGE(TAG, "item pointer is NULL");
        }
        else
        {
            ESP_LOGI(TAG, "Array size: %d", cJSON_GetArraySize(item));
            for (i = 0 ; i < cJSON_GetArraySize(item) ; i++)
            {
                cJSON * subitem = cJSON_GetArrayItem(item, i);
                json_date = cJSON_GetObjectItem(subitem, "valid_from");
                unit_rate = cJSON_GetObjectItem(subitem, "value_inc_vat");
                ESP_LOGI(TAG, "from: %s unit rate: %f", json_date->valuestring, unit_rate->valuedouble);
                
                // Parse the valid-from string (json_date->valuestring)
                // Format is YYYY-MM-DDTHH:MM:SSZ
                // Check that the date part matches today's date, by comparing first 10 characters
                // with today's date string, because yesterday/tomorrow could be there as well
                if (strncmp(json_date->valuestring, time_string, 10) == 0)
                {
                    price = unit_rate->valuedouble;
                    // Get hour for current entry from valid-from string
                    agile_hour = (json_date->valuestring[11] - 48 ) * 10 + (json_date->valuestring[12] - 48);
                    // Put the prices into the array
                    // 00:00 agile_rates_ref[0]
                    // 00:30 agile_rates_ref[1]
                    // 01:00 agile_rates_ref[2]
                    // and so on
                    if (agile_hour <= 23)
                    {
                        if (json_date->valuestring[14] == '0')
                        {
                            agile_rates_ref[agile_hour * 2] = price;
                            *agile_validity_ref |= ((uint64_t)1 << (agile_hour * 2));
                        }
                        else if (json_date->valuestring[14] == '3')
                        {
                            agile_rates_ref[agile_hour * 2 + 1] = price;
                            *agile_validity_ref |= ((uint64_t)1 << (agile_hour * 2 + 1));
                        }
                    }
                }
            }
            // Check for null pointer then set
            if (got_unit_rate_today)
                *got_unit_rate_today = true;
        }
    }
    if (unit_rate_today)
        *unit_rate_today = price;
    if (unit_rate_tomorrow)
        *unit_rate_tomorrow = price_tomorrow;
}

void http_client(char * url, uint8_t tariff_type, double * agile_rates_ref, uint64_t * agile_validity_ref, bool * got_unit_rate, double * unit_rate, bool * got_tracker_tomorrow_rate, double * tracker_tomorrow_rate)
{
    double unit_rate_local = 0.0;
    bool got_unit_rate_local = 0;
    double tracker_tomorrow_rate_local = 0.0;
    bool got_tracker_tomorrow_rate_local = 0;
	// Get content length from event handler
	size_t content_length;
	while (1) {
		content_length = http_client_content_length(url);
		ESP_LOGI(TAG, "content_length=%d", content_length);
		if (content_length > 0) break;
		vTaskDelay(100);
	}

	// Allocate buffer to store response of http request from event handler
	char *response_buffer;
	response_buffer = (char *) malloc(content_length+1);
	if (response_buffer == NULL) {
		ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
		while(1) {
			vTaskDelay(1);
		}
	}
	bzero(response_buffer, content_length+1);
    ESP_LOGD(TAG, "Memory allocated");

	// Get content from event handler
	while(1) {
		esp_err_t err = http_client_content_get(url, response_buffer);
		if (err == ESP_OK) break;
		vTaskDelay(100);
	}
	ESP_LOGD(TAG, "content_length=%d", content_length);
	ESP_LOGD(TAG, "\n[%s]", response_buffer);

    if (!timeSet)
    {
        ESP_LOGE(TAG, "Time was not set, so it will not be possible to get price for today");
    }
    else
    {
        time_t time_now;
        char time_string[11];
        time_now = time(NULL);
        strftime(time_string, (sizeof(time_string) / sizeof(char)), "%Y-%m-%d", gmtime(&time_now));
        ESP_LOGI(TAG, "Date string: %s", time_string);
    
        // Deserialize JSON
        ESP_LOGI(TAG, "Deserialize.....");
        cJSON *root = cJSON_Parse(response_buffer);
        
        
        ESP_LOGI(TAG, "Attempt parse.....");
        // Parse the JSON in 'root'
        parse_object(root, time_now, tariff_type, agile_rates_ref, agile_validity_ref, &got_unit_rate_local, &unit_rate_local, &got_tracker_tomorrow_rate_local, &tracker_tomorrow_rate_local);
        ESP_LOGI(TAG, "price returned: %f", unit_rate_local);
        
        if (tariff_type == TARIFF_TYPE_TRACKER)
        {
            ESP_LOGI(TAG, "price returned for tomorrow: %f", tracker_tomorrow_rate_local);
            if (tracker_tomorrow_rate)
                *tracker_tomorrow_rate = tracker_tomorrow_rate_local;
            if (tracker_tomorrow_rate)
                *got_tracker_tomorrow_rate = got_tracker_tomorrow_rate_local;
        }
        
        if (tariff_type == TARIFF_TYPE_AGILE)
        {
            for (uint8_t i = 0; i < 48; i++)
            {
                ESP_LOGI(TAG, "Agile price entry %d: %f", i, agile_rates_ref[i]);
            }
            ESP_LOGI(TAG, "Agile validity: %llX", *agile_validity_ref);
        }
        
        cJSON_Delete(root);
        free(response_buffer);
    }
    if (unit_rate)
        *unit_rate = unit_rate_local;
    if (got_unit_rate)
        *got_unit_rate = got_unit_rate_local;
}

// Task for testing the display task - disable get_unit_rates_task and enable this one to test extreme values
void test_task(void * pvParameters)
{
    ESP_LOGI(TAG, "Test task started");
    vTaskDelay(2000 / portTICK_RATE_MS);
    ESP_LOGI(TAG, "Test task: Wifi connected");
    wifi_connected = true;
    vTaskDelay(2000 / portTICK_RATE_MS);
    ESP_LOGI(TAG, "Test task: Time set");
    timeSet = true;
    vTaskDelay(2000 / portTICK_RATE_MS);
    
    gas_unit_rate = 2.73;
    got_gas_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Gas unit rate set %f", gas_unit_rate);
    vTaskDelay(2000 / portTICK_RATE_MS);
    
    elec_unit_rate = 16.5;
    got_elec_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Elec unit rate set %f", elec_unit_rate);
    vTaskDelay(2000 / portTICK_RATE_MS);
    
    ESP_LOGI(TAG, "Test task: Unit rates unset");
    got_gas_unit_rate = true;
    got_elec_unit_rate = true;
    vTaskDelay(2000 / portTICK_RATE_MS);
    
    gas_unit_rate = 0.0;
    got_gas_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Gas unit rate set %f", gas_unit_rate);
    
    elec_unit_rate = -10000.1;
    got_elec_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Elec unit rate set %f", elec_unit_rate);
    vTaskDelay(2000 / portTICK_RATE_MS);
    
    gas_unit_rate = 0.1;
    got_gas_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Gas unit rate set %f", gas_unit_rate);
    
    elec_unit_rate = -10000.0;
    got_elec_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Elec unit rate set %f", elec_unit_rate);
    vTaskDelay(2000 / portTICK_RATE_MS);
    
    gas_unit_rate = 9.9;
    got_gas_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Gas unit rate set %f", gas_unit_rate);
    
    elec_unit_rate = -9999.9;
    got_elec_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Elec unit rate set %f", elec_unit_rate);
    vTaskDelay(2000 / portTICK_RATE_MS);
    
    gas_unit_rate = 10.0;
    got_gas_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Gas unit rate set %f", gas_unit_rate);
    
    elec_unit_rate = -1000.1;
    got_elec_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Elec unit rate set %f", elec_unit_rate);
    vTaskDelay(2000 / portTICK_RATE_MS);
    
    gas_unit_rate = 10.1;
    got_gas_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Gas unit rate set %f", gas_unit_rate);
    
    elec_unit_rate = -1000.0;
    got_elec_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Elec unit rate set %f", elec_unit_rate);
    vTaskDelay(2000 / portTICK_RATE_MS);
    
    gas_unit_rate = 99.9;
    got_gas_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Gas unit rate set %f", gas_unit_rate);
    
    elec_unit_rate = -999.9;
    got_elec_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Elec unit rate set %f", elec_unit_rate);
    vTaskDelay(2000 / portTICK_RATE_MS);
    
    gas_unit_rate = 100.0;
    got_gas_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Gas unit rate set %f", gas_unit_rate);
    
    elec_unit_rate = -100.1;
    got_elec_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Elec unit rate set %f", elec_unit_rate);
    vTaskDelay(2000 / portTICK_RATE_MS);
    
    gas_unit_rate = 100.1;
    got_gas_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Gas unit rate set %f", gas_unit_rate);
    
    elec_unit_rate = -100.0;
    got_elec_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Elec unit rate set %f", elec_unit_rate);
    vTaskDelay(2000 / portTICK_RATE_MS);
    
    gas_unit_rate = 999.9;
    got_gas_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Gas unit rate set %f", gas_unit_rate);
    
    elec_unit_rate = -99.9;
    got_elec_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Elec unit rate set %f", elec_unit_rate);
    vTaskDelay(2000 / portTICK_RATE_MS);
    
    gas_unit_rate = 1000.0;
    got_gas_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Gas unit rate set %f", gas_unit_rate);
    
    elec_unit_rate = -10.1;
    got_elec_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Elec unit rate set %f", elec_unit_rate);
    vTaskDelay(2000 / portTICK_RATE_MS);
    
    gas_unit_rate = 1000.1;
    got_gas_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Gas unit rate set %f", gas_unit_rate);
    
    elec_unit_rate = -10;
    got_elec_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Elec unit rate set %f", elec_unit_rate);
    vTaskDelay(2000 / portTICK_RATE_MS);
    
    gas_unit_rate = 9999.9;
    got_gas_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Gas unit rate set %f", gas_unit_rate);
    
    elec_unit_rate = -9.9;
    got_elec_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Elec unit rate set %f", elec_unit_rate);
    vTaskDelay(2000 / portTICK_RATE_MS);
    
    gas_unit_rate = 10000.0;
    got_gas_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Gas unit rate set %f", gas_unit_rate);
    
    elec_unit_rate = -0.1;
    got_elec_unit_rate = true;
    ESP_LOGI(TAG, "Test task: Elec unit rate set %f", elec_unit_rate);
    vTaskDelay(2000 / portTICK_RATE_MS);
}

// Task for connecting to wifi and getting unit rates
void get_unit_rates_task(void * pvParameters)
{
    ESP_LOGI(TAG, "starting get_unit_rates on core %d", xPortGetCoreID());
    time_t time_now;
    uint8_t hour_last;
    uint8_t day_last;
    struct tm time_struct;
    char url[255];
    
    while(1)
    {
        // Start wifi connection if not already connected
        if (!wifi_connected)
        {
            ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
            wifi_init_sta();
        }

        // Tracker tariff
        // Get tariff information
        // Print tariff names in debug console
        ESP_LOGI(TAG, "Elec tariff=%s",CONFIG_ESP_TARIFF_ELEC);
        ESP_LOGI(TAG, "Gas tariff=%s",CONFIG_ESP_TARIFF_GAS);
        
        if (!got_elec_unit_rate)
        {
            // Generate url for elec tariff api
            sprintf(url, "https://api.octopus.energy/v1/products/%s/electricity-tariffs/%s/standard-unit-rates/", CONFIG_ESP_TARIFF, CONFIG_ESP_TARIFF_ELEC);
            ESP_LOGI(TAG, "url=%s",url);
            // Do HTTP request and parse
            http_client(url, TARIFF_TYPE_TRACKER, NULL, NULL, &got_elec_unit_rate, &elec_unit_rate, &got_elec_tomorrow_unit_rate, &elec_tomorrow_unit_rate); 
        }
        
        if (!got_gas_unit_rate)
        {
            // Generate url for gas tariff api
            sprintf(url, "https://api.octopus.energy/v1/products/%s/gas-tariffs/%s/standard-unit-rates/", CONFIG_ESP_TARIFF, CONFIG_ESP_TARIFF_GAS);
            ESP_LOGI(TAG, "url=%s",url);
            // Do HTTP request and parse
            http_client(url, TARIFF_TYPE_TRACKER, NULL, NULL, &got_gas_unit_rate, &gas_unit_rate, &got_gas_tomorrow_unit_rate, &gas_tomorrow_unit_rate);
        }
        
        // Flexible tariff
        if (CONFIG_ESP_TARIFF_FLEX_ENABLE)
        {
            // Get tariff information
            // Print tariff names in debug console
            ESP_LOGI(TAG, "Elec tariff=%s",CONFIG_ESP_TARIFF_ELEC_FLEX);
            ESP_LOGI(TAG, "Gas tariff=%s",CONFIG_ESP_TARIFF_GAS_FLEX);
            
            if (!got_elec_flex_unit_rate)
            {
                // Generate url for elec tariff api
                sprintf(url, "https://api.octopus.energy/v1/products/%s/electricity-tariffs/%s/standard-unit-rates/", CONFIG_ESP_TARIFF_FLEX, CONFIG_ESP_TARIFF_ELEC_FLEX);
                ESP_LOGI(TAG, "url=%s",url);
                // Do HTTP request and parse
                http_client(url, TARIFF_TYPE_FLEXIBLE, NULL, NULL, &got_elec_flex_unit_rate, &elec_flex_unit_rate, NULL, NULL); 
            }
            
            if (!got_gas_flex_unit_rate)
            {
                // Generate url for gas tariff api
                sprintf(url, "https://api.octopus.energy/v1/products/%s/gas-tariffs/%s/standard-unit-rates/", CONFIG_ESP_TARIFF_FLEX, CONFIG_ESP_TARIFF_GAS_FLEX);
                ESP_LOGI(TAG, "url=%s",url);
                // Do HTTP request and parse
                http_client(url, TARIFF_TYPE_FLEXIBLE, NULL, NULL, &got_gas_flex_unit_rate, &gas_flex_unit_rate, NULL, NULL);
            }
        }
        
        // Agile tariff
        if (CONFIG_ESP_TARIFF_AGILE_ENABLE && !got_elec_agile_unit_rate)
        {
            // Get tariff information
            // Print tariff names in debug console
            ESP_LOGI(TAG, "Elec tariff=%s",CONFIG_ESP_TARIFF_ELEC_AGILE);
            // Generate url for elec tariff api
            sprintf(url, "https://api.octopus.energy/v1/products/%s/electricity-tariffs/%s/standard-unit-rates/", CONFIG_ESP_TARIFF_AGILE, CONFIG_ESP_TARIFF_ELEC_AGILE);
            ESP_LOGI(TAG, "url=%s",url);
            // Do HTTP request and parse
            http_client(url, TARIFF_TYPE_AGILE, elec_agile_rates, &elec_agile_validity, &got_elec_agile_unit_rate, NULL, NULL, NULL); 
        }
        
        
        ESP_LOGI(TAG, "Reached the end");
        // Get time (comment out first line for testing to make it detect a change in time every time)
        time_now = time(NULL);
        gmtime_r(&time_now, &time_struct);
        hour_last = time_struct.tm_hour;
        day_last = time_struct.tm_mday;
        ESP_LOGI(TAG, "hour_last set to %d", hour_last);
        ESP_LOGI(TAG, "day_last set to %d", day_last);
        agile_time = (time_struct.tm_hour * 2) + (time_struct.tm_min / 30);
    
        while(1)
        {
            vTaskDelay(10000 / portTICK_RATE_MS);
            // Check for change in current hour and repeat http_client in that case
            // Check every hour even though rates should only change once a day because
            // sometimes the day's rates are not available until several hours into the day.
            time_now = time(NULL);
            gmtime_r(&time_now, &time_struct);
            agile_time = (time_struct.tm_hour * 2) + (time_struct.tm_min / 30);
            if (time_struct.tm_hour != hour_last)
            {
                // Move the got_x_rate statements here to always refresh prices hourly instead.
                
                ESP_LOGI(TAG, "time_struct.tm_hour %d differs from hour_last %d", time_struct.tm_hour, hour_last);
                // Check for change in current day and update prices daily
                if (time_struct.tm_mday != day_last)
                {
                    got_gas_unit_rate = false;
                    got_elec_unit_rate = false;
                    got_gas_flex_unit_rate = false;
                    got_elec_flex_unit_rate = false;
                    got_elec_agile_unit_rate = false;
                }
                // New API should never return incorrect prices when the new price is not
                // yet available, but tomorrow's price for the tracker doesn't appear until
                // some time later in the day so we need to check hourly until those appear.
                if ((got_gas_tomorrow_unit_rate == false) || (got_elec_tomorrow_unit_rate == false))
                {
                    got_gas_unit_rate = false;
                    got_elec_unit_rate = false;
                }
                // The agile prices for the last hour of the day are not always available
                // so refresh the prices if the agile prices for the new hour are not valid
                if (((elec_agile_validity >> (time_struct.tm_hour * 2)) & 0b11) != 0b11)
                {
                    got_elec_agile_unit_rate = false;
                }
                break;
            }
        }
    }
}

void get_display_digits(double value, uint8_t * display_digits, uint32_t * dp_pos)
{
    int32_t value_int = (int32_t)(value * 100.0);
    if (value_int >= 100000)        // >= 1000.00 out of range
    {
        display_digits[0] = 0x0A;
        display_digits[1] = 1;
        display_digits[2] = 0x0A;
        *dp_pos = 4;     // Decimal point in 3rd position
    }
    else if (value_int >= 10000)         // 100.00 - 999.99
    {
        display_digits[0] = value_int / 10000ul % 10;
        display_digits[1] = value_int / 1000ul % 10;
        display_digits[2] = value_int / 100ul % 10;
        *dp_pos = 4;     // Decimal point in 3rd position
    }
    else if (value_int >= 1000)     // 10.00 - 99.99
    {
        display_digits[0] = value_int / 1000ul % 10;
        display_digits[1] = value_int / 100ul % 10;
        display_digits[2] = value_int / 10ul % 10;
        *dp_pos = 2;     // Decimal point in 2nd position
    }
    else if (value_int >= 0)        // 0.00 - 9.99
    {
        display_digits[0] = value_int / 100ul % 10;
        display_digits[1] = value_int / 10ul % 10;
        display_digits[2] = value_int % 10;
        *dp_pos = 1;     // Decimal point in 1st position
    }
    else if (value_int > -1000)     // -9.90 to -0.10
    {
        display_digits[0] = 0x0B;
        display_digits[1] = (value_int * -1) / 100ul % 10;
        display_digits[2] = (value_int * -1) / 10ul % 10;
        *dp_pos = 2;     // Decimal point in 2nd position
    }
    else if (value_int > -10000)    // -99.9 to -10
    {
        display_digits[0] = 0x0B;
        display_digits[1] = (value_int * -1) / 1000ul % 10;
        display_digits[2] = (value_int * -1) / 100ul % 10;
        *dp_pos = 4;     // Decimal point in 3rd position
    }
    else                                // Out of range (negative)
    {
        display_digits[0] = 0x0B;
        display_digits[1] = 1;
        display_digits[2] = 0x0A;
        *dp_pos = 0;     // Decimal point off
    }
}

// Callback for Timer Interrupt - displays the next digit on the 7-segment display on each run
static bool IRAM_ATTR timer_group_isr_callback(void *args)
{
    static uint8_t display_digits[NUM_OF_ANODES * 2] = {0,1,2,3,4,5,6,7,8,9,0,1,2,3,4,5,6,7,8,9,0,1,2,3,4,5,6,7,8,9,0,1};
    static uint8_t display_segments[NUM_OF_ANODES * 2];
    static uint32_t decimal_points = 0;
    const uint8_t segment_patterns[12] = {0b00111111, 0b00000110, 0b01011011, 0b01001111, 0b01100110, 0b01101101, 0b01111100, 0b00000111, 0b01111111, 0b01100111, 0b00000000, 0b01000000};
    static uint8_t current_disp_index = 0;
    static uint8_t dim_cycle_counter = 0;
    uint32_t dp_temp;
    bool button2_held = !gpio_get_level(pin_BUTTON2);
    //bool button3_held = !gpio_get_level(pin_BUTTON3);
    bool display_agile = 0;
    bool display_flex = 0;
    
    if (CONFIG_ESP_TARIFF_AGILE_ENABLE)
    {
        display_agile = button2_held;
    }
    if (CONFIG_ESP_TARIFF_FLEX_ENABLE)
    {
        display_flex = button2_held;
    }
    
    // If first digit is about to be displayed, update display data
    if ((current_disp_index == 0) && (dim_cycle_counter == 0))
    {
        decimal_points = 0;
        // Right hand displays
        if (display_agile)
        {
            // Gas - not applicable to agile
            display_digits[0] = 10;
            display_digits[1] = 10;
            display_digits[2] = 10;
            
            
            if (timeSet && wifi_connected && got_elec_agile_unit_rate && ((elec_agile_validity >> agile_time) & 1))
            {
                // Electricity
                get_display_digits(elec_agile_rates[agile_time], &display_digits[3], &dp_temp);
                
                decimal_points |= dp_temp << 3;
            }
            else
            {
                // generate dashes pattern
                display_digits[3] = wifi_connected ? 0xB : 0xA;
                display_digits[4] = timeSet ? 0xB : 0xA;
                display_digits[5] = got_elec_agile_unit_rate ? 0xB : 0xA;
            }

        }
        else 
        {
            if (timeSet && wifi_connected && got_gas_tomorrow_unit_rate)
            {
                // Generate numeric digits
                // Gas
                get_display_digits(gas_tomorrow_unit_rate, &display_digits[0], &dp_temp);
                
                decimal_points |= dp_temp;
            }
            else
            {
                // generate dashes pattern
                display_digits[0] = wifi_connected ? 0xB : 0xA;
                display_digits[1] = timeSet ? 0xB : 0xA;
                display_digits[2] = got_gas_tomorrow_unit_rate ? 0xB : 0xA;
            }
            
            if (timeSet && wifi_connected && got_elec_tomorrow_unit_rate)
            {
                // Electricity
                get_display_digits(elec_tomorrow_unit_rate, &display_digits[3], &dp_temp);
                
                decimal_points |= dp_temp << 3;
            }
            else
            {
                // generate dashes pattern
                display_digits[3] = wifi_connected ? 0xB : 0xA;
                display_digits[4] = timeSet ? 0xB : 0xA;
                display_digits[5] = got_elec_tomorrow_unit_rate ? 0xB : 0xA;
            }
        }
        
        // Left hand display
        if (display_flex)
        {
            if (timeSet && wifi_connected && got_gas_flex_unit_rate)
            {
                // Generate numeric digits
                // Gas
                get_display_digits(gas_flex_unit_rate, &display_digits[16], &dp_temp);
                
                decimal_points |= dp_temp << 16;
            }
            else
            {
                // generate dashes pattern
                display_digits[16] = wifi_connected ? 0xB : 0xA;
                display_digits[17] = timeSet ? 0xB : 0xA;
                display_digits[18] = got_gas_flex_unit_rate ? 0xB : 0xA;
            }
            
            if (timeSet && wifi_connected && got_elec_flex_unit_rate)
            {
                // Electricity
                get_display_digits(elec_flex_unit_rate, &display_digits[19], &dp_temp);
                
                decimal_points |= dp_temp << 19;
            }
            else
            {
                // generate dashes pattern
                display_digits[19] = wifi_connected ? 0xB : 0xA;
                display_digits[20] = timeSet ? 0xB : 0xA;
                display_digits[21] = got_elec_flex_unit_rate ? 0xB : 0xA;
            }
        }
        else
        {
            if (timeSet && wifi_connected && got_gas_unit_rate)
            {
                // Generate numeric digits
                // Gas
                get_display_digits(gas_unit_rate, &display_digits[16], &dp_temp);
                
                decimal_points |= dp_temp << 16;
            }
            else
            {
                // generate dashes pattern
                display_digits[16] = wifi_connected ? 0xB : 0xA;
                display_digits[17] = timeSet ? 0xB : 0xA;
                display_digits[18] = got_gas_unit_rate ? 0xB : 0xA;
            }
            
            if (timeSet && wifi_connected && got_elec_unit_rate)
            {
                // Electricity
                get_display_digits(elec_unit_rate, &display_digits[19], &dp_temp);
                
                decimal_points |= dp_temp << 19;
            }
            else
            {
                // generate dashes pattern
                display_digits[19] = wifi_connected ? 0xB : 0xA;
                display_digits[20] = timeSet ? 0xB : 0xA;
                display_digits[21] = got_elec_unit_rate ? 0xB : 0xA;
            }
        }
        
        // Get required segment patterns
        for (uint8_t i = 0; i < NUM_OF_ANODES * 2; i++)
        {
            display_segments[i] = segment_patterns[display_digits[i]];
        }
    }
    
    // Turn off all digits
    // SPI code needed here
    gpio_set_level(pin_SOE, 1);
    
    // Turn off all segments by setting specified bits high in 'Write 1 to set' register
    GPIO.out_w1ts =      ((uint32_t)1 << pin_segAL)
                       | ((uint32_t)1 << pin_segBL)
                       | ((uint32_t)1 << pin_segCL)
                       | ((uint32_t)1 << pin_segDL)
                       | ((uint32_t)1 << pin_segEL)
                       | ((uint32_t)1 << pin_segFL)
                       | ((uint32_t)1 << pin_segGL)
                       | ((uint32_t)1 << pin_segDPL)
                       | ((uint32_t)1 << pin_segAR)
                       | ((uint32_t)1 << pin_segBR);
    GPIO.out1_w1ts.val = 
                         ((uint32_t)1 << (pin_segCR - 32))
                       | ((uint32_t)1 << (pin_segDR - 32))
                       | ((uint32_t)1 << (pin_segER - 32))
                       | ((uint32_t)1 << (pin_segFR - 32))
                       | ((uint32_t)1 << (pin_segGR - 32))
                       | ((uint32_t)1 << (pin_segDPR - 32));
    
    if (dim_cycle_counter >= (NUMBER_OF_BRIGHTNESS_SETTINGS - 1 - display_brightness))
    {
        // Turn on required digit
        // SPI code needed here
        for (uint8_t i = 0; i < NUM_OF_ANODES; i++)
        {
            gpio_set_level(pin_SDAT, (NUM_OF_ANODES - 1 - current_disp_index) == i ? 0 : 1);
            ets_delay_us(SR_DELAY_US);
            gpio_set_level(pin_SCK, 1);
            ets_delay_us(SR_DELAY_US);
            gpio_set_level(pin_SCK, 0);
        }
        
        ets_delay_us(SR_DELAY_US);
        gpio_set_level(pin_SLAT, 1);
        ets_delay_us(SR_DELAY_US);
        gpio_set_level(pin_SLAT, 0);
        ets_delay_us(SR_DELAY_US);
        gpio_set_level(pin_SOE, 0);
        
        // Turn on required segments
        GPIO.out_w1tc =
                             ((((display_segments[current_disp_index] & 0x01) >> 0)) << pin_segAL )
                           | ((((display_segments[current_disp_index] & 0x02) >> 1)) << pin_segBL )
                           | ((((display_segments[current_disp_index] & 0x04) >> 2)) << pin_segCL )
                           | ((((display_segments[current_disp_index] & 0x08) >> 3)) << pin_segDL )
                           | ((((display_segments[current_disp_index] & 0x10) >> 4)) << pin_segEL )
                           | ((((display_segments[current_disp_index] & 0x20) >> 5)) << pin_segFL )
                           | ((((display_segments[current_disp_index] & 0x40) >> 6)) << pin_segGL )
                           | ((((decimal_points >> current_disp_index & 0x01) >> 0)) << pin_segDPL)
                           | ((((display_segments[current_disp_index + NUM_OF_ANODES] & 0x01) >> 0)) << pin_segAR )
                           | ((((display_segments[current_disp_index + NUM_OF_ANODES] & 0x02) >> 1)) << pin_segBR )
                           ;
        GPIO.out1_w1tc.val = 
                             ((((display_segments[current_disp_index + NUM_OF_ANODES] & 0x04) >> 2)) << (pin_segCR - 32) )
                           | ((((display_segments[current_disp_index + NUM_OF_ANODES] & 0x08) >> 3)) << (pin_segDR - 32) )
                           | ((((display_segments[current_disp_index + NUM_OF_ANODES] & 0x10) >> 4)) << (pin_segER - 32) )
                           | ((((display_segments[current_disp_index + NUM_OF_ANODES] & 0x20) >> 5)) << (pin_segFR - 32) )
                           | ((((display_segments[current_disp_index + NUM_OF_ANODES] & 0x40) >> 6)) << (pin_segGR - 32) )
                           | ((((decimal_points >> (current_disp_index + NUM_OF_ANODES) & 0x01) >> 0)) << (pin_segDPR - 32) )
                           ;
    }
    
    // There are NUMBER_OF_BRIGHTNESS_SETTINGS iterations of dim_cycle_counter where the display is turned off or on
    // according to the value of display_brightness to change the brightness of the digit. After all iterations,
    // current_disp_index (the digit counter) is incremented. 
    if (dim_cycle_counter < (NUMBER_OF_BRIGHTNESS_SETTINGS - 1))
    {
        dim_cycle_counter++;
    }
    else
    {
        dim_cycle_counter = 0;
        do
        {
            if (current_disp_index < NUM_OF_ANODES - 1)
            {
                current_disp_index++;
            }
            else
            {
                current_disp_index = 0;
            }
        }
        while ((ANODES_IN_USE >> current_disp_index) == 0);
    }
    
    
    
    return true; // return whether we need to yield at the end of ISR
}


/**
 * @brief Initialize selected timer of timer group
 *
 * @param group Timer Group number, index from 0
 * @param timer timer ID, index from 0
 * @param auto_reload whether auto-reload on alarm event
 * @param timer_interval_tenthmsec interval of alarm
 */
static void example_timer_init(int group, int timer, bool auto_reload, int timer_interval_tenthmsec)
{
    /* Select and initialize basic parameters of the timer */
    timer_config_t config = {
        .divider = TIMER_DIVIDER,
        .counter_dir = TIMER_COUNT_UP,
        .counter_en = TIMER_PAUSE,
        .alarm_en = TIMER_ALARM_EN,
        .auto_reload = auto_reload,
    }; // default clock source is APB
    timer_init(group, timer, &config);

    /* Timer's counter will initially start from value below.
       Also, if auto_reload is set, this value will be automatically reload on alarm */
    timer_set_counter_value(group, timer, 0);

    /* Configure the alarm value and the interrupt on alarm. */
    timer_set_alarm_value(group, timer, timer_interval_tenthmsec * TIMER_SCALE);
    timer_enable_intr(group, timer);

    timer_info_t *timer_info = calloc(1, sizeof(timer_info_t));
    timer_info->timer_group = group;
    timer_info->timer_idx = timer;
    timer_info->auto_reload = auto_reload;
    timer_info->alarm_interval = timer_interval_tenthmsec;
    timer_isr_callback_add(group, timer, timer_group_isr_callback, timer_info, 0);

    timer_start(group, timer);
}

// Display task - sets up the Timer Interrupt on whichever core display_task is pinned to
// Timer interrupt used for updating the display to allow for faster refresh rate
void display_task(void * pvParameters)
{
    ESP_LOGI(TAG, "starting display_task on core %d", xPortGetCoreID());
    
    // Configure timer in this task to guarantee that correct core is used for ISR
    example_timer_init(TIMER_GROUP_0, TIMER_0, true, 2);
    
    while(1)
    {
        vTaskDelay(10000 / portTICK_RATE_MS);
    }
}

/* Read brightness sensor and update LED brightness
 * The photodiode is connected between the ADC input pin (K) and GND (A)
 * A 10k resistor is connected between the input pin and +3V3
 */
void get_light_level_task(void * pvParameters)
{
    adc1_config_width(width);
    adc1_config_channel_atten(channel, atten);
    uint16_t adc_filter[ADC_FILTER_LENGTH];
    uint32_t adc_average = 0;
    uint16_t adc_reading;
    
    while(1)
    {
        // 50ms sampling interval
        vTaskDelay(50 / portTICK_RATE_MS);
        
        // Move entries in the filter along
        for (uint8_t i = 0; i < ADC_FILTER_LENGTH - 1; i++)
        {
            adc_filter[i] = adc_filter[i+1];
        }
        // Get ADC reading and add to filter (invert so that higher value means brighter)
        adc_reading = adc1_get_raw((adc1_channel_t)channel);
        /*
        // Multiply by 2
        adc_reading = (adc_reading << 2);
        // Expand the upper half of the range and discard the lower half
        if (adc_reading > ((ADC_MAX_VALUE + 1) * 3))
        {
            adc_reading -= ((ADC_MAX_VALUE + 1) * 3);
        }
        else
        {
            adc_reading = 0;
        }
        */
        // Add to filter
        adc_filter[ADC_FILTER_LENGTH - 1] = ADC_MAX_VALUE - adc_reading;
        
        // Calculate average from the filter
        adc_average = 0;
        for (uint8_t i = 0; i < ADC_FILTER_LENGTH; i++)
        {
            adc_average += (uint32_t)adc_filter[i];
        }
        adc_average /= (uint32_t)ADC_FILTER_LENGTH;
        
        // Calculate LED brightness with hysteresis
        int adc_range_per_level = ADC_MAX_VALUE / NUMBER_OF_BRIGHTNESS_SETTINGS;
        int upper_limit = display_brightness * adc_range_per_level + adc_range_per_level + BRIGHTNESS_HYSTERESIS;
        int lower_limit = display_brightness * adc_range_per_level - BRIGHTNESS_HYSTERESIS;
        
        if ((int)adc_average >= upper_limit) {
            display_brightness = (display_brightness + 1) % NUMBER_OF_BRIGHTNESS_SETTINGS;
        } else if ((int)adc_average <= lower_limit) {
            display_brightness = (display_brightness + NUMBER_OF_BRIGHTNESS_SETTINGS - 1) % NUMBER_OF_BRIGHTNESS_SETTINGS;
        }
        
        // Print ADC reading
        //ESP_LOGI(TAG_ADC, "ADC value %d Brightness %d", adc_average, display_brightness);
    }
}

/* Reset the microcontroller if the prices have not been received within
 * a certain timeframe to ensure that the display never gets stuck
 * if the API is down or the WiFi access point is temporarily switched off,
 * causing ESP_MAXIMUM_RETRY to be exceeded and the connection procedure to give up.
 */
void fetcher_watchdog_task(void * pvParameters)
{
    uint32_t secondsCounter = 0;
    while(1)
    {
        // Non-blocking one-second delay
        vTaskDelay(1000 / portTICK_RATE_MS);
        // Check if any enabled unit rates haven't been obtained for any reason.
        // Tomorrow's rates don't need to be checked here as they are checked hourly elsewhere.
        if
        (
            (!got_gas_unit_rate) ||
            (!got_elec_unit_rate) ||
            ((!got_gas_flex_unit_rate) && CONFIG_ESP_TARIFF_FLEX_ENABLE) ||
            ((!got_elec_flex_unit_rate) && CONFIG_ESP_TARIFF_FLEX_ENABLE) ||
            ((!got_elec_agile_unit_rate) && CONFIG_ESP_TARIFF_AGILE_ENABLE)
        )
        {
            // Increment seconds counter and restart if limit is exceeded
            secondsCounter++;
            ESP_LOGI(TAG_FW, "Watchdog increment %d", secondsCounter);
            ESP_LOGI(TAG_FW, "Got unit rate flags %d %d %d %d %d", got_gas_unit_rate, got_elec_unit_rate, got_gas_flex_unit_rate, got_elec_flex_unit_rate, got_elec_agile_unit_rate);
            if (secondsCounter > FETCHER_WDOG_LIMIT_IN_SECONDS)
            {
                ESP_LOGI(TAG_FW, "Fetcher Watchdog reset");
                esp_restart();
            }
        }
        else
        {
            // Reset seconds counter if all expected unit rates have been obtained
            secondsCounter = 0;
        }
    }
}

// Main function - execution starts here
void app_main()
{
    ESP_LOGI("Reset reason: ", "%d", esp_reset_reason());
	//Initialize NVS
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);
    
    // Set up GPIO
    
    gpio_config_t io_conf;
    io_conf.intr_type = (gpio_int_type_t)GPIO_PIN_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = ((uint64_t)1 << pin_segAL)
                         | ((uint64_t)1 << pin_segBL)
                         | ((uint64_t)1 << pin_segCL)
                         | ((uint64_t)1 << pin_segDL)
                         | ((uint64_t)1 << pin_segEL)
                         | ((uint64_t)1 << pin_segFL)
                         | ((uint64_t)1 << pin_segGL)
                         | ((uint64_t)1 << pin_segDPL)
                         | ((uint64_t)1 << pin_segAR)
                         | ((uint64_t)1 << pin_segBR)
                         | ((uint64_t)1 << pin_segCR)
                         | ((uint64_t)1 << pin_segDR)
                         | ((uint64_t)1 << pin_segER)
                         | ((uint64_t)1 << pin_segFR)
                         | ((uint64_t)1 << pin_segGR)
                         | ((uint64_t)1 << pin_segDPR)
                         | ((uint64_t)1 << pin_SLAT)
                         | ((uint64_t)1 << pin_SOE)
                         | ((uint64_t)1 << pin_SDAT)
                         | ((uint64_t)1 << pin_SCK);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    ret = gpio_config(&io_conf);
    
	ESP_ERROR_CHECK(ret);
    
    /*
    spi_bus_config_t buscfg={
        .miso_io_num = -1,
        .mosi_io_num = pin_SDAT,
        .sclk_io_num = pin_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    //Initialize the SPI bus
    ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);
    
    spi_device_interface_config_t devcfg={
        .command_bits=16,
        .address_bits=0,
        .cs_ena_pretrans=2,
        .cs_ena_posttrans=2,
        .clock_speed_hz=1000000,
        .spics_io_num=pin_SLAT,
    }
    ret = spi_bus_add_device(SPi2_HOST, &devcfg, &spihandle);
    ESP_ERROR_CHECK(ret);
    */
    
    // FreeRTOS task setup
    
    TaskHandle_t getUnitRatesHandle;
    TaskHandle_t displayHandle;
    TaskHandle_t getLightLevelHandle;
    TaskHandle_t fetcherWatchdogHandle;
    
    xTaskCreatePinnedToCore(get_unit_rates_task, "get_unit_rates_task", 8192, NULL, configMAX_PRIORITIES - 3, &getUnitRatesHandle, 1);
    
    // Uncomment this task and comment out display_task below to test display with different values
    //xTaskCreatePinnedToCore(test_task, "test_task", 4096, NULL, configMAX_PRIORITIES - 3, &getUnitRatesHandle, 1);
    
    xTaskCreatePinnedToCore(display_task, "display_task", 2048, NULL, configMAX_PRIORITIES - 2, &displayHandle, 1);
    
    xTaskCreatePinnedToCore(fetcher_watchdog_task, "fetcher_watchdog_task", 4096, NULL, configMAX_PRIORITIES - 1, &fetcherWatchdogHandle, 1);
    
    xTaskCreatePinnedToCore(get_light_level_task, "get_light_level_task", 4096, NULL, configMAX_PRIORITIES - 4, &getLightLevelHandle, 1);

	while(1)
    {
        vTaskDelay(1000 / portTICK_RATE_MS);
    }
}
