/*
 * sleep_tactil — Clase 3 (Sistemas Embebidos, UCU 2026)
 *
 * Base de la App 3: la ESP32-S2-Kaluga-1 recorre los tres modos de energía
 * —activo, light sleep y deep sleep— despertando por los botones táctiles de
 * la ESP-LyraP-TouchA, y publica el balance de consumo de cada ciclo.
 *
 *      ACTIVO ──▶ LIGHT SLEEP ──(toque o plazo)──▶ ACTIVO ──▶ …
 *         │                                                   │
 *         └──── tras N rondas ──▶ DEEP SLEEP ──(toque T5)──────┘
 *                                      │
 *                            reinicia desde app_main()
 *
 * La diferencia que hay que ver con los propios ojos:
 *   - de LIGHT sleep se vuelve a la instrucción siguiente, con la RAM intacta;
 *   - de DEEP sleep se vuelve a app_main(), y lo único que sobrevive es lo
 *     que esté marcado RTC_DATA_ATTR.
 *
 * Hardware:
 *   - ESP-LyraP-TouchA en el Touch FPC Connector (cable de 20 pines).
 *   - Microinterruptores T1-T14 de la cara inferior en OFF.
 *   - LED RGB direccionable en GPIO45 (requiere el JUMPER RGB colocado).
 *   - Target: esp32s2 (touch sensor V2).
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/touch_sens.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "sdkconfig.h"

#define RGB_LED_GPIO            45
#define RGB_LED_COUNT           1

/* --- Parámetros del ciclo. Son los que tocarás en la App 3. -------------- */

/* Plazo máximo de cada light sleep; también se sale con un toque. */
#define LIGHT_SLEEP_MS          5000
/* Cuántas rondas de light sleep antes de irse a deep sleep. */
#define RONDAS_LIGHT            3
/* Plazo de respaldo del deep sleep, por si nadie toca la placa. */
#define DEEP_SLEEP_S            30

/* Corrientes TÍPICAS del ESP32-S2 Series Datasheet v1.9 (tablas 5-8 y 5-9),
 * a 3,3 V, en MICROAMPERIOS. No son medidas de esta placa: son el dato del
 * fabricante para el chip solo. Todo el balance de energía se apoya en ellas.
 *
 * Van en enteros a propósito: el Xtensa LX7 del ESP32-S2 NO tiene unidad de
 * punto flotante, así que cada operación con float la emula el compilador en
 * software. Ver clase-2/detalle-punto-fijo-y-float.md. */
#define I_ACTIVO_UA             23000
#define I_LIGHT_SLEEP_UA          750
#define I_DEEP_SLEEP_UA            22
/* Capacidad de la batería usada para estimar la autonomía, en µAh. */
#define BATERIA_UAH           1000000L

/* --- Calibración del táctil (igual que en la Clase 2) -------------------- */
#define BARRIDOS_CALIBRACION    3
#define UMBRAL_RATIO            0.02f
#define UMBRAL_INICIAL          2000
#define COLA_LARGO              16

static const char *TAG = "sleep_tactil";

/* ------------------------------------------------------ botones --------- */

typedef enum {
    ACC_DESPERTAR = 0,   /* solo despierta; es el canal de deep sleep */
    ACC_AVANZAR,         /* fuerza el paso al modo siguiente          */
    ACC_MEMORIA,         /* muestra la información de memoria         */
} accion_t;

typedef struct {
    int canal;
    const char *nombre;
    accion_t accion;
} boton_t;

/* El PRIMERO de la lista es el canal designado para despertar del deep sleep:
 * hace de "botón de encendido" de la aplicación. */
static const boton_t BOTONES[] = {
    {  5, "RECORD",  ACC_DESPERTAR },
    {  2, "PLAY",    ACC_AVANZAR   },
    { 11, "NETWORK", ACC_MEMORIA   },
};
#define BOTONES_N   (sizeof(BOTONES) / sizeof(BOTONES[0]))
#define CANAL_DEEP_SLEEP   0        /* índice en BOTONES */

/* ------------------------------------------- estado que cruza el sueño -- */

/* Todo lo que esté en RTC_DATA_ATTR vive en la RTC SLOW y sobrevive al deep
 * sleep. Se inicializa en el encendido y NO al despertar: por eso sirve de
 * memoria entre ciclos. Son 8 KB en total: contadores, no buffers. */
RTC_DATA_ATTR static uint32_t s_arranques;
RTC_DATA_ATTR static uint32_t s_despertares_tactil;
RTC_DATA_ATTR static uint32_t s_despertares_timer;
RTC_DATA_ATTR static int64_t  s_us_activo;
RTC_DATA_ATTR static int64_t  s_us_light;
RTC_DATA_ATTR static int64_t  s_us_deep;
/* Instante en que entramos en deep sleep, para medir cuánto dormimos. */
RTC_DATA_ATTR static struct timeval s_entrada_deep;

/* ------------------------------------------- estado de esta sesión ------ */

static QueueHandle_t s_cola;
static touch_channel_handle_t s_canales[BOTONES_N];
static touch_sensor_handle_t s_sensor;
static led_strip_handle_t s_led;

typedef struct {
    int canal;
    int64_t marca_us;
} evento_t;

/* ---------------------------------------------------------- ISR --------- */

/* Contexto de interrupción: solo encola. Todo lo demás pasa en la tarea. */
static bool IRAM_ATTR al_tocar(touch_sensor_handle_t sensor,
                               const touch_active_event_data_t *ev,
                               void *ctx)
{
    BaseType_t hp_task_woken = pdFALSE;
    const evento_t e = { .canal = ev->chan_id, .marca_us = esp_timer_get_time() };
    xQueueSendFromISR(s_cola, &e, &hp_task_woken);
    return hp_task_woken == pdTRUE;
}

/* ------------------------------------------------------ periféricos ----- */

static void configurar_led(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_GPIO,
        .max_leds = RGB_LED_COUNT,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, /* 10 MHz */
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led));
    ESP_ERROR_CHECK(led_strip_clear(s_led));
}

static void led(uint8_t r, uint8_t g, uint8_t b)
{
    ESP_ERROR_CHECK(led_strip_set_pixel(s_led, 0, r, g, b));
    ESP_ERROR_CHECK(led_strip_refresh(s_led));
}

static touch_channel_config_t config_canal_por_defecto(void)
{
    touch_channel_config_t cfg = {
        .active_thresh = { UMBRAL_INICIAL },
        .charge_speed = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
    };
    return cfg;
}

/* Mide la línea base real de cada canal y fija el umbral como un porcentaje
 * de ella. Ver clase-2/detalles/touch-capacitivo-esp32s2.md. */
static void calibrar_umbrales(void)
{
    ESP_LOGI(TAG, "Calibrando... NO toques la placa durante el arranque");

    ESP_ERROR_CHECK(touch_sensor_enable(s_sensor));
    for (int i = 0; i < BARRIDOS_CALIBRACION; i++) {
        ESP_ERROR_CHECK(touch_sensor_trigger_oneshot_scanning(s_sensor, 2000));
    }
    ESP_ERROR_CHECK(touch_sensor_disable(s_sensor));

    for (size_t i = 0; i < BOTONES_N; i++) {
        uint32_t benchmark[TOUCH_SAMPLE_CFG_NUM] = { 0 };
        ESP_ERROR_CHECK(touch_channel_read_data(s_canales[i],
                                                TOUCH_CHAN_DATA_TYPE_BENCHMARK,
                                                benchmark));

        touch_channel_config_t cfg = config_canal_por_defecto();
        cfg.active_thresh[0] = (uint32_t)(benchmark[0] * UMBRAL_RATIO);
        ESP_ERROR_CHECK(touch_sensor_reconfig_channel(s_canales[i], &cfg));

        ESP_LOGI(TAG, "  %-8s T%-2d  benchmark=%" PRIu32 "  umbral=%" PRIu32,
                 BOTONES[i].nombre, BOTONES[i].canal, benchmark[0],
                 cfg.active_thresh[0]);
    }
}

static void configurar_tactil(void)
{
    touch_sensor_sample_config_t muestreo[TOUCH_SAMPLE_CFG_NUM] = {
        TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5,
                                                   TOUCH_VOLT_LIM_H_2V2),
    };

    touch_sensor_config_t sens_cfg =
        TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(TOUCH_SAMPLE_CFG_NUM, muestreo);
    ESP_ERROR_CHECK(touch_sensor_new_controller(&sens_cfg, &s_sensor));

    touch_channel_config_t chan_cfg = config_canal_por_defecto();
    for (size_t i = 0; i < BOTONES_N; i++) {
        ESP_ERROR_CHECK(touch_sensor_new_channel(s_sensor, BOTONES[i].canal,
                                                 &chan_cfg, &s_canales[i]));
    }

    touch_sensor_filter_config_t filtro = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    ESP_ERROR_CHECK(touch_sensor_config_filter(s_sensor, &filtro));

    calibrar_umbrales();

    touch_event_callbacks_t callbacks = { .on_active = al_tocar };
    ESP_ERROR_CHECK(touch_sensor_register_callbacks(s_sensor, &callbacks, NULL));

    /* El sensor tiene que estar DESHABILITADO para configurar el wake-up.
     * El canal designado es el único que despierta del deep sleep; en light
     * sleep sirve cualquier canal habilitado.
     * En el ESP32-S2 el driver avisa por consola de que mantiene alimentado
     * el dominio RTC_PERIPH aunque se le pida apagarlo: es una precaución
     * contra disparos falsos, y es correcto. */
    touch_chan_info_t info = { 0 };
    ESP_ERROR_CHECK(touch_sensor_get_channel_info(s_canales[CANAL_DEEP_SLEEP], &info));

    touch_sleep_config_t slp_cfg =
        TOUCH_SENSOR_DEFAULT_DSLP_PD_CONFIG(s_canales[CANAL_DEEP_SLEEP],
                                            info.active_thresh[0]);
    ESP_ERROR_CHECK(touch_sensor_config_sleep_wakeup(s_sensor, &slp_cfg));

    ESP_ERROR_CHECK(touch_sensor_enable(s_sensor));
    ESP_ERROR_CHECK(touch_sensor_start_continuous_scanning(s_sensor));

    ESP_LOGI(TAG, "Canal de deep sleep: %s (T%d, GPIO%d)",
             BOTONES[CANAL_DEEP_SLEEP].nombre, info.chan_id, info.chan_gpio);
}

/* ------------------------------------------------------- utilidades ----- */

static const boton_t *boton_de_canal(int canal)
{
    for (size_t i = 0; i < BOTONES_N; i++) {
        if (BOTONES[i].canal == canal) {
            return &BOTONES[i];
        }
    }
    return NULL;
}

/* -------------------------------------------------------- memoria ------- */

static const char *region_de(const void *p)
{
    if (p == NULL) { return "(no disponible en este sdkconfig)"; }

    if (esp_ptr_in_rtc_slow(p))      { return "RTC SLOW  (retenida en sueno)"; }
    if (esp_ptr_in_rtc_dram_fast(p)) { return "RTC FAST  (retenida, datos)"; }
    if (esp_ptr_in_rtc_iram_fast(p)) { return "RTC FAST  (retenida, codigo)"; }

    if (esp_ptr_in_iram(p))          { return "IRAM      (SRAM, codigo)"; }
    if (esp_ptr_in_dram(p))          { return "DRAM      (SRAM, datos)"; }

    if (esp_ptr_in_drom(p))          { return "DROM      (flash, .rodata)"; }
    if (esp_ptr_executable(p))       { return "IROM      (flash, codigo)"; }

    return "desconocida";
}

static void mostrar(const char *que, const void *p)
{
    ESP_LOGI(TAG, "  %-24s 0x%08" PRIxPTR "   %s", que, (uintptr_t)p, region_de(p));
}

static void informar_direcciones(void)
{
    uint32_t en_la_pila = 0;

    ESP_LOGI(TAG, "=== Donde vive cada cosa ===");
    mostrar("ISR tactil",       (const void *)al_tocar);
    mostrar("cadena literal",   "hola");
    mostrar("variable de pila", &en_la_pila);
    mostrar("RTC_DATA_ATTR",    &s_arranques);
}

static void informar_heap(void)
{
    ESP_LOGI(TAG, "=== El heap, por capacidades ===");
    ESP_LOGI(TAG, "  %-22s %8s %8s %8s", "capacidad", "total", "libre", "mayor");
    ESP_LOGI(TAG, "  %-22s %8u %8u %8u", "INTERNAL (SRAM)",
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "  %-22s %8u %8u %8u", "8BIT (byte a byte)",
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "  %-22s %8u %8u %8u", "DMA (alcanzable por DMA)",
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    ESP_LOGI(TAG, "  %-22s %8u %8u %8u", "RTCRAM (RTC FAST)",
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_RTCRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_RTCRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_RTCRAM));

    ESP_LOGI(TAG, "  heap libre ahora  : %" PRIu32 " B", esp_get_free_heap_size());
    ESP_LOGI(TAG, "  minimo historico  : %" PRIu32 " B  <- el que importa",
             esp_get_minimum_free_heap_size());
    ESP_LOGI(TAG, "  margen de pila    : %u palabras de 4 B",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static void informar_causa_despertar(void)
{
    const uint32_t causas = esp_sleep_get_wakeup_causes();

    if (causas & BIT(ESP_SLEEP_WAKEUP_UNDEFINED)) {
        ESP_LOGI(TAG, "Causa del despertar: ENCENDIDO o RESET (no veniamos de dormir)");
        return;
    }

    /* Tiempo realmente dormido: el reloj del sistema se recupera del
     * temporizador RTC, que sigue contando con el chip dormido. */
    struct timeval ahora;
    gettimeofday(&ahora, NULL);
    const int64_t dormido_ms = (ahora.tv_sec - s_entrada_deep.tv_sec) * 1000LL +
                               (ahora.tv_usec - s_entrada_deep.tv_usec) / 1000LL;
    if (dormido_ms > 0) {
        s_us_deep += dormido_ms * 1000LL;
        ESP_LOGI(TAG, "Dormido en deep sleep: %" PRId64 " ms", dormido_ms);
    }

    if (causas & BIT(ESP_SLEEP_WAKEUP_TOUCHPAD)) {
        s_despertares_tactil++;
        const int canal = esp_sleep_get_touchpad_wakeup_status();
        const boton_t *b = boton_de_canal(canal);
        ESP_LOGI(TAG, "Causa del despertar: TACTIL (canal T%d, %s)",
                 canal, b != NULL ? b->nombre : "?");
    }

    if (causas & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
        s_despertares_timer++;
        ESP_LOGI(TAG, "Causa del despertar: TEMPORIZADOR RTC (nadie toco la placa)");
    }
}

/* El balance completo: tiempo medido por modo, corriente citada del
 * datasheet, energía y autonomía. Es la tabla que pide la App 3.
 *
 * Todo en aritmética entera de 64 bits:
 *   carga [µA·ms] = corriente [µA] x tiempo [ms]
 *   1 µAh = 3600 µA·s = 3 600 000 µA·ms
 * Con tiempos de horas y corrientes de decenas de mA el producto no pasa de
 * 1e12, muy lejos del límite de int64_t. */
static void linea_modo(const char *nombre, int64_t t_ms, int32_t i_ua,
                       int64_t t_total_ms)
{
    const int64_t carga_uams = (int64_t)i_ua * t_ms;
    const int64_t nah = carga_uams / 3600;
    const int64_t pct_x10 = (t_total_ms > 0) ? (t_ms * 1000) / t_total_ms : 0;

    ESP_LOGI(TAG, "  %-12s %6" PRId64 ",%01" PRId64 " s  x %6" PRId32
                  " uA  = %7" PRId64 ",%03" PRId64 " uAh  (%3" PRId64 ",%01"
                  PRId64 " %%)",
             nombre, t_ms / 1000, (t_ms % 1000) / 100, i_ua,
             nah / 1000, nah % 1000, pct_x10 / 10, pct_x10 % 10);
}

static void informar_balance(void)
{
    const int64_t ms_activo = s_us_activo / 1000;
    const int64_t ms_light  = s_us_light  / 1000;
    const int64_t ms_deep   = s_us_deep   / 1000;
    const int64_t ms_total  = ms_activo + ms_light + ms_deep;

    if (ms_total <= 0) {
        return;
    }

    const int64_t carga_uams = (int64_t)I_ACTIVO_UA      * ms_activo +
                               (int64_t)I_LIGHT_SLEEP_UA * ms_light  +
                               (int64_t)I_DEEP_SLEEP_UA  * ms_deep;
    const int64_t total_nah = carga_uams / 3600;

    /* Corriente media = carga total / tiempo total, ambas ya en las mismas
     * unidades de tiempo, así que el resultado sale directamente en µA. */
    const int64_t i_media_ua = carga_uams / ms_total;
    const int64_t horas = (i_media_ua > 0) ? BATERIA_UAH / i_media_ua : 0;
    const int64_t dias_x10 = horas * 10 / 24;

    ESP_LOGI(TAG, "--- Balance de energia acumulado (%" PRIu32 " arranques) ---",
             s_arranques);
    linea_modo("activo",      ms_activo, I_ACTIVO_UA,      ms_total);
    linea_modo("light sleep", ms_light,  I_LIGHT_SLEEP_UA, ms_total);
    linea_modo("deep sleep",  ms_deep,   I_DEEP_SLEEP_UA,  ms_total);
    ESP_LOGI(TAG, "  %-12s %6" PRId64 ",%01" PRId64 " s                = %7"
                  PRId64 ",%03" PRId64 " uAh",
             "TOTAL", ms_total / 1000, (ms_total % 1000) / 100,
             total_nah / 1000, total_nah % 1000);
    ESP_LOGI(TAG, "  corriente media : %" PRId64 " uA  (%" PRId64 ",%03"
                  PRId64 " mA)",
             i_media_ua, i_media_ua / 1000, i_media_ua % 1000);
    ESP_LOGI(TAG, "  autonomia con %ld uAh: %" PRId64 " h  (%" PRId64 ",%01"
                  PRId64 " dias)",
             (long)BATERIA_UAH, horas, dias_x10 / 10, dias_x10 % 10);
    ESP_LOGI(TAG, "  despertares: %" PRIu32 " por tactil, %" PRIu32 " por timer",
             s_despertares_tactil, s_despertares_timer);
    ESP_LOGI(TAG, "  (corrientes del datasheet, no medidas: ver el README)");
}

/* Deja el sistema en condiciones de dormir: LED apagado y consola vaciada.
 * Un LED encendido son decenas de mA; el mensaje a medio enviar se pierde. */
static void preparar_sueno(void)
{
    ESP_ERROR_CHECK(led_strip_clear(s_led));
    uart_wait_tx_idle_polling(CONFIG_ESP_CONSOLE_UART_NUM);
}

/* ----------------------------------------------------------- fases ------ */

/* Fase activa: el LED verde indica "despierto".
 * PLAY avanza a light sleep y NETWORK muestra la memoria. */
static int64_t fase_activa(void)
{
    const int64_t inicio = esp_timer_get_time();
    evento_t ev;

    led(0, 24, 0);
    ESP_LOGI(TAG, "[ACTIVO] PLAY = avanzar  ·  NETWORK = memoria");

    while (1) {
        if (xQueueReceive(s_cola, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const boton_t *b = boton_de_canal(ev.canal);
        if (b == NULL) {
            continue;
        }

        ESP_LOGI(TAG, "  toque en %s (T%d), latencia ISR->tarea %" PRId64 " us",
                 b->nombre, ev.canal, esp_timer_get_time() - ev.marca_us);

        if (b->accion == ACC_AVANZAR) {
            break;
        }

        if (b->accion == ACC_MEMORIA) {
            informar_direcciones();
            informar_heap();
        }
    }

    return esp_timer_get_time() - inicio;
}

/* Light sleep: la CPU se detiene, la RAM se conserva y la ejecución continúa
 * en la línea siguiente. Devuelve los µs dormidos, medidos por el propio
 * esp_timer, que sigue avanzando durante el sueño. */
static int64_t fase_light_sleep(int ronda)
{
    ESP_LOGI(TAG, "[LIGHT SLEEP] ronda %d/%d, hasta %d ms o hasta que toques",
             ronda + 1, RONDAS_LIGHT, LIGHT_SLEEP_MS);

    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)LIGHT_SLEEP_MS * 1000));
    preparar_sueno();

    const int64_t antes = esp_timer_get_time();
    esp_light_sleep_start();                 /* <-- vuelve AQUI MISMO */
    const int64_t dormido = esp_timer_get_time() - antes;

    const uint32_t causas = esp_sleep_get_wakeup_causes();
    if (causas & BIT(ESP_SLEEP_WAKEUP_TOUCHPAD)) {
        s_despertares_tactil++;
        ESP_LOGI(TAG, "  desperto un TOQUE tras %" PRId64 " ms", dormido / 1000);
    } else if (causas & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
        s_despertares_timer++;
        ESP_LOGI(TAG, "  vencio el PLAZO tras %" PRId64 " ms", dormido / 1000);
    } else {
        ESP_LOGW(TAG, "  desperto por otra causa (mascara 0x%" PRIx32 ")", causas);
    }

    /* Las variables locales y la cola siguen ahí: no hemos perdido nada. */
    led(0, 0, 24);
    vTaskDelay(pdMS_TO_TICKS(120));
    return dormido;
}

/* Deep sleep: no vuelve. El núcleo digital se apaga, se pierde la RAM y al
 * despertar se ejecuta app_main() otra vez. */
static void __attribute__((noreturn)) fase_deep_sleep(void)
{
    ESP_LOGI(TAG, "[DEEP SLEEP] hasta %d s, o hasta que toques %s (T%d)",
             DEEP_SLEEP_S, BOTONES[CANAL_DEEP_SLEEP].nombre,
             BOTONES[CANAL_DEEP_SLEEP].canal);
    ESP_LOGI(TAG, "  al despertar se reinicia desde app_main(): lo unico que");
    ESP_LOGI(TAG, "  sobrevive es lo marcado RTC_DATA_ATTR.");

    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)DEEP_SLEEP_S * 1000000ULL));

    gettimeofday(&s_entrada_deep, NULL);     /* en RTC: sobrevive al sueño */
    preparar_sueno();

    esp_deep_sleep_start();                  /* <-- no retorna nunca */
}

/* ------------------------------------------------------------ main ------ */

static void tarea_ciclo(void *arg)
{
    while (1) {
        s_us_activo += fase_activa();

        for (int ronda = 0; ronda < RONDAS_LIGHT; ronda++) {
            s_us_light += fase_light_sleep(ronda);
        }

        informar_balance();
        fase_deep_sleep();                   /* no vuelve */
    }
}

void app_main(void)
{
    s_arranques++;

    ESP_LOGI(TAG, "=== Arranque #%" PRIu32 " ===", s_arranques);
    informar_causa_despertar();
    informar_balance();

    configurar_led();
    led(24, 12, 0);                          /* ambar: arrancando */

    s_cola = xQueueCreate(COLA_LARGO, sizeof(evento_t));
    ESP_ERROR_CHECK(s_cola != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    /* La cola debe existir antes de que se registren los callbacks. */
    configurar_tactil();

    xTaskCreate(tarea_ciclo, "ciclo", 4096, NULL, 10, NULL);
}