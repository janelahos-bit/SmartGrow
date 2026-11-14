/*
======================================================
SMART GROW
CONTROLE POR HISTERESE + SEGURANCA + MQTT/TLS
======================================================

SENSORES:
- HW-103: umidade do solo.
- DS18B20: temperatura.
- DHT11: umidade do ar.

ACIONAMENTO:
- HW383: controle da bomba.
- OLED: exibicao das informacoes.

CONTROLE POR HISTERESE:
- 3500 ou mais: liga a bomba.
- 2700 ou menos: desliga a bomba.
- Entre 2701 e 3499: mantem o estado anterior.

CLASSIFICACAO DA UMIDADE DO SOLO (ADC 12 BITS):
- 0 a 1299: Encharcado
- 1300 a 1899: Molhado
- 1900 a 2599: Umido
- 2600 ou mais: Seco

SEGURANCA:
- Detecta leitura anormalmente baixa do HW-103.
- Bloqueia a bomba em caso de falha identificada.
- Exige 5 leituras validas consecutivas para recuperar.
- Bomba desligada durante a falha.

RELE ATIVO EM LOW:
- LOW  = bomba ligada.
- HIGH = bomba desligada.
*/


// ----------------------------------------------------
// 1. BIBLIOTECAS
// ----------------------------------------------------

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// Rede e comunicacao MQTT segura
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>


// ----------------------------------------------------
// 2. WIFI E MQTT SOBRE TLS
// ----------------------------------------------------

// Preencha estes campos no Arduino IDE.
// Nao compartilhe suas senhas ou certificados privados.

const char* WIFI_SSID = "*";
const char* WIFI_PASSWORD = "*";

const char* MQTT_BROKER = "*";
const int MQTT_PORT = 8883;
const char* MQTT_USER = "*";
const char* MQTT_PASSWORD = "*";

const char* MQTT_TOPIC_TESTE = "smartgrow/teste";
const char* MQTT_TOPIC_SENSORES = "smartgrow/sensores";

// Intervalo de gravacao no banco de dados: 30 minutos (1.800.000 ms)
const unsigned long INTERVALO_ENVIO_SENSORES = 1800000;
unsigned long ultimoEnvioSensores = 0;

// Certificado publico da autoridade certificadora SmartGrow.
// Nao substitua por server.key nem por ca.key.
static const char CERTIFICADO_CA[] PROGMEM = R"SMARTGROW_CA(
-*-
)SMARTGROW_CA";

WiFiClientSecure clienteTLS;
PubSubClient clienteMQTT(clienteTLS);

unsigned long ultimaTentativaWiFi = 0;
unsigned long ultimaTentativaMQTT = 0;
unsigned long ultimoEnvioTesteMQTT = 0;

const unsigned long INTERVALO_RECONEXAO = 5000;
const unsigned long INTERVALO_TESTE_MQTT = 10000;


// ----------------------------------------------------
// 3. PINOS (ESP32-S3 N16R8)
// ----------------------------------------------------

const int PINO_UMIDADE = 1;
const int PINO_BOMBA = 18;

const int OLED_SDA = 8;
const int OLED_SCL = 9;

const int PINO_DHT = 4;
#define TIPO_DHT DHT11

const int PINO_DS18B20 = 5;


// ----------------------------------------------------
// 4. OBJETOS DOS SENSORES
// ----------------------------------------------------

DHT dht(PINO_DHT, TIPO_DHT);

OneWire oneWire(PINO_DS18B20);

DallasTemperature sensorTemperatura(&oneWire);


// ----------------------------------------------------
// 5. CONFIGURACAO DO OLED
// ----------------------------------------------------

#define LARGURA_OLED 128
#define ALTURA_OLED 64

Adafruit_SSD1306 display(
  LARGURA_OLED,
  ALTURA_OLED,
  &Wire,
  -1
);


// ----------------------------------------------------
// 6. PARAMETROS DE CONTROLE E FAIXAS DO SOLO
// ----------------------------------------------------

// Limites de acionamento da bomba por histerese
const int LIMITE_LIGA = 3500;
const int LIMITE_DESLIGA = 2700;

// Faixas de classificacao da umidade do solo (ADC)
const int LIMITE_ENCHARCADO = 1299; // 0 a 1299
const int LIMITE_MOLHADO = 1899;    // 1300 a 1899
const int LIMITE_UMIDO = 2599;      // 1900 a 2599
                                    // >= 2600: Seco

// Leitura abaixo deste valor indica falha suspeita
// conforme a faixa observada na calibracao do sensor.
const int LEITURA_MINIMA_VALIDA = 100;

// Quantidade de leituras validas para liberar o sistema
const int LEITURAS_RECUPERACAO = 5;


// ----------------------------------------------------
// 7. INTERVALOS DE MONITORAMENTO LOCAL
// ----------------------------------------------------

const unsigned long INTERVALO_LEITURA = 1000;

const unsigned long INTERVALO_DHT = 2500;

const unsigned long INTERVALO_DS = 1000;

// Tempo de conversao do DS18B20 em resolucao de 10 bits
const unsigned long TEMPO_CONVERSAO_DS = 200;


// ----------------------------------------------------
// 8. VARIAVEIS DE CONTROLE
// ----------------------------------------------------

// Estado da bomba
bool bombaLigada = false;

// Estado de seguranca do sensor de solo
bool falhaSensorSolo = false;
int leiturasValidas = 0;

// Valores dos sensores
int valorUmidade = 0;
float temperatura = 0.0;
float umidadeAr = 0.0;

// Validacao dos sensores
bool temperaturaValida = false;
bool umidadeArValida = false;

// Controle dos intervalos
unsigned long ultimaLeitura = 0;
unsigned long ultimaLeituraDHT = 0;
unsigned long ultimaSolicitacaoDS = 0;
unsigned long inicioConversaoDS = 0;

// Controle da conversao do DS18B20
bool conversaoPendenteDS = false;
bool primeiraLeituraDHT = true;


// ----------------------------------------------------
// 9. FUNCOES DE COMUNICACAO WI-FI E MQTT
// ----------------------------------------------------

void manterWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  unsigned long agora = millis();
  if (agora - ultimaTentativaWiFi < INTERVALO_RECONEXAO) return;

  ultimaTentativaWiFi = agora;
  Serial.println("Wi-Fi desconectado. Tentando reconectar...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void conectarMQTT() {
  if (WiFi.status() != WL_CONNECTED || clienteMQTT.connected()) return;

  unsigned long agora = millis();
  if (agora - ultimaTentativaMQTT < INTERVALO_RECONEXAO) return;

  ultimaTentativaMQTT = agora;
  Serial.print("Conectando ao MQTT/TLS... ");

  if (clienteMQTT.connect("SmartGrow-ESP32S3", MQTT_USER, MQTT_PASSWORD)) {
    Serial.println("CONECTADO!");
    clienteMQTT.publish(MQTT_TOPIC_TESTE, "ESP32-S3 conectado via MQTT/TLS");
  } else {
    Serial.print("FALHOU. Estado MQTT: ");
    Serial.println(clienteMQTT.state());
  }
}

void manterComunicacao() {
  manterWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    conectarMQTT();
    clienteMQTT.loop();
  }
}


// ----------------------------------------------------
// 10. FUNCAO DE SEGURANCA DA BOMBA
// ----------------------------------------------------

void desligarBomba() {
  digitalWrite(PINO_BOMBA, HIGH);
  bombaLigada = false;
}


// ----------------------------------------------------
// 11. CLASSIFICACAO DA UMIDADE DO SOLO
// ----------------------------------------------------

const char* classificarUmidadeSolo(int adc) {
  if (falhaSensorSolo) {
    return "Falha";
  }
  if (adc <= LIMITE_ENCHARCADO) {
    return "Encharcado";
  }
  if (adc <= LIMITE_MOLHADO) {
    return "Molhado";
  }
  if (adc <= LIMITE_UMIDO) {
    return "Umido";
  }
  return "Seco";
}


// ----------------------------------------------------
// 12. PUBLICACAO DOS DADOS VIA MQTT
// ----------------------------------------------------

void publicarDadosSensores() {
  if (!clienteMQTT.connected()) return;

  char temp[16];
  char umidAr[16];

  if (temperaturaValida) {
    snprintf(temp, sizeof(temp), "%.1f", temperatura);
  } else {
    strcpy(temp, "null");
  }

  if (umidadeArValida) {
    snprintf(umidAr, sizeof(umidAr), "%.1f", umidadeAr);
  } else {
    strcpy(umidAr, "null");
  }

  const char* statusSolo = classificarUmidadeSolo(valorUmidade);

  char payload[256];

  snprintf(
    payload,
    sizeof(payload),
    "{\"temperatura\":%s,\"umidade\":%s,\"umidade_solo_adc\":%d,\"status_solo\":\"%s\",\"bomba_ligada\":%s,\"falha_sensor_solo\":%s}",
    temp,
    umidAr,
    valorUmidade,
    statusSolo,
    bombaLigada ? "true" : "false",
    falhaSensorSolo ? "true" : "false"
  );

  if (clienteMQTT.publish(MQTT_TOPIC_SENSORES, payload)) {
    Serial.println("MQTT: dados dos sensores enviados!");
    Serial.println(payload);
  } else {
    Serial.println("ERRO: falha ao publicar dados MQTT!");
  }
}


// ----------------------------------------------------
// 13. LEITURA DO SENSOR DE SOLO
// ----------------------------------------------------

int lerUmidadeSolo() {
  long soma = 0;

  // Realiza 5 leituras para reduzir oscilacoes
  for (int i = 0; i < 5; i++) {
    soma += analogRead(PINO_UMIDADE);
    delayMicroseconds(2000);
  }

  return soma / 5;
}


// ----------------------------------------------------
// 14. VALIDACAO DO SENSOR DE SOLO
// ----------------------------------------------------

void validarSensorSolo() {
  // Verifica leitura fora da faixa calibrada inferior
  if (valorUmidade <= LEITURA_MINIMA_VALIDA) {
    falhaSensorSolo = true;
    leiturasValidas = 0;
    desligarBomba();
    return;
  }

  // Se existe falha registrada, exige leituras validas
  if (falhaSensorSolo) {
    leiturasValidas++;

    if (leiturasValidas >= LEITURAS_RECUPERACAO) {
      falhaSensorSolo = false;
      leiturasValidas = 0;
      Serial.println("SENSOR SOLO: LEITURA RECUPERADA!");
    }
  }
}


// ----------------------------------------------------
// 15. CONTROLE DA BOMBA POR HISTERESE
// ----------------------------------------------------

void controlarBomba() {
  // A falha do sensor tem prioridade sobre a irrigacao
  if (falhaSensorSolo) {
    desligarBomba();
    return;
  }

  // Condicao para ligar a bomba
  if (valorUmidade >= LIMITE_LIGA) {
    bombaLigada = true;
  }
  // Condicao para desligar a bomba
  else if (valorUmidade <= LIMITE_DESLIGA) {
    bombaLigada = false;
  }

  // Entre os limites, mantem o estado anterior
  if (bombaLigada) {
    digitalWrite(PINO_BOMBA, LOW);
  } else {
    digitalWrite(PINO_BOMBA, HIGH);
  }
}


// ----------------------------------------------------
// 16. LEITURA DO DS18B20 SEM BLOQUEIO
// ----------------------------------------------------

void atualizarTemperatura(unsigned long agora) {
  // Solicita uma nova conversao quando necessario
  if (!conversaoPendenteDS &&
      agora - ultimaSolicitacaoDS >= INTERVALO_DS) {
    sensorTemperatura.requestTemperatures();
    conversaoPendenteDS = true;
    inicioConversaoDS = agora;
    ultimaSolicitacaoDS = agora;
  }

  // Recupera o resultado apos o tempo de conversao
  if (conversaoPendenteDS &&
      agora - inicioConversaoDS >= TEMPO_CONVERSAO_DS) {
    temperatura = sensorTemperatura.getTempCByIndex(0);

    temperaturaValida =
      temperatura != DEVICE_DISCONNECTED_C &&
      !isnan(temperatura) &&
      temperatura >= -55.0 &&
      temperatura <= 125.0;

    conversaoPendenteDS = false;
  }
}


// ----------------------------------------------------
// 17. LEITURA DO DHT11
// ----------------------------------------------------

void atualizarUmidadeAr(unsigned long agora) {
  if (primeiraLeituraDHT ||
      agora - ultimaLeituraDHT >= INTERVALO_DHT) {
    umidadeAr = dht.readHumidity();

    umidadeArValida =
      !isnan(umidadeAr) &&
      umidadeAr >= 0.0 &&
      umidadeAr <= 100.0;

    ultimaLeituraDHT = agora;
    primeiraLeituraDHT = false;
  }
}


// ----------------------------------------------------
// 18. MONITOR SERIAL E ATUALIZACAO DO OLED
// ----------------------------------------------------

void atualizarSerial() {
  Serial.println();
  Serial.println("================================");
  Serial.print("Umidade do solo: ");
  Serial.print(classificarUmidadeSolo(valorUmidade));
  Serial.print(" (ADC: ");
  Serial.print(valorUmidade);
  Serial.println(")");

  if (falhaSensorSolo) {
    Serial.println("SENSOR SOLO: FALHA");
    Serial.println("BOMBA: BLOQUEADA");
    Serial.print("Leituras validas para recuperacao: ");
    Serial.println(leiturasValidas);
  } else {
    if (valorUmidade >= LIMITE_LIGA) {
      Serial.println("Acao: NECESSITA IRRIGACAO");
    } else if (valorUmidade <= LIMITE_DESLIGA) {
      Serial.println("Acao: SOLO ADEQUADO");
    } else {
      Serial.println("Acao: FAIXA DE HISTERESE");
    }

    if (bombaLigada) {
      Serial.println("Bomba: LIGADA");
    } else {
      Serial.println("Bomba: DESLIGADA");
    }
  }

  if (temperaturaValida) {
    Serial.print("Temperatura DS18B20: ");
    Serial.print(temperatura, 1);
    Serial.println(" C");
  } else {
    Serial.println("Erro na leitura do DS18B20");
  }

  if (umidadeArValida) {
    Serial.print("Umidade do ar DHT11: ");
    Serial.print(umidadeAr, 1);
    Serial.println(" %");
  } else {
    Serial.println("Erro na leitura do DHT11");
  }

  Serial.println("================================");
}

void atualizarOLED() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);

  display.println("SMART GROW");
  display.println("----------------");

  // Umidade do solo (Status + valor ADC)
  display.print("Solo: ");
  display.print(classificarUmidadeSolo(valorUmidade));
  display.print(" (");
  display.print(valorUmidade);
  display.println(")");

  // Estado da bomba
  display.print("Bomba: ");
  if (falhaSensorSolo) {
    display.println("BLOQUEADA");
  } else if (bombaLigada) {
    display.println("LIGADA");
  } else {
    display.println("DESLIGADA");
  }

  // Temperatura
  display.print("Temp: ");
  if (temperaturaValida) {
    display.print(temperatura, 1);
    display.println(" C");
  } else {
    display.println("ERRO");
  }

  // Umidade do ar
  display.print("Umid. ar: ");
  if (umidadeArValida) {
    display.print(umidadeAr, 1);
    display.println(" %");
  } else {
    display.println("ERRO");
  }

  // Estado do sensor de solo
  display.print("Sensor solo: ");
  if (falhaSensorSolo) {
    display.println("FALHA");
  } else {
    display.println("OK");
  }

  display.display();
}


// ----------------------------------------------------
// 19. SETUP E LOOP PRINCIPAL
// ----------------------------------------------------

void setup() {
  Serial.begin(115200);

  // Inicializa Wi-Fi e configura MQTT com validacao do certificado CA
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  clienteTLS.setCACert(CERTIFICADO_CA);
  clienteMQTT.setServer(MQTT_BROKER, MQTT_PORT);
  clienteMQTT.setSocketTimeout(5);
  clienteMQTT.setKeepAlive(30);

  Serial.println("Iniciando Wi-Fi e MQTT/TLS...");

  // Configura o ADC do ESP32-S3
  analogReadResolution(12);

  // Configura rele e desliga a bomba imediatamente
  pinMode(PINO_BOMBA, OUTPUT);
  desligarBomba();

  // Inicializa os sensores
  dht.begin();
  sensorTemperatura.begin();

  // Configura DS18B20 para conversao nao bloqueante
  sensorTemperatura.setResolution(10);
  sensorTemperatura.setWaitForConversion(false);

  // Inicializa comunicacao OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("ERRO: OLED nao encontrado!");
    while (true) {
      desligarBomba();
    }
  }

  // Tela inicial
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("SMART GROW");
  display.println("----------------");
  display.println("Inicializando...");
  display.println("Controle seguro");
  display.display();

  Serial.println("SMART GROW INICIADO!");
  Serial.println("Controle por histerese ativo.");
  Serial.println("Liga: 3500 | Desliga: 2700");
  Serial.println("Envio MQTT sensores: a cada 30 min.");
}

void loop() {
  manterComunicacao();

  unsigned long agora = millis();

  // Executa o monitoramento local a cada 1 segundo
  if (agora - ultimaLeitura >= INTERVALO_LEITURA) {
    ultimaLeitura = agora;

    // 1. Leitura do solo
    valorUmidade = lerUmidadeSolo();

    // 2. Validacao do sensor
    validarSensorSolo();

    // 3. Controle da bomba
    controlarBomba();

    // 4. Atualizacao da temperatura
    atualizarTemperatura(agora);

    // 5. Atualizacao da umidade do ar
    atualizarUmidadeAr(agora);

    // 6. Monitor Serial
    atualizarSerial();

    // 7. Atualizacao do OLED
    atualizarOLED();
  }

  // Conversao do DS18B20 continua sendo verificada
  // independentemente do ciclo de atualizacao do OLED.
  atualizarTemperatura(millis());

  // Publica mensagem simples de keep-alive/teste a cada 10 segundos.
  if (clienteMQTT.connected() &&
      millis() - ultimoEnvioTesteMQTT >= INTERVALO_TESTE_MQTT) {
    ultimoEnvioTesteMQTT = millis();
    clienteMQTT.publish(MQTT_TOPIC_TESTE, "SmartGrow ESP32-S3 ativo");
  }

  // Publica os dados reais dos sensores a cada 30 minutos (1.800.000 ms).
  if (clienteMQTT.connected() &&
      millis() - ultimoEnvioSensores >= INTERVALO_ENVIO_SENSORES) {
    ultimoEnvioSensores = millis();
    publicarDadosSensores();
  }
}