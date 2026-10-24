/*
 * ============================================================
 *  PROJETO IoT - SISTEMA DE SEGURANCA RESIDENCIAL
 *  ESP32 + MQTT + Node-RED + Dashboard
 *
 *  Central de alarme com:
 *   - sensor de presenca (PIR)
 *   - sensor de abertura de porta (reed switch)
 *   - sensor de gas/fumaca (MQ-2) e de temperatura (DHT22) - zona 24 h
 *   - teclado 4x4 para armar / desarmar por senha
 *   - sirene (rele), buzzer local, LED RGB de status e LCD 16x2
 *   - publicacao de estado, sensores, alertas e telemetria por MQTT
 *   - comandos remotos vindos do Dashboard (Node-RED)
 *
 *  Toda a logica roda em uma maquina de estados nao bloqueante:
 *  nenhum delay() no loop principal, para nao atrasar o MQTT.
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <ArduinoJson.h>
#include <DHTesp.h>

#include "config.h"

// ============================================================
//  MAQUINA DE ESTADOS
// ============================================================
enum EstadoSistema {
  DESARMADO,   // sensores ignorados
  SAINDO,      // armado, contando o delay de saida
  ARMADO,      // vigilancia ativa
  ENTRANDO,    // violacao detectada, contando o delay de entrada
  ALARME,      // sirene disparada
  BLOQUEADO    // teclado bloqueado por excesso de tentativas
};

EstadoSistema estado = DESARMADO;
EstadoSistema estadoAntesBloqueio = DESARMADO;   // para onde voltar apos o bloqueio

const char *nomeEstado(EstadoSistema e) {
  switch (e) {
    case DESARMADO: return "DESARMADO";
    case SAINDO:    return "SAINDO";
    case ARMADO:    return "ARMADO";
    case ENTRANDO:  return "ENTRANDO";
    case ALARME:    return "ALARME";
    case BLOQUEADO: return "BLOQUEADO";
  }
  return "DESCONHECIDO";
}

// ============================================================
//  OBJETOS DE HARDWARE
// ============================================================
LiquidCrystal_I2C lcd(LCD_ENDERECO, LCD_COLUNAS, LCD_LINHAS);

char mapaTeclas[KEYPAD_LINHAS][KEYPAD_COLUNAS] = {
  { '1', '2', '3', 'A' },
  { '4', '5', '6', 'B' },
  { '7', '8', '9', 'C' },
  { '*', '0', '#', 'D' }
};
byte pinosLinha[KEYPAD_LINHAS]  = { PIN_KP_R1, PIN_KP_R2, PIN_KP_R3, PIN_KP_R4 };
byte pinosColuna[KEYPAD_COLUNAS] = { PIN_KP_C1, PIN_KP_C2, PIN_KP_C3, PIN_KP_C4 };

Keypad teclado = Keypad(makeKeymap(mapaTeclas), pinosLinha, pinosColuna,
                        KEYPAD_LINHAS, KEYPAD_COLUNAS);

DHTesp dht;

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// ============================================================
//  VARIAVEIS DE ESTADO
// ============================================================
String senhaDigitada    = "";
String senhaSistema     = SENHA_PADRAO;
uint8_t tentativas      = 0;

bool movimentoDetectado = false;
bool portaAberta        = false;
bool sireneLigada       = false;
bool sireneSilenciada   = false;

String ultimaOrigemAlarme = "-";

// Zona 24 h (incendio / gas)
float temperatura   = NAN;
float umidade       = NAN;
float gasPct        = 0;      // leitura atual do MQ-2 em % da escala do ADC
float gasReferencia = -1;     // media em ar limpo medida no aquecimento
float somaGasAquecimento = 0;
uint16_t amostrasGas     = 0;
bool alertaTemp = false, alarmeTemp = false;
bool alertaGas  = false, alarmeGas  = false;

unsigned long tEstado        = 0;   // quando entramos no estado atual
unsigned long tUltimaTecla   = 0;
unsigned long tUltimoPir     = 0;
unsigned long tUltimaPorta   = 0;
unsigned long tTelemetria    = 0;
unsigned long tHeartbeat     = 0;
unsigned long tReconexao     = 0;
unsigned long tBuzzer        = 0;
unsigned long tLcd           = 0;
unsigned long tAmbiente      = 0;

bool buzzerLigado = false;

String lcdLinha1Atual = "";
String lcdLinha2Atual = "";

// ============================================================
//  PROTOTIPOS
// ============================================================
void mudarEstado(EstadoSistema novo, const char *origem);
void publicarEstado();
void publicarEvento(const char *tipo, const char *origem, const char *descricao);
void publicarAlerta(const char *mensagem);
void publicarTelemetria();
void atualizarLcd(const String &l1, const String &l2);
void setLed(bool r, bool g, bool b);
void buzzerTom(unsigned int frequencia);
void buzzerDesligar();
void registrarSenhaIncorreta(const char *origem);

// ============================================================
//  BUZZER - compativel com Arduino core 2.x e 3.x do ESP32
// ============================================================
#define BUZZER_CANAL 0

void buzzerIniciar() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_BUZZER, 2000, 8);
#else
  ledcSetup(BUZZER_CANAL, 2000, 8);
  ledcAttachPin(PIN_BUZZER, BUZZER_CANAL);
#endif
  buzzerDesligar();
}

void buzzerTom(unsigned int frequencia) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(PIN_BUZZER, frequencia);
#else
  ledcWriteTone(BUZZER_CANAL, frequencia);
#endif
}

void buzzerDesligar() {
  buzzerTom(0);
}

// ============================================================
//  LED RGB (catodo comum)
// ============================================================
void setLed(bool r, bool g, bool b) {
  digitalWrite(PIN_LED_R, r ? HIGH : LOW);
  digitalWrite(PIN_LED_G, g ? HIGH : LOW);
  digitalWrite(PIN_LED_B, b ? HIGH : LOW);
}

// ============================================================
//  UTILITARIO: repete um caractere N vezes
//  (a classe String do Arduino nao tem construtor de repeticao)
// ============================================================
String repetir(char c, uint8_t n) {
  String s = "";
  for (uint8_t i = 0; i < n; i++) s += c;
  return s;
}

// ============================================================
//  LCD - so reescreve quando o texto muda (evita flicker)
// ============================================================
void atualizarLcd(const String &l1, const String &l2) {
  if (l1 == lcdLinha1Atual && l2 == lcdLinha2Atual) return;

  lcdLinha1Atual = l1;
  lcdLinha2Atual = l2;

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(l1.substring(0, LCD_COLUNAS));
  lcd.setCursor(0, 1);
  lcd.print(l2.substring(0, LCD_COLUNAS));
}

// ============================================================
//  WIFI
// ============================================================
void conectarWifi() {
  atualizarLcd("Conectando Wi-Fi", "aguarde...");
  Serial.print("[WiFi] Conectando a ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_PASSWORD) == 0) {
    WiFi.begin(WIFI_SSID, NULL, WIFI_CHANNEL);
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 20000UL) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("[WiFi] Conectado. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[WiFi] Falha ao conectar.");
  }
}

// ============================================================
//  MQTT - recepcao de comandos do Dashboard
//  Payloads aceitos no topico .../comando:
//    ARMAR            -> arma o sistema (inicia delay de saida)
//    DESARMAR:1234    -> desarma somente com a senha correta
//    SILENCIAR        -> corta a sirene mas mantem o alarme registrado
//    PANICO           -> dispara o alarme imediatamente
//    TESTE            -> bipe curto de teste do buzzer
// ============================================================
void aoReceberMensagem(char *topico, byte *payload, unsigned int tamanho) {
  String msg;
  msg.reserve(tamanho);
  for (unsigned int i = 0; i < tamanho; i++) msg += (char)payload[i];
  msg.trim();
  msg.toUpperCase();

  Serial.printf("[MQTT] %s -> %s\n", topico, msg.c_str());

  if (msg == "ARMAR") {
    if (estado == DESARMADO) {
      mudarEstado(SAINDO, "DASHBOARD");
    } else {
      publicarAlerta("Comando ARMAR ignorado: sistema nao esta desarmado");
    }

  } else if (msg.startsWith("DESARMAR")) {
    int sep = msg.indexOf(':');
    String senha = (sep >= 0) ? msg.substring(sep + 1) : "";
    if (senha == senhaSistema) {
      mudarEstado(DESARMADO, "DASHBOARD");
    } else {
      publicarAlerta("Tentativa de desarme remoto com senha incorreta");
      registrarSenhaIncorreta("DASHBOARD");
    }

  } else if (msg == "SILENCIAR") {
    if (estado == ALARME) {
      sireneSilenciada = true;
      sireneLigada = false;
      digitalWrite(PIN_RELE, LOW);
      buzzerDesligar();
      publicarEvento("SIRENE_SILENCIADA", "DASHBOARD",
                     "Sirene silenciada pelo operador; alarme continua ativo");
    }

  } else if (msg == "PANICO") {
    ultimaOrigemAlarme = "PANICO";
    mudarEstado(ALARME, "DASHBOARD");
    publicarAlerta("BOTAO DE PANICO acionado pelo dashboard");

  } else if (msg == "TESTE") {
    buzzerTom(1500);
    tBuzzer = millis();
    buzzerLigado = true;
    publicarEvento("TESTE", "DASHBOARD", "Teste de buzzer solicitado");
  }
}

void conectarMqtt() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - tReconexao < 3000UL) return;
  tReconexao = millis();

  Serial.print("[MQTT] Conectando ao broker...");

  bool ok;
  // Last Will and Testament: se o ESP32 cair, o broker avisa "offline"
  if (strlen(MQTT_USER) == 0) {
    ok = mqtt.connect(DEVICE_ID, NULL, NULL,
                      TOPIC_CONEXAO, 1, true, "offline");
  } else {
    ok = mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS,
                      TOPIC_CONEXAO, 1, true, "offline");
  }

  if (ok) {
    Serial.println(" conectado!");
    mqtt.publish(TOPIC_CONEXAO, "online", true);
    mqtt.subscribe(TOPIC_COMANDO);
    publicarEstado();
    publicarEvento("BOOT", "SISTEMA", "Central conectada ao broker MQTT");
  } else {
    Serial.printf(" falhou (rc=%d)\n", mqtt.state());
  }
}

// ============================================================
//  PUBLICACOES
// ============================================================
void publicarEstado() {
  if (!mqtt.connected()) return;
  // retained: quem assinar depois ja recebe o estado atual
  mqtt.publish(TOPIC_STATUS, nomeEstado(estado), true);
}

void publicarEvento(const char *tipo, const char *origem, const char *descricao) {
  // o log sai sempre no Monitor Serial; o MQTT so recebe se estiver conectado
  JsonDocument doc;
  doc["dispositivo"] = DEVICE_ID;
  doc["local"]       = LOCAL_INSTALACAO;
  doc["tipo"]        = tipo;
  doc["origem"]      = origem;
  doc["estado"]      = nomeEstado(estado);
  doc["descricao"]   = descricao;
  doc["uptime_s"]    = millis() / 1000;

  char buffer[320];
  serializeJson(doc, buffer, sizeof(buffer));
  if (mqtt.connected()) mqtt.publish(TOPIC_EVENTO, buffer);

  Serial.printf("[EVENTO] %s\n", buffer);
}

void publicarAlerta(const char *mensagem) {
  if (mqtt.connected()) mqtt.publish(TOPIC_ALERTA, mensagem);
  Serial.printf("[ALERTA] %s\n", mensagem);
}

void publicarTelemetria() {
  if (!mqtt.connected()) return;

  JsonDocument doc;
  doc["estado"]      = nomeEstado(estado);
  doc["movimento"]   = movimentoDetectado;
  doc["porta"]       = portaAberta ? "ABERTA" : "FECHADA";
  doc["sirene"]      = sireneLigada;
  doc["tentativas"]  = tentativas;
  doc["rssi"]        = WiFi.RSSI();
  doc["uptime_s"]    = millis() / 1000;
  doc["origem_alarme"] = ultimaOrigemAlarme;
  if (!isnan(temperatura)) doc["temperatura"] = roundf(temperatura * 10) / 10;
  if (!isnan(umidade))     doc["umidade"]     = roundf(umidade * 10) / 10;
  doc["gas"] = roundf(gasPct * 10) / 10;

  char buffer[384];
  serializeJson(doc, buffer, sizeof(buffer));
  mqtt.publish(TOPIC_TELEMETRIA, buffer);
}

// ============================================================
//  TRANSICAO DE ESTADO
// ============================================================
void mudarEstado(EstadoSistema novo, const char *origem) {
  if (novo == estado) return;

  estado  = novo;
  tEstado = millis();

  switch (estado) {
    case DESARMADO:
      sireneLigada     = false;
      sireneSilenciada = false;
      tentativas       = 0;
      senhaDigitada    = "";
      digitalWrite(PIN_RELE, LOW);
      buzzerDesligar();
      setLed(false, true, false);              // verde
      publicarEvento("DESARMADO", origem, "Sistema desarmado");
      break;

    case SAINDO:
      setLed(false, false, true);              // azul
      publicarEvento("ARMANDO", origem, "Delay de saida iniciado");
      break;

    case ARMADO:
      senhaDigitada = "";
      setLed(false, false, true);              // azul
      publicarEvento("ARMADO", origem, "Vigilancia ativa");
      break;

    case ENTRANDO:
      publicarEvento("ENTRADA_DETECTADA", origem,
                     "Violacao detectada, aguardando senha");
      publicarAlerta("Entrada detectada - aguardando desarme");
      break;

    case ALARME: {
      sireneLigada     = !sireneSilenciada;
      digitalWrite(PIN_RELE, sireneLigada ? HIGH : LOW);
      setLed(true, false, false);              // vermelho
      publicarEvento("ALARME", origem, "ALARME DISPARADO");
      char msg[80];
      snprintf(msg, sizeof(msg), "ALARME DISPARADO - origem: %s",
               ultimaOrigemAlarme.c_str());
      publicarAlerta(msg);
      break;
    }

    case BLOQUEADO:
      buzzerDesligar();
      publicarEvento("BLOQUEIO", origem,
                     "Teclado bloqueado por excesso de tentativas");
      publicarAlerta("Teclado bloqueado: possivel tentativa de invasao");
      break;
  }

  publicarEstado();
  publicarTelemetria();
}

// ============================================================
//  LEITURA DOS SENSORES
// ============================================================

// Vigilancia ativa: armado, ou teclado bloqueado enquanto estava armado.
// Assim errar a senha 3 vezes nao "desliga" os sensores.
bool vigilanciaAtiva() {
  return estado == ARMADO ||
         (estado == BLOQUEADO && estadoAntesBloqueio == ARMADO);
}

// Violacao de PIR ou porta: armado -> delay de entrada;
// durante o bloqueio do teclado nao ha como desarmar -> alarme direto.
void violacao(const char *origem) {
  ultimaOrigemAlarme = origem;
  mudarEstado(estado == BLOQUEADO ? ALARME : ENTRANDO, origem);
}

void lerSensores() {
  // --- PIR ---
  if (millis() - tUltimoPir > DEBOUNCE_SENSOR_MS) {
    bool leitura = digitalRead(PIN_PIR) == HIGH;
    if (leitura != movimentoDetectado) {
      movimentoDetectado = leitura;
      tUltimoPir = millis();
      if (mqtt.connected()) {
        mqtt.publish(TOPIC_MOVIMENTO, movimentoDetectado ? "DETECTADO" : "LIVRE");
      }
      if (movimentoDetectado && vigilanciaAtiva()) violacao("PIR");
    }
  }

  // --- Reed switch da porta ---
  // INPUT_PULLUP: fechada (ima presente) = LOW ; aberta = HIGH
  if (millis() - tUltimaPorta > DEBOUNCE_SENSOR_MS) {
    bool leitura = digitalRead(PIN_PORTA) == HIGH;
    if (leitura != portaAberta) {
      portaAberta = leitura;
      tUltimaPorta = millis();
      if (mqtt.connected()) {
        mqtt.publish(TOPIC_PORTA, portaAberta ? "ABERTA" : "FECHADA");
      }
      if (portaAberta && vigilanciaAtiva()) violacao("PORTA");
    }
  }
}

// ============================================================
//  ZONA 24 H - TEMPERATURA (DHT22) E GAS/FUMACA (MQ-2)
//  Funciona em qualquer estado: incendio e vazamento de gas
//  disparam o alarme mesmo com o sistema desarmado.
// ============================================================
void dispararZona24h(const char *origem) {
  ultimaOrigemAlarme = origem;
  if (estado == ALARME) {
    publicarEvento("ALARME", origem, "Nova origem de alarme com alarme ativo");
  } else {
    mudarEstado(ALARME, origem);
  }
}

void lerAmbiente() {
  if (millis() - tAmbiente < INTERVALO_AMBIENTE_MS) return;
  tAmbiente = millis();

  // ---------- DHT22 ----------
  TempAndHumidity th = dht.getTempAndHumidity();
  if (dht.getStatus() == DHTesp::ERROR_NONE) {
    temperatura = th.temperature;
    umidade     = th.humidity;
  }

  if (!isnan(temperatura)) {
    if (!alarmeTemp && temperatura >= TEMP_ALARME_C) {
      alarmeTemp = true;
      char msg[80];
      snprintf(msg, sizeof(msg), "INCENDIO: temperatura de %.1f C", temperatura);
      publicarAlerta(msg);
      dispararZona24h("TEMP");
    } else if (alarmeTemp && temperatura < TEMP_ALARME_C - HISTERESE_TEMP_C) {
      alarmeTemp = false;
      publicarEvento("TEMPERATURA_NORMAL", "DHT22", "Temperatura abaixo do limite de alarme");
    }

    if (!alertaTemp && temperatura >= TEMP_ALERTA_C) {
      alertaTemp = true;
      char msg[80];
      snprintf(msg, sizeof(msg), "Temperatura elevada: %.1f C", temperatura);
      publicarAlerta(msg);
    } else if (alertaTemp && temperatura < TEMP_ALERTA_C - HISTERESE_TEMP_C) {
      alertaTemp = false;
    }
  }

  // ---------- MQ-2 ----------
  gasPct = analogRead(PIN_GAS) * 100.0f / 4095.0f;

  // Aquecimento: media da leitura em ar limpo vira a referencia,
  // como na calibracao de um modulo MQ-2 real.
  if (gasReferencia < 0) {
    somaGasAquecimento += gasPct;
    amostrasGas++;
    if (millis() >= AQUECIMENTO_GAS_MS) {
      gasReferencia = somaGasAquecimento / amostrasGas;
      char msg[80];
      snprintf(msg, sizeof(msg), "MQ-2 calibrado: referencia de %.1f%%", gasReferencia);
      publicarEvento("GAS_CALIBRADO", "MQ-2", msg);
    }
    return;
  }

  float subida = gasPct - gasReferencia;

  if (!alarmeGas && subida >= GAS_ALARME_PCT) {
    alarmeGas = true;
    char msg[80];
    snprintf(msg, sizeof(msg), "GAS/FUMACA detectado: %.1f%%", gasPct);
    publicarAlerta(msg);
    dispararZona24h("GAS");
  } else if (alarmeGas && subida < GAS_ALARME_PCT - HISTERESE_GAS_PCT) {
    alarmeGas = false;
    publicarEvento("GAS_NORMAL", "MQ-2", "Nivel de gas abaixo do limite de alarme");
  }

  if (!alertaGas && subida >= GAS_ALERTA_PCT) {
    alertaGas = true;
    char msg[80];
    snprintf(msg, sizeof(msg), "Nivel de gas elevado: %.1f%%", gasPct);
    publicarAlerta(msg);
  } else if (alertaGas && subida < GAS_ALERTA_PCT - HISTERESE_GAS_PCT) {
    alertaGas = false;
  }
}

// ============================================================
//  TECLADO
// ============================================================
void processarSenha() {
  if (senhaDigitada == senhaSistema) {
    tentativas = 0;
    senhaDigitada = "";

    if (estado == DESARMADO) {
      mudarEstado(SAINDO, "TECLADO");          // senha tambem arma
    } else {
      mudarEstado(DESARMADO, "TECLADO");
    }

  } else {
    senhaDigitada = "";

    // bipe curto de erro
    buzzerTom(400);
    tBuzzer = millis();
    buzzerLigado = true;

    registrarSenhaIncorreta("TECLADO");
  }
}

// Conta erros do teclado e do dashboard no mesmo contador.
void registrarSenhaIncorreta(const char *origem) {
  tentativas++;

  char msg[64];
  snprintf(msg, sizeof(msg), "Senha incorreta (%u/%u)", tentativas, MAX_TENTATIVAS);
  publicarEvento("SENHA_INCORRETA", origem, msg);

  if (tentativas < MAX_TENTATIVAS) return;
  tentativas = 0;

  if (estado == BLOQUEADO) {
    // erros remotos durante o bloqueio apenas reiniciam a contagem
    tEstado = millis();
    publicarAlerta("Novas senhas incorretas durante o bloqueio");
  } else if (estado == ENTRANDO || estado == ALARME) {
    // Quem invadiu e esta chutando a senha nao pode "pausar" o alarme
    // com o bloqueio: dispara (ou mantem) a sirene na hora.
    publicarAlerta("3 senhas incorretas durante a entrada - alarme imediato");
    ultimaOrigemAlarme = "SENHA";
    mudarEstado(ALARME, origem);
  } else {
    estadoAntesBloqueio = (estado == DESARMADO) ? DESARMADO : ARMADO;
    mudarEstado(BLOQUEADO, origem);
  }
}

void lerTeclado() {
  char tecla = teclado.getKey();
  if (!tecla) {
    // limpa digitacao abandonada
    if (senhaDigitada.length() > 0 &&
        millis() - tUltimaTecla > TIMEOUT_DIGITACAO_MS) {
      senhaDigitada = "";
    }
    return;
  }

  tUltimaTecla = millis();

  // Durante o bloqueio o teclado nao aceita nada
  if (estado == BLOQUEADO) {
    buzzerTom(300);
    tBuzzer = millis();
    buzzerLigado = true;
    return;
  }

  // feedback sonoro da tecla
  buzzerTom(1200);
  tBuzzer = millis();
  buzzerLigado = true;

  switch (tecla) {
    case '*':                                   // limpar
      senhaDigitada = "";
      break;

    case '#':                                   // confirmar
      processarSenha();
      break;

    case 'A':                                   // atalho: armar sem senha
      if (estado == DESARMADO) mudarEstado(SAINDO, "TECLADO");
      break;

    case 'B':                                   // atalho: panico
      ultimaOrigemAlarme = "PANICO";
      mudarEstado(ALARME, "TECLADO");
      break;

    default:
      if (senhaDigitada.length() < 8) senhaDigitada += tecla;
      break;
  }
}

// ============================================================
//  ATUALIZACAO DOS ESTADOS (temporizadores nao bloqueantes)
// ============================================================
void atualizarEstado() {
  unsigned long decorrido = millis() - tEstado;

  switch (estado) {

    case SAINDO:
      if (decorrido >= DELAY_SAIDA_MS) {
        mudarEstado(ARMADO, "TEMPORIZADOR");
      }
      break;

    case ENTRANDO:
      if (decorrido >= DELAY_ENTRADA_MS) {
        mudarEstado(ALARME, "TEMPORIZADOR");
      }
      break;

    case ALARME:
      // corta a sirene depois do tempo maximo, mas mantem o estado
      if (sireneLigada && decorrido >= DURACAO_SIRENE_MS) {
        sireneLigada = false;
        digitalWrite(PIN_RELE, LOW);
        publicarEvento("SIRENE_ENCERRADA", "TEMPORIZADOR",
                       "Tempo maximo de sirene atingido");
      }
      break;

    case BLOQUEADO:
      if (decorrido >= TEMPO_BLOQUEIO_MS) {
        tentativas = 0;
        publicarEvento("DESBLOQUEIO", "TEMPORIZADOR", "Teclado liberado");
        // volta ao estado em que estava antes do bloqueio
        mudarEstado(estadoAntesBloqueio, "TEMPORIZADOR");
      }
      break;

    default:
      break;
  }
}

// ============================================================
//  SINALIZACAO (LED + buzzer conforme o estado)
// ============================================================
void atualizarSinalizacao() {
  unsigned long agora = millis();
  unsigned long decorrido = agora - tEstado;

  // desliga bipes curtos (teclas / erro)
  if (buzzerLigado && estado != ALARME && estado != SAINDO && estado != ENTRANDO) {
    if (agora - tBuzzer > 80) {
      buzzerDesligar();
      buzzerLigado = false;
    }
  }

  switch (estado) {

    case DESARMADO:
      setLed(false, true, false);
      break;

    case SAINDO: {
      // bipe a cada segundo, acelerando no fim
      unsigned long restante = (DELAY_SAIDA_MS > decorrido)
                               ? (DELAY_SAIDA_MS - decorrido) : 0;
      unsigned long periodo = (restante < 5000UL) ? 300UL : 1000UL;
      bool ligar = (agora / (periodo / 2)) % 2 == 0;
      setLed(false, false, ligar);
      if (ligar) buzzerTom(1000); else buzzerDesligar();
      break;
    }

    case ARMADO:
      // pisca azul devagar: vigilancia ativa
      setLed(false, false, (agora / 1000) % 2 == 0);
      break;

    case ENTRANDO: {
      bool ligar = (agora / 250) % 2 == 0;
      setLed(true, false, false);
      if (ligar) buzzerTom(900); else buzzerDesligar();
      break;
    }

    case ALARME: {
      bool ligar = (agora / 200) % 2 == 0;
      setLed(ligar, false, false);
      if (sireneLigada) {
        buzzerTom(ligar ? 2200 : 1400);        // sirene bitonal
      } else {
        buzzerDesligar();
      }
      break;
    }

    case BLOQUEADO:
      setLed((agora / 500) % 2 == 0, (agora / 500) % 2 == 0, false); // amarelo
      break;
  }
}

// ============================================================
//  INTERFACE NO LCD
// ============================================================
void atualizarDisplay() {
  if (millis() - tLcd < 200) return;
  tLcd = millis();

  String l1, l2;
  unsigned long decorrido = millis() - tEstado;

  switch (estado) {

    case DESARMADO:
      l1 = "DESARMADO";
      l2 = senhaDigitada.length() > 0
           ? "Senha: " + repetir('*', senhaDigitada.length())
           : "A=armar  #=OK";
      break;

    case SAINDO: {
      unsigned long s = (DELAY_SAIDA_MS > decorrido)
                        ? (DELAY_SAIDA_MS - decorrido) / 1000 : 0;
      l1 = "SAINDO...";
      l2 = "Ativa em " + String(s + 1) + "s";
      break;
    }

    case ARMADO:
      l1 = "ARMADO";
      l2 = senhaDigitada.length() > 0
           ? "Senha: " + repetir('*', senhaDigitada.length())
           : "Digite a senha";
      break;

    case ENTRANDO: {
      unsigned long s = (DELAY_ENTRADA_MS > decorrido)
                        ? (DELAY_ENTRADA_MS - decorrido) / 1000 : 0;
      l1 = "!! ENTRADA !!";
      l2 = "Senha em " + String(s + 1) + "s";
      break;
    }

    case ALARME:
      l1 = "** ALARME **";
      l2 = "Origem: " + ultimaOrigemAlarme;
      break;

    case BLOQUEADO: {
      unsigned long s = (TEMPO_BLOQUEIO_MS > decorrido)
                        ? (TEMPO_BLOQUEIO_MS - decorrido) / 1000 : 0;
      l1 = "BLOQUEADO";
      l2 = "Aguarde " + String(s + 1) + "s";
      break;
    }
  }

  // indicador de comunicacao no canto da linha 1
  while (l1.length() < (LCD_COLUNAS - 1)) l1 += " ";
  l1 += mqtt.connected() ? "*" : "!";

  atualizarLcd(l1, l2);
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Sistema de Seguranca IoT - iniciando ===");

  // Sensores
  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_PORTA, INPUT_PULLUP);
  pinMode(PIN_GAS, INPUT);
  analogReadResolution(12);
  dht.setup(PIN_DHT, DHTesp::DHT22);

  // Atuadores
  pinMode(PIN_RELE, OUTPUT);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  digitalWrite(PIN_RELE, LOW);

  buzzerIniciar();
  setLed(false, true, false);

  // LCD
  Wire.begin(PIN_SDA, PIN_SCL);
  lcd.init();
  lcd.backlight();
  atualizarLcd("Sistema Seguranca", "Iniciando...");

  // Teclado
  teclado.setDebounceTime(40);

  // Rede
  conectarWifi();
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(aoReceberMensagem);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(60);   // folga para o simulador, que roda mais lento que o tempo real
  conectarMqtt();

  tEstado = millis();
  Serial.println("=== Pronto. Estado inicial: DESARMADO ===");
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  // Conectividade
  if (WiFi.status() != WL_CONNECTED) {
    conectarWifi();
  } else if (!mqtt.connected()) {
    conectarMqtt();
  } else {
    mqtt.loop();
  }

  // Logica do alarme
  lerSensores();
  lerAmbiente();
  lerTeclado();
  atualizarEstado();
  atualizarSinalizacao();
  atualizarDisplay();

  // Telemetria periodica (alimenta graficos e gauges do dashboard)
  if (millis() - tTelemetria >= INTERVALO_TELEMETRIA_MS) {
    tTelemetria = millis();
    publicarTelemetria();
    if (mqtt.connected()) {
      if (!isnan(temperatura)) mqtt.publish(TOPIC_TEMPERATURA, String(temperatura, 1).c_str());
      mqtt.publish(TOPIC_GAS, String(gasPct, 1).c_str());
    }
    Serial.printf("[SENSORES] estado=%s mov=%s porta=%s temp=%.1fC umid=%.1f%% gas=%.1f%% rssi=%d\n",
                  nomeEstado(estado), movimentoDetectado ? "SIM" : "NAO",
                  portaAberta ? "ABERTA" : "FECHADA", temperatura, umidade,
                  gasPct, WiFi.RSSI());
  }

  // Heartbeat: o Node-RED usa isso para dizer se a central esta online
  if (millis() - tHeartbeat >= INTERVALO_HEARTBEAT_MS) {
    tHeartbeat = millis();
    if (mqtt.connected()) mqtt.publish(TOPIC_CONEXAO, "online", true);
  }
}
