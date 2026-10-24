#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
//  CONFIGURACOES GERAIS DO SISTEMA DE SEGURANCA
//  Centraliza tudo que pode mudar entre simulacao e uso real.
// ============================================================

// ---------- Identificacao do dispositivo ----------
#define DEVICE_ID          "esp32-seguranca-01"
#define LOCAL_INSTALACAO   "Residencia - Sala"

// ---------- Rede Wi-Fi ----------
// No Wokwi use SEMPRE "Wokwi-GUEST" com senha vazia.
#define WIFI_SSID          "Wokwi-GUEST"
#define WIFI_PASSWORD      ""
#define WIFI_CHANNEL       6          // acelera a conexao no Wokwi

// ---------- Broker MQTT ----------
#define MQTT_BROKER        "public.cloud.shiftr.io"
#define MQTT_PORT          1883
#define MQTT_USER          "public"   // instancia publica do shiftr.io
#define MQTT_PASS          "public"

// ---------- Topicos MQTT ----------
#define TOPIC_BASE         "fatec/seguranca/casa01"

#define TOPIC_STATUS       TOPIC_BASE "/status"       // armado/desarmado/alarme (retained)
#define TOPIC_MOVIMENTO    TOPIC_BASE "/sensor/movimento"
#define TOPIC_PORTA        TOPIC_BASE "/sensor/porta"
#define TOPIC_TEMPERATURA  TOPIC_BASE "/sensor/temperatura"  // graus C
#define TOPIC_GAS          TOPIC_BASE "/sensor/gas"          // % (0-100)
#define TOPIC_ALERTA       TOPIC_BASE "/alerta"
#define TOPIC_EVENTO       TOPIC_BASE "/evento"       // log em JSON
#define TOPIC_TELEMETRIA   TOPIC_BASE "/telemetria"   // uptime, rssi, estado
#define TOPIC_CONEXAO      TOPIC_BASE "/conexao"      // online/offline (LWT)
#define TOPIC_COMANDO      TOPIC_BASE "/comando"      // <- assinado pelo ESP32

// ---------- Pinos ----------
// Sensores
#define PIN_PIR            13    // sensor de movimento HC-SR501
#define PIN_PORTA          14    // reed switch / chave da porta (INPUT_PULLUP)
#define PIN_DHT            23    // DHT22 - temperatura e umidade
#define PIN_GAS            34    // MQ-2 (gas/fumaca) saida analogica AOUT - ADC1

// Atuadores
#define PIN_BUZZER         12    // buzzer piezoeletrico
#define PIN_RELE           27    // modulo rele -> sirene externa
#define PIN_LED_R          25    // LED RGB - vermelho
#define PIN_LED_G          26    // LED RGB - verde
#define PIN_LED_B          33    // LED RGB - azul

// Display LCD I2C
#define LCD_ENDERECO       0x27
#define LCD_COLUNAS        16
#define LCD_LINHAS         2
#define PIN_SDA            21
#define PIN_SCL            22

// Teclado matricial 4x4
#define KEYPAD_LINHAS      4
#define KEYPAD_COLUNAS     4
// linhas  R1..R4
#define PIN_KP_R1          19
#define PIN_KP_R2          18
#define PIN_KP_R3          5
#define PIN_KP_R4          17
// colunas C1..C4
#define PIN_KP_C1          16
#define PIN_KP_C2          4
#define PIN_KP_C3          2
#define PIN_KP_C4          15

// ---------- Regras de uso ----------
#define SENHA_PADRAO       "1234"
#define MAX_TENTATIVAS     3          // tentativas antes do bloqueio
#define TEMPO_BLOQUEIO_MS  30000UL    // 30 s de bloqueio do teclado
#define DELAY_SAIDA_MS     15000UL    // 15 s para sair depois de armar
#define DELAY_ENTRADA_MS   10000UL    // 10 s para digitar a senha ao entrar
#define DURACAO_SIRENE_MS  60000UL    // sirene toca no maximo 60 s

// ---------- Zona 24 h: incendio / vazamento de gas ----------
// Disparam o alarme em QUALQUER estado, mesmo desarmado.
#define TEMP_ALERTA_C       45.0f     // pre-alerta de temperatura
#define TEMP_ALARME_C       57.0f     // alarme de incendio (detector termico)
#define GAS_ALERTA_PCT      20.0f     // pre-alerta: subida acima da referencia
#define GAS_ALARME_PCT      35.0f     // alarme: subida acima da referencia
#define HISTERESE_TEMP_C    3.0f      // evita alarmes repetidos na fronteira
#define HISTERESE_GAS_PCT   5.0f
#define AQUECIMENTO_GAS_MS  10000UL   // MQ-2 precisa aquecer; mede a referencia
#define INTERVALO_AMBIENTE_MS    2000UL   // DHT22 aceita 1 leitura a cada 2 s

// ---------- Temporizacoes internas ----------
#define INTERVALO_TELEMETRIA_MS  5000UL   // envia telemetria a cada 5 s
#define INTERVALO_HEARTBEAT_MS   3000UL   // prova de vida (status da comunicacao)
#define DEBOUNCE_SENSOR_MS       300UL
#define TIMEOUT_DIGITACAO_MS     8000UL   // limpa senha parcial abandonada

#endif // CONFIG_H
