 /**
 *  @file    twilio.cpp
 *  @author  Sean Mathews <coder@f34r.com>
 *  @date    09/12/2020
 *  @version 1.0
 *
 *  @brief Simple commands to post to api.twilio.com
 *
 *  @copyright Copyright (C) 2020 Nu Tech Software Solutions, Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <string>
#include <sstream>
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "mbedtls/platform.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/esp_debug.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/certs.h"
#if defined(MBEDTLS_SSL_CACHE_C_BROKEN)
#include "mbedtls/ssl_cache.h"
#endif

extern "C" {
#include "ad2_utils.h"
#include "iot_uart_cli.h"
#include "iot_cli_cmd.h"
#include "twilio.h"
}

#if CONFIG_TWILIO_CLIENT

QueueHandle_t  sendQ=NULL;
mbedtls_ssl_config conf;
mbedtls_entropy_context entropy;
mbedtls_ctr_drbg_context ctr_drbg;
mbedtls_ssl_context ssl;
mbedtls_x509_crt cacert;
mbedtls_net_context server_fd;
#if defined(MBEDTLS_SSL_CACHE_C_BROKEN)
mbedtls_ssl_cache_context cache;
#endif

static const char *TAG = "TWILIO";

/* Root cert for api.twilio.com
   The PEM file was extracted from the output of this command:
   openssl s_client -showcerts -connect www.howsmyssl.com:443 </dev/null
   The CA root cert is the last cert given in the chain of certs.
   To embed it in the app binary, the PEM file is named
   in the component.mk COMPONENT_EMBED_TXTFILES variable.
*/
extern const uint8_t twilio_root_pem_start[] asm("_binary_twilio_root_pem_start");
extern const uint8_t twilio_root_pem_end[]   asm("_binary_twilio_root_pem_end");

/**
 * url_encode
 */
std::string urlencode(std::string str) {
  std::string encoded = "";
  char c;
  char code0;
  char code1;
  for (int i = 0; i < str.length(); i++) {
    c = str[i];
    if (c == ' ') {
      encoded += '+';
    } else if (isalnum(c)) {
      encoded += c;
    } else {
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9) {
        code1 = (c & 0xf) - 10 + 'A';
      }
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9) {
        code0 = c - 10 + 'A';
      }
      encoded += '%';
      encoded += code0;
      encoded += code1;
    }
  }
  return encoded;
}

/**
 * missing std::to_string()
 */
std::string to_string(int n)
{
    std::ostringstream stm;
    stm << n;
    return stm.str();
}

/**
 * build auth string from user and pass
 */
std::string get_auth_header(const std::string& user, const std::string& password) {

  size_t toencodeLen = user.length() + password.length() + 2;
  size_t out_len = 0;
  char toencode[toencodeLen];
  unsigned char outbuffer[(toencodeLen + 2 - ((toencodeLen + 2) % 3)) / 3 * 4 + 1];

  memset(toencode, 0, toencodeLen);

  snprintf(
    toencode,
    toencodeLen,
    "%s:%s",
    user.c_str(),
    password.c_str()
  );

  mbedtls_base64_encode(outbuffer,sizeof(outbuffer),&out_len,(unsigned char*)toencode, toencodeLen-1);
  outbuffer[out_len] = '\0';

  std::string encoded_string = std::string((char *)outbuffer);
  return "Authorization: Basic " + encoded_string;
}

/**
 * build_request_string()
 */
std::string build_request_string(std::string sid,
    std::string token, std::string body)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    std::string auth_header = get_auth_header(sid, token);
    std::string http_request =
        "POST /" + std::string(API_VERSION) + "/Accounts/" + sid + "/Messages HTTP/1.0\r\n" +
        "User-Agent: esp-idf/1.0 esp32(v" + to_string(chip_info.revision) + ")\r\n" +
        auth_header + "\r\n" +
        "Host: " + WEB_SERVER + "\r\n" +
        "Cache-control: no-cache\r\n" +
        "Content-Type: application/x-www-form-urlencoded; charset=utf-8\r\n" +
        "Content-Length: " + to_string(body.length()) + "\r\n" +
        "Connection: close\r\n" +
        "\r\n" + body + "\r\n";
    return http_request;
}

/**
 * twilio_add_queue()
 */
void twilio_add_queue(const char * sid, const char * token, const char *from, const char *to, char type, const char *arg) {
    if (sendQ) {
        twilio_message_data_t *message_data = NULL;
        message_data = (twilio_message_data_t *)malloc(sizeof(twilio_message_data_t));
        message_data->sid = strdup(sid);
        message_data->token = strdup(token);
        message_data->from =  strdup(from);
        message_data->to =  strdup(to);
        message_data->type = type;
        message_data->arg = strdup(arg);
        xQueueSend(sendQ,(void *)&message_data,(TickType_t )0);
    } else {
        ESP_LOGE(TAG, "Invalid queue handle");
    }
}

/**
 * Background task to watch queue and synchronously spawn tasks.
 * Note:
 *   1) Only runs one task at a time.
 *   2) Monitor/limit max tasks in queue.
 */
void twilio_consumer_task(void *pvParameter) {
    while(1) {
        ESP_LOGI(TAG,"queue consumer loop start");
        if(sendQ == NULL) {
            ESP_LOGW(TAG, "sendQ is not ready task ending");
            break;
        }
        twilio_message_data_t *message_data = NULL;
        if ( xQueueReceive(sendQ,&message_data,portMAX_DELAY) ) {
            ESP_LOGI(TAG, "creating task for twilio notification");
            // process the message
            BaseType_t xReturned = xTaskCreate(&twilio_send_task, "twilio_send_task", 1024*7, (void *)message_data, tskIDLE_PRIORITY+5, NULL);
            if (xReturned != pdPASS) {
                ESP_LOGE(TAG, "failed to create twilio task.");
            }
            // Sleep after spawingin a task.
            vTaskDelay(500/portTICK_PERIOD_MS); //wait for 500 ms
        }
    }
    vTaskDelete(NULL);
}

/**
 * cleanup memory
 */
void twilio_free() {
    mbedtls_ssl_free( &ssl );
    mbedtls_x509_crt_free( &cacert );
    mbedtls_ctr_drbg_free( &ctr_drbg);
    mbedtls_ssl_config_free( &conf );
    mbedtls_entropy_free( &entropy );
}

/**
 * Background task to send a message to twilio
 */
void twilio_send_task(void *pvParameters) {
    ESP_LOGI(TAG, "twilio send task start stack free %s", esp_get_free_heap_size());

    twilio_message_data_t *message_data = (twilio_message_data_t *)pvParameters;

    char buf[512];
    int ret, flags, len;
    size_t written_bytes = 0;
    std::string http_request;
    std::string body;
    char * reqp = NULL;


    // should never happen sanity check.
    if (message_data == NULL) {
        ESP_LOGE(TAG, "error null message_data aborting task.");
        vTaskDelete(NULL);
        return;
    }
    // load message data local for ease of access
    std::string sid = message_data->sid;
    std::string token = message_data->token;
    std::string to = message_data->to;
    std::string from = message_data->from;
    char type = message_data->type;
    std::string arg = message_data->arg;

    // free we are done with the pointers.
    free(message_data->sid);
    free(message_data->token);
    free(message_data->from);
    free(message_data->to);
    free(message_data->arg);
    free(message_data);

    /* Build the HTTPS POST request. */
    switch(type) {
        case 'M': // Messages
            body = "To=" + urlencode(to) + "&From=" + urlencode(from) + \
                     "&Body=" + urlencode(arg);
            break;
        case 'R': // Redirect
            break;
        case 'T': // Twiml URL
            body = "To=" + urlencode(to) + "&From=" + urlencode(from) + \
                     "&Url=" + urlencode(arg);
            break;
        default:
            ESP_LOGW(TAG, "Unknown message type '%c' aborting task.", type);
            vTaskDelete(NULL);
            return;
    }

    ESP_LOGI(TAG, "Connecting to %s:%s...", WEB_SERVER, WEB_PORT);

    // clean up and ready for new request
    mbedtls_ssl_session_reset(&ssl);
    mbedtls_net_free(&server_fd);

    if ((ret = mbedtls_net_connect(&server_fd, WEB_SERVER,
                                    WEB_PORT, MBEDTLS_NET_PROTO_TCP)) != 0)
    {
        ESP_LOGE(TAG, "mbedtls_net_connect returned -%x", -ret);
        goto exit;
    }

    ESP_LOGI(TAG, "Connected.");

    mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

    ESP_LOGI(TAG, "Performing the SSL/TLS handshake...");

    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0)
    {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            ESP_LOGE(TAG, "mbedtls_ssl_handshake returned -0x%x", -ret);
            goto exit;
        }
    }

    ESP_LOGI(TAG, "Verifying peer X.509 certificate...");

    if ((flags = mbedtls_ssl_get_verify_result(&ssl)) != 0)
    {
        /* In real life, we probably want to close connection if ret != 0 */
        ESP_LOGW(TAG, "Failed to verify peer certificate!");
        bzero(buf, sizeof(buf));
        mbedtls_x509_crt_verify_info(buf, sizeof(buf), "  ! ", flags);
        ESP_LOGW(TAG, "verification info: %s", buf);
    }
    else {
        ESP_LOGI(TAG, "Certificate verified.");
    }

    ESP_LOGI(TAG, "Cipher suite is %s", mbedtls_ssl_get_ciphersuite(&ssl));

    // build request string including basic auth headers
    http_request = build_request_string(sid, token, body);
    reqp = (char *)http_request.c_str();
#if defined(TWILIO_DEBUG)
    printf("sending message to twilio\n%s\n",http_request.c_str());
#endif
    /* Send HTTPS POST request. */
    ESP_LOGI(TAG, "Writing HTTP request...");
    do {
        ret = mbedtls_ssl_write(&ssl,
                                (const unsigned char *)reqp + written_bytes,
                                strlen(reqp) - written_bytes);
        if (ret >= 0) {
            ESP_LOGI(TAG, "%d bytes written", ret);
            written_bytes += ret;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_WRITE && ret != MBEDTLS_ERR_SSL_WANT_READ) {
            ESP_LOGE(TAG, "mbedtls_ssl_write returned -0x%x", -ret);
            goto exit;
        }
    } while(written_bytes < strlen(reqp));

    ESP_LOGI(TAG, "Reading HTTP response...");

    do
    {
        len = sizeof(buf) - 1;
        bzero(buf, sizeof(buf));
        ret = mbedtls_ssl_read(&ssl, (unsigned char *)buf, len);

        if(ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;

        if(ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            ret = 0;
            break;
        }

        if(ret < 0)
        {
            ESP_LOGE(TAG, "mbedtls_ssl_read returned -0x%x", -ret);
            break;
        }

        if(ret == 0)
        {
            ESP_LOGI(TAG, "connection closed");
            break;
        }

        len = ret;
        ESP_LOGD(TAG, "%d bytes read", len);
    } while(1);

    mbedtls_ssl_close_notify(&ssl);

exit:

    if(ret != 0)
    {
        mbedtls_strerror(ret, buf, 100);
        ESP_LOGE(TAG, "Last error was: -0x%x - %s", -ret, buf);
    }
    ESP_LOGI(TAG, "Completed requests stack free %d", uxTaskGetStackHighWaterMark(NULL));

    vTaskDelete(NULL);
}

extern "C" {

char * TWILIO_SETTINGS [] = {
  TWILIO_SID,
  TWILIO_TOKEN,
  TWILIO_TYPE,
  TWILIO_TO,
  TWILIO_FROM,
  TWILIO_BODY,
  0 // EOF
};


/**
 * Twilio generic command event processing
 *  command: [COMMAND] <id> <arg>
 * ex.
 *   [COMMAND] 0 arg...
 */
static void _cli_cmd_twilio_event(char *string)
{
    int slot = -1;
    char buf[80];
    char key[80];

    // key value validation
    ad2_copy_nth_arg(key, string, sizeof(key), 0);
    strlwr(key);

    int i;
    for(i = 0;; ++i)
    {
        if (TWILIO_SETTINGS[i] == 0) {
            printf("What?\n"); // FIXME: Impossible ish.
            break;
        }
        if(!strcmp(key, TWILIO_SETTINGS[i]) == 0)
        {
            if (ad2_copy_nth_arg(buf, string, sizeof(buf), 1) >= 0) {
                slot = strtol(buf, NULL, 10);
            }
            if (slot >= 0) {
                if (ad2_copy_nth_arg(buf, string, sizeof(buf), 2) >= 0) {
                    ad2_set_nv_slot_key_string(key, slot, buf);
                } else {
                    // FIXME: block get in production
                    ad2_get_nv_slot_key_string(key, slot, buf, sizeof(buf));
                    printf("Current slot #%02i '%s' value '%s'\n", slot, key, buf);
                }
            } else {
                printf("Missing <slot>\n");
            }
            // found match search done
            break;
        }
    }
}

static struct cli_command twilio_cmd_list[] = {
    {TWILIO_TOKEN,
        "Sets the 'User Auth Token' for notification <slot>.\n"
        "  Syntax: '" TWILIO_TOKEN "' <slot> <hash>\n"
        "  Example: '" TWILIO_TOKEN "' 0 aabbccdd112233..\n", _cli_cmd_twilio_event},
    {TWILIO_SID,
        "Sets the 'Account SID' for notification <slot>.\n"
        "  Syntax: '" TWILIO_SID "' <slot> <hash>\n"
        "  Example: '" TWILIO_SID "' 0 aabbccdd112233..\n", _cli_cmd_twilio_event},
    {TWILIO_FROM,
        "Sets the 'From' address for notification <slot>\n"
        "  Syntax: '" TWILIO_FROM "' <slot> <phone#>\n"
        "  Example: '" TWILIO_FROM "' 0 13115552368\n", _cli_cmd_twilio_event},
    {TWILIO_TO,
        "Sets the 'To' address for notification <slot>\n"
        "  Syntax: '" TWILIO_TO "' <slot> <phone#>\n"
        "  Example: '" TWILIO_TO "' 0 13115552368\n", _cli_cmd_twilio_event},
    {TWILIO_TYPE,
        "Sets the 'Type' [M]essages|[R]edirect|[T]wilio for notification <slot>\n"
        "  Syntax: '" TWILIO_TYPE "' <slot> <type>\n"
        "  Example: '" TWILIO_TYPE "' 0 M\n", _cli_cmd_twilio_event},
};

/**
 * Initialize queue and SSL
 */
void twilio_init() {

    // init server_fd
    mbedtls_net_init(&server_fd);
    // init ssl
    mbedtls_ssl_init(&ssl);
    // init SSL conf
    mbedtls_ssl_config_init(&conf);
#if defined(MBEDTLS_SSL_CACHE_C_BROKEN)
    mbedtls_ssl_cache_init( &cache );
#endif
    // init cert
    mbedtls_x509_crt_init(&cacert);
    // init entropy
    mbedtls_entropy_init(&entropy);
    // init drbg
    mbedtls_ctr_drbg_init(&ctr_drbg);
#if defined(DEBUG_TWILIO)
    mbedtls_debug_set_threshold( DEBUG_LEVEL );
#endif

    ESP_LOGI(TAG, "Loading the CA root certificate...");
    int ret = mbedtls_x509_crt_parse(&cacert, twilio_root_pem_start,
                                 twilio_root_pem_end-twilio_root_pem_start);
    if(ret < 0)
    {
        ESP_LOGE(TAG, "mbedtls_x509_crt_parse returned -0x%x\n\n", -ret);
        twilio_free();
        return;
    }

    ESP_LOGI(TAG, "Seeding the random number generator");
    if((ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                    NULL, 0)) != 0)
    {
        ESP_LOGE(TAG, "mbedtls_ctr_drbg_seed returned %d", ret);
        twilio_free();
        return;
    }

    /* MBEDTLS_SSL_VERIFY_OPTIONAL is bad for security, in this example it will print
       a warning if CA verification fails but it will continue to connect.
       You should consider using MBEDTLS_SSL_VERIFY_REQUIRED in your own code.
    */
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_ca_chain(&conf, &cacert, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
#ifdef CONFIG_MBEDTLS_DEBUG
    mbedtls_esp_enable_debug_log(&conf, 4);
#endif

#if defined(MBEDTLS_SSL_CACHE_C_BROKEN)
    mbedtls_ssl_conf_session_cache( &conf, &cache,
                                   mbedtls_ssl_cache_get,
                                   mbedtls_ssl_cache_set );
#endif

    ESP_LOGI(TAG, "Setting hostname for TLS session...");
     /* Hostname set here should match CN in server certificate */
    if((ret = mbedtls_ssl_set_hostname(&ssl, WEB_SERVER)) != 0)
    {
        ESP_LOGE(TAG, "mbedtls_ssl_set_hostname returned -0x%x", -ret);
        twilio_free();
        return;
    }

    ESP_LOGI(TAG, "Setting up the SSL/TLS structure...");
    if((ret = mbedtls_ssl_config_defaults(&conf,
                                          MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
    {
        ESP_LOGE(TAG, "mbedtls_ssl_config_defaults returned %d", ret);
        twilio_free();
        return;
    }


    if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0)
    {
        ESP_LOGE(TAG, "mbedtls_ssl_setup returned -0x%x\n\n", -ret);
        twilio_free();
        return;
    }


    // register twilio CLI commands
    for (int i = 0; i < ARRAY_SIZE(twilio_cmd_list); i++)
        cli_register_command(&twilio_cmd_list[i]);

    ESP_LOGI(TAG, "Starting twilio queue consumer task");
    sendQ = xQueueCreate(TWILIO_QUEUE_SIZE,sizeof(struct twilio_message_data *));
    if( sendQ == 0 ) {
        ESP_LOGE(TAG, "Failed to create twilio queue.");
    } else {
        xTaskCreate(&twilio_consumer_task, "twilio_consumer_task", 2048, NULL, tskIDLE_PRIORITY+1, NULL);
    }
}

} // extern "C"

#endif /*  CONFIG_TWILIO_CLIENT */