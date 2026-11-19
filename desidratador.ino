/*
  DESIDRATADOR - hotend 24 V acionado por rele
  Sensores: DHT11 (ar da camara) + NTC 100k 3950 (bloco do hotend)
  Display:  OLED SSD1306 0,96" 128x64 I2C

  Ligacoes (Arduino Uno / Nano):
    Rele IN ............. D7
    DHT11 DATA .......... D4
    NTC (divisor 4,7k) .. A0
    OLED SDA ............ A4
    OLED SCL ............ A5
    OLED VCC ............ 5V
    OLED GND ............ GND

  Bibliotecas (Ferramentas > Gerenciar Bibliotecas):
    - "DHT sensor library" (Adafruit)  + "Adafruit Unified Sensor"
    - "Adafruit SSD1306"               + "Adafruit GFX Library"
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "DHT.h"

// ---------------- Pinos ----------------
const int PINO_RELE = 7;    // IN1 do modulo rele
const int PINO_DHT  = 4;    // DATA do DHT11
const int PINO_NTC  = A0;   // ponto medio do divisor do termistor
// O OLED usa o I2C fixo do Uno/Nano: SDA = A4, SCL = A5

// ---------------- OLED ----------------
const uint8_t OLED_LARGURA  = 128;
const uint8_t OLED_ALTURA   = 64;
const uint8_t OLED_ENDERECO = 0x3C;   // a maioria usa 0x3C; alguns usam 0x3D
Adafruit_SSD1306 oled(OLED_LARGURA, OLED_ALTURA, &Wire, -1);  // -1 = sem pino RESET
bool oledOk = false;

// ---------------- DHT ----------------
#define DHTTYPE DHT11
DHT dht(PINO_DHT, DHTTYPE);

// ---------------- Logica do rele (modulo com optoacoplador = invertido) ----------------
const int LIGAR    = LOW;    // LOW aciona o rele (liga o cartucho)
const int DESLIGAR = HIGH;   // HIGH solta o rele (desliga)

// ---------------- Termistor NTC (divisor) ----------------
const float R_PULL = 4700.0;    // resistor de topo (ao 5V)
const float R0     = 100000.0;  // NTC a 25 C
const float T0     = 298.15;    // 25 C em Kelvin
const float BETA   = 3950.0;    // Beta tipico de NTC 100k 3950 (calibrar se preciso)

// ---------------- Alvo + histerese (AR / DHT11) ----------------
const float LIGA_EM    = 49.0;  // ar caiu ate 49 C  -> religa o bloco
const float DESLIGA_EM = 55.0;  // ar chegou a 55 C  -> desliga o bloco
                                // entre 49 e 55 C mantem o estado (histerese)

// ---------------- Seguranca do bloco (NTC) ----------------
const float MAX_BLOCO    = 70.0;   // corte total: o bloco nunca passa disso
const float RETOMA_BLOCO = 65.0;   // depois do corte, so volta a aquecer abaixo disso
                                   // (evita o rele ficar ligando/desligando em 70 C)

// ---------------- Tempos ----------------
const unsigned long INTERVALO_BLOCO = 500;   // ms: checa seguranca do bloco
const unsigned long INTERVALO_DHT   = 2000;  // ms: DHT11 le no max a cada 2 s
const unsigned long INTERVALO_TELA  = 1000;  // ms: atualiza o OLED

unsigned long ultimoBloco = 0;
unsigned long ultimoDHT   = 0;
unsigned long ultimaTela  = 0;

float tempCamara = NAN;   // DHT11 (ar)
float umidade    = NAN;
float tempBloco  = NAN;   // NTC (bloco)
bool  aquecendo  = false; // estado atual do rele (para a histerese)

// Estado mostrado no display
enum Estado { EST_DESLIGADO, EST_AQUECENDO, EST_FALHA, EST_LIMITE };
Estado estado = EST_DESLIGADO;

// =====================================================================
float lerBloco() {
  int bruto = analogRead(PINO_NTC);
  if (bruto < 5 || bruto > 1018) return NAN;   // fio solto / curto
  float R = R_PULL * (float)bruto / (1023.0 - bruto);
  return 1.0 / (1.0 / T0 + (1.0 / BETA) * log(R / R0)) - 273.15;
}

void desligaAgora() {
  aquecendo = false;
  digitalWrite(PINO_RELE, DESLIGAR);
}

// ---------------- Funcoes do display ----------------
void imprimeValor(float v, uint8_t casas) {
  if (isnan(v)) oled.print(F("--"));
  else          oled.print(v, casas);
}

void imprimeGrauC() {
  oled.write(248);   // simbolo de grau (tabela CP437)
  oled.print('C');
}

void telaInicial() {
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(2);
  oled.setCursor(4, 8);
  oled.print(F("DESIDRA-"));
  oled.setCursor(4, 26);
  oled.print(F("TADOR"));
  oled.setTextSize(1);
  oled.setCursor(4, 50);
  oled.print(F("iniciando..."));
  oled.display();
}

void atualizaTela() {
  if (!oledOk) return;

  oled.clearDisplay();

  // Barra de status: texto preto sobre faixa branca
  oled.fillRect(0, 0, OLED_LARGURA, 11, SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_BLACK);
  oled.setCursor(2, 2);
  switch (estado) {
    case EST_AQUECENDO: oled.print(F("AQUECENDO"));    break;
    case EST_DESLIGADO: oled.print(F("DESLIGADO"));    break;
    case EST_FALHA:     oled.print(F("FALHA SENSOR")); break;
    case EST_LIMITE:    oled.print(F("LIMITE BLOCO")); break;
  }
  oled.setCursor(80, 2);
  oled.print(aquecendo ? F("RELE ON") : F("RELE OFF"));

  oled.setTextColor(SSD1306_WHITE);

  // Temperatura do ar (variavel controlada) em destaque
  oled.setCursor(0, 19);
  oled.print(F("AR"));
  oled.setTextSize(2);
  oled.setCursor(22, 15);
  imprimeValor(tempCamara, 1);
  oled.setTextSize(1);
  oled.setCursor(oled.getCursorX() + 2, 15);
  imprimeGrauC();

  // Umidade
  oled.setCursor(0, 35);
  oled.print(F("Umidade: "));
  imprimeValor(umidade, 0);
  oled.print(F(" %"));

  // Bloco (NTC) / limite
  oled.setCursor(0, 45);
  oled.print(F("Bloco:   "));
  imprimeValor(tempBloco, 1);
  imprimeGrauC();
  oled.print(F(" /"));
  oled.print(MAX_BLOCO, 0);

  // Faixa de controle
  oled.setCursor(0, 55);
  oled.print(F("Faixa:   "));
  oled.print(LIGA_EM, 0);
  oled.print('-');
  oled.print(DESLIGA_EM, 0);
  imprimeGrauC();

  oled.display();
}

// ---------------- Controle (mesma logica de antes) ----------------
void controla() {
  // 1) Falha de sensor -> desliga
  if (isnan(tempBloco) || isnan(tempCamara)) {
    if (estado != EST_FALHA) Serial.println(F("[FALHA SENSOR] desligado por seguranca"));
    desligaAgora();
    estado = EST_FALHA;
    return;
  }

  // 2) Bloco quente demais -> corte duro (o NTC manda)
  //    So libera de novo quando o bloco esfriar ate RETOMA_BLOCO.
  if (tempBloco >= MAX_BLOCO || (estado == EST_LIMITE && tempBloco > RETOMA_BLOCO)) {
    if (estado != EST_LIMITE) Serial.println(F("[LIMITE BLOCO] cortando aquecimento"));
    desligaAgora();
    estado = EST_LIMITE;
    return;
  }

  // 3) Histerese pelo AR da camara
  if (!aquecendo && tempCamara <= LIGA_EM) {
    aquecendo = true;
    digitalWrite(PINO_RELE, LIGAR);
  } else if (aquecendo && tempCamara >= DESLIGA_EM) {
    aquecendo = false;
    digitalWrite(PINO_RELE, DESLIGAR);
  }
  // entre 49 e 55 C mantem o estado atual (poupa o rele)

  estado = aquecendo ? EST_AQUECENDO : EST_DESLIGADO;
}

// =====================================================================
void setup() {
  Serial.begin(9600);
  pinMode(PINO_RELE, OUTPUT);
  desligaAgora();               // SEGURANCA: nasce desligado
  dht.begin();

  // OLED - o begin() ja inicia o I2C (Wire)
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ENDERECO);
  // Se o I2C travar (fio solto, ruido do rele), o Arduino nao fica preso:
  // desiste apos 3 ms e reinicia o barramento. O controle continua rodando.
  Wire.setWireTimeout(3000, true);
  if (oledOk) {
    Wire.beginTransmission(OLED_ENDERECO);       // o display respondeu?
    oledOk = (Wire.endTransmission() == 0);
  }
  if (oledOk) {
    oled.cp437(true);           // tabela de caracteres correta (simbolo de grau)
    // oled.setRotation(2);     // descomente se a tela ficar de cabeca para baixo
    telaInicial();
  } else {
    Serial.println(F("[OLED] nao encontrado - seguindo sem display"));
  }

  Serial.println(F("=========================================="));
  Serial.println(F("  DESIDRATADOR (hotend 24 V / RELE) INICIADO"));
  Serial.println(F("=========================================="));
  Serial.print(F("Liga <= ")); Serial.print(LIGA_EM);
  Serial.print(F(" C | Desliga >= ")); Serial.print(DESLIGA_EM); Serial.println(F(" C"));
  Serial.print(F("Corte do bloco: ")); Serial.print(MAX_BLOCO);
  Serial.print(F(" C (retoma < ")); Serial.print(RETOMA_BLOCO); Serial.println(F(" C)"));
  delay(2000);
}

void loop() {
  unsigned long agora = millis();

  // --- Leitura do AR (DHT11) + telemetria a cada 2 s ---
  if (agora - ultimoDHT >= INTERVALO_DHT) {
    ultimoDHT = agora;
    umidade    = dht.readHumidity();
    tempCamara = dht.readTemperature();

    Serial.print(aquecendo ? F("[AQUECENDO] ") : F("[DESLIGADO] "));
    Serial.print(F("Ar: "));
    isnan(tempCamara) ? Serial.print(F("--")) : Serial.print(tempCamara, 1);
    Serial.print(F(" C | Umid: "));
    isnan(umidade) ? Serial.print(F("--")) : Serial.print(umidade, 0);
    Serial.print(F(" % | Bloco: "));
    isnan(tempBloco) ? Serial.print(F("--")) : Serial.print(tempBloco, 1);
    Serial.println(F(" C"));
  }

  // --- Seguranca do bloco + decisao a cada 500 ms ---
  if (agora - ultimoBloco >= INTERVALO_BLOCO) {
    ultimoBloco = agora;
    tempBloco = lerBloco();
    controla();
  }

  // --- Display a cada 1 s (depois do controle, nunca antes) ---
  if (agora - ultimaTela >= INTERVALO_TELA) {
    ultimaTela = agora;
    atualizaTela();
  }
}
