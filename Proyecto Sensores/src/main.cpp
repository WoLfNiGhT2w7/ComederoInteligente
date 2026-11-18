#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RTClib.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <math.h>
#include <ESP32Servo.h>
#include <WebServer.h>

const char* WIFI_SSID = "Comedero_ESP32";
const char* WIFI_PASS = "Comedero123";
const char* URL_NIVELES = "http://192.168.4.1/niveles";
const char* URL_HORA = "http://192.168.4.1/hora";
const char* URL_HORARIOS = "http://192.168.4.1/api/horarios";
const char* URL_SERVIDOR = "http://192.168.4.3:5000";
const char* URL_RECONOCER = "http://192.168.4.3:5000/reconocer";
const char* URL_DETECCIONES = "http://192.168.4.3:5000/detecciones";

#define TRIG_1 25
#define ECHO_1 34
#define TRIG_2 26
#define ECHO_2 35
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_ADDR 0x3C
#define MICROFONO 36
#define SERVO_1_PIN 13
#define SERVO_2_PIN 14

// Indicadores fisicos (solo el sensor 1 controla el LED rojo)
#define LED_VERDE 18
#define LED_AMARILLO 19
#define LED_ROJO 23
const int UMBRAL_NIVEL_BAJO = 20; // Porcentaje del deposito 1
const unsigned long SEMIPERIODO_ROJO_MS = 500; // 1 parpadeo/segundo
const unsigned long SEMIPERIODO_AMARILLO_MS = 150;
bool dispensandoAlimento = false;

Servo servo1;
Servo servo2;

// Servidor que recibe las ordenes de la pagina web de ESP32-CAM
WebServer servidorLocal(80);
IPAddress IP_LOCAL(192, 168, 4, 2);
IPAddress GATEWAY_LOCAL(192, 168, 4, 1);
IPAddress SUBNET_LOCAL(255, 255, 255, 0);

const int POS_CERRADO = 60;
const int POS_ABIERTO = 120;
const int TIEMPO_ABIERTO = 1000;

Adafruit_SSD1306 oled(128, 64, &Wire, -1);
RTC_DS3231 rtc;
Preferences prefs;

// Calibracion: 2 cm = lleno (100%), 12 cm = vacio (0%).
const float LLENO_1 = 2.0f, VACIO_1 = 12.0f;
const float LLENO_2 = 2.0f, VACIO_2 = 12.0f;
const unsigned long INTERVALO_NIVELES = 2000UL;
const unsigned long INTERVALO_PANTALLA = 500UL;
const unsigned long INTERVALO_WIFI = 10000UL;
const unsigned long INTERVALO_HORARIOS = 10000UL;
const unsigned long INTERVALO_RTC = 86400000UL;
const unsigned long REINTENTO_RTC = 30000UL;
const unsigned long INTERVALO_VERIFICACION = 1000UL;
// YOLO autoriza la alimentacion programada; las ordenes manuales siguen independientes.
const unsigned long INTERVALO_YOLO = 2000UL;
const unsigned long VIGENCIA_YOLO = 6000UL;
unsigned long ultimaConsultaYOLO = 0;
unsigned long ultimaRespuestaYOLO = 0;
bool primeraConsultaYOLO = true;
bool yoloConectado = false;
bool mascota1Detectada = false;
bool mascota2Detectada = false;
float confianzaMascota1 = 0.0f;
float confianzaMascota2 = 0.0f;



int medirSonido() {
    int minimo = 4095;
    int maximo = 0;

    unsigned long inicio = millis();

    while (millis() - inicio < 50) {
        int lectura = analogRead(MICROFONO);

        if (lectura < minimo) minimo = lectura;
        if (lectura > maximo) maximo = lectura;
    }

    return maximo - minimo;
}

struct Horario {
  uint8_t mascota;
  uint8_t hora;
  uint8_t minuto;
};
const int MAX_HORARIOS = 16;
Horario horarios[MAX_HORARIOS];
int cantidadHorarios = 0;

float distancia1 = -1, distancia2 = -1;
int porcentaje1 = -1, porcentaje2 = -1;
bool rtcDisponible = false, rtcSincronizado = false;
bool ultimoEnvioCorrecto = false;
unsigned long ultimoEnvio = 0, ultimaPantalla = 0;
unsigned long ultimoIntentoWiFi = 0, ultimaConsultaHorarios = 0;
unsigned long ultimoIntentoRTC = 0, ultimaSincronizacion = 0;
unsigned long ultimaVerificacion = 0;
bool primerIntentoRTC = true;

// Se actualizan sin delay para no bloquear las demas funciones.
void actualizarLEDs() {
  const unsigned long ahoraLED = millis();
  digitalWrite(LED_VERDE, WiFi.status() == WL_CONNECTED ? HIGH : LOW);

  // Solo porcentaje1: porcentaje2 no afecta a los indicadores.
  if (porcentaje1 < 0) {
    digitalWrite(LED_ROJO, ((ahoraLED / SEMIPERIODO_ROJO_MS) % 2) ? LOW : HIGH);
  } else {
    digitalWrite(LED_ROJO, porcentaje1 <= UMBRAL_NIVEL_BAJO ? HIGH : LOW);
  }

  digitalWrite(LED_AMARILLO,
    dispensandoAlimento && ((ahoraLED / SEMIPERIODO_AMARILLO_MS) % 2 == 0)
      ? HIGH : LOW);
}

// Mantiene el parpadeo amarillo durante el movimiento de los servos.
void esperarDispensacion(unsigned long duracion) {
  unsigned long inicio = millis();
  while (millis() - inicio < duracion) {
    actualizarLEDs();
    delay(10);
  }
}

// Clave persistente unica por mascota + hora + minuto.
String claveAlimentacion(const Horario& h) {
  char clave[16];
  snprintf(clave, sizeof(clave), "a%d_%02d%02d", h.mascota, h.hora, h.minuto);
  return String(clave);
}

void conectarWiFi() {
  Serial.println("Conectando al AP de la ESP32-CAM...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

float medirDistancia(int trig, int echo) {
  digitalWrite(trig, LOW);
  delayMicroseconds(2);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);
  unsigned long duracion = pulseIn(echo, HIGH, 30000);
  if (duracion == 0) return -1;
  return duracion * 0.0343f / 2.0f;
}

int calcularPorcentaje(float distancia, float lleno, float vacio) {
  if (!isfinite(distancia) || distancia < 0 || vacio <= lleno) return -1;
  return constrain((int)roundf((vacio - distancia) * 100.0f / (vacio - lleno)), 0, 100);
}

void actualizarSensores() {
  distancia1 = medirDistancia(TRIG_1, ECHO_1);
  delay(100);
  distancia2 = medirDistancia(TRIG_2, ECHO_2);
  porcentaje1 = calcularPorcentaje(distancia1, LLENO_1, VACIO_1);
  porcentaje2 = calcularPorcentaje(distancia2, LLENO_2, VACIO_2);
  if (rtcDisponible) {
    DateTime t = rtc.now();
    Serial.printf("%02d/%02d/%04d %02d:%02d:%02d | D1: %.1f cm (%d%%) | D2: %.1f cm (%d%%)\n",
      t.day(), t.month(), t.year(), t.hour(), t.minute(), t.second(),
      distancia1, porcentaje1, distancia2, porcentaje2);
  }
}

void enviarNiveles() {
  if (WiFi.status() != WL_CONNECTED) {
    ultimoEnvioCorrecto = false;
    return;
  }
  HTTPClient http;
  http.begin(URL_NIVELES);
  http.setTimeout(1500);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String datos = "d1=" + String(distancia1, 2) + "&d2=" + String(distancia2, 2);
  int respuesta = http.POST(datos);
  ultimoEnvioCorrecto = respuesta == 200;
  http.end();
}

bool sincronizarRTCdesdeCAM() {
  if (!rtcDisponible || WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.begin(URL_HORA);
  http.setTimeout(2000);
  int codigo = http.GET();
  if (codigo != 200) {
    Serial.printf("Hora no disponible, HTTP: %d\n", codigo);
    http.end();
    return false;
  }
  String respuesta = http.getString();
  http.end();
  int y, mo, d, h, mi, s;
  if (sscanf(respuesta.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6 ||
      y < 2025 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
      h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59) return false;
  DateTime fecha(y, mo, d, h, mi, s);
  if (!fecha.isValid()) return false;
  rtc.adjust(fecha);
  Serial.printf("RTC sincronizado: %04d-%02d-%02d %02d:%02d:%02d\n", y, mo, d, h, mi, s);
  return true;
}

// Conserva una copia de los horarios para reinicios o cortes de WiFi.
void guardarHorariosLocales() {
  if (cantidadHorarios == 0) prefs.remove("lista");
  else prefs.putBytes("lista", horarios, cantidadHorarios * sizeof(Horario));
}

void cargarHorariosLocales() {
  size_t n = prefs.getBytesLength("lista");
  if (!n || n > sizeof(horarios) || n % sizeof(Horario)) return;
  Horario tmp[MAX_HORARIOS] = {};
  if (prefs.getBytes("lista", tmp, n) != n) return;
  int cantidad = n / sizeof(Horario);
  for (int i = 0; i < cantidad; i++) {
    if (tmp[i].mascota < 1 || tmp[i].mascota > 2 || tmp[i].hora > 23 || tmp[i].minuto > 59) return;
  }
  memcpy(horarios, tmp, n);
  cantidadHorarios = cantidad;
  Serial.printf("Horarios recuperados: %d\n", cantidadHorarios);
}

bool descargarHorarios() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.begin(URL_HORARIOS);
  http.setTimeout(2500);
  int codigo = http.GET();
  if (codigo != 200) {
    Serial.printf("Error consultando horarios, HTTP: %d\n", codigo);
    http.end();
    return false;
  }
  String cuerpo = http.getString();
  http.end();
  StaticJsonDocument<2048> doc;
  DeserializationError error = deserializeJson(doc, cuerpo);
  if (error || !doc.is<JsonArray>()) {
    Serial.println("JSON de horarios invalido");
    return false;
  }
  JsonArray arr = doc.as<JsonArray>();
  if (arr.size() > MAX_HORARIOS) return false;
  Horario nuevos[MAX_HORARIOS] = {};
  int cantidad = 0;
  for (JsonVariant v : arr) {
    if (!v.is<JsonObject>()) return false;
    JsonObject obj = v.as<JsonObject>();
    if (!obj["mascota"].is<int>() || !obj["hora"].is<const char*>()) return false;
    int mascota = obj["mascota"].as<int>();
    const char* horaTexto = obj["hora"].as<const char*>();
    if (!horaTexto || strlen(horaTexto) != 5 || horaTexto[2] != ':' ||
        horaTexto[0] < '0' || horaTexto[0] > '9' ||
        horaTexto[1] < '0' || horaTexto[1] > '9' ||
        horaTexto[3] < '0' || horaTexto[3] > '9' ||
        horaTexto[4] < '0' || horaTexto[4] > '9') return false;
    int h = (horaTexto[0]-'0')*10 + (horaTexto[1]-'0');
    int m = (horaTexto[3]-'0')*10 + (horaTexto[4]-'0');
    if (mascota < 1 || mascota > 2 || h > 23 || m > 59) return false;
    nuevos[cantidad++] = {(uint8_t)mascota, (uint8_t)h, (uint8_t)m};
  }
  bool cambio = cantidad != cantidadHorarios || memcmp(horarios, nuevos, cantidad * sizeof(Horario)) != 0;
  memcpy(horarios, nuevos, cantidad * sizeof(Horario));
  cantidadHorarios = cantidad;
  if (cambio) {
    guardarHorariosLocales();
    Serial.printf("Horarios actualizados: %d\n", cantidadHorarios);
  }
  return true;
}


bool alimentarMascota(uint8_t mascota) {

  if (mascota == 1) {
    Serial.println("Alimentando mascota 1");

    dispensandoAlimento = true;
    // Abrir servomotor 1
    servo1.write(POS_ABIERTO);
    esperarDispensacion(TIEMPO_ABIERTO);

    // Cerrar servomotor 1
    servo1.write(POS_CERRADO);
    dispensandoAlimento = false;
    actualizarLEDs();

    Serial.println("Alimentacion mascota 1 completada");
    return true;
  }

  else if (mascota == 2) {
    Serial.println("Alimentando mascota 2");

    dispensandoAlimento = true;
    // Abrir servomotor 2
    servo2.write(POS_ABIERTO);
    esperarDispensacion(TIEMPO_ABIERTO);

    // Cerrar servomotor 2
    servo2.write(POS_CERRADO);
    dispensandoAlimento = false;
    actualizarLEDs();

    Serial.println("Alimentacion mascota 2 completada");
    return true;
  }

  Serial.println("ERROR: Mascota no valida");
  return false;
}


// Recibir las ordenes manuales enviadas por la ESP32-CAM.
// La ruta coincide con http://192.168.4.2/alimentar?n=1 (o 2).
void configurarServidorLocal() {
  servidorLocal.on("/alimentar", HTTP_POST, []() {
    if (!servidorLocal.hasArg("n")) {
      servidorLocal.send(400, "text/plain", "Falta numero de mascota");
      return;
    }

    String numero = servidorLocal.arg("n");
    if (numero != "1" && numero != "2") {
      servidorLocal.send(400, "text/plain", "Mascota invalida");
      return;
    }

    uint8_t mascota = (uint8_t)numero.toInt();
    bool exito = alimentarMascota(mascota);
    servidorLocal.send(exito ? 200 : 409, "text/plain",
                       exito ? "Alimentacion completada" : "No se pudo alimentar");
  });

  servidorLocal.begin();
  Serial.println("Servidor HTTP local iniciado: http://192.168.4.2");
}

// Un horario vence al finalizar su dia. Si la mascota llega tarde,
// la comida queda pendiente hasta las 23:59:59 de ese mismo dia.
// La fecha de la ultima comida de cada horario se guarda en NVS.
void verificarHorarios() {
  if (!rtcDisponible || !rtcSincronizado || WiFi.status() != WL_CONNECTED) return;

  // Nunca autorizar una comida con detecciones ausentes o antiguas.
  if (!yoloConectado || ultimaRespuestaYOLO == 0 ||
      millis() - ultimaRespuestaYOLO > VIGENCIA_YOLO) return;

  DateTime ahora = rtc.now();
  uint32_t fecha = (uint32_t)ahora.year() * 10000UL +
                   (uint32_t)ahora.month() * 100UL + ahora.day();
  uint16_t minutoActual = (uint16_t)ahora.hour() * 60U + ahora.minute();

  // Se procesa como maximo una alimentacion en cada comprobacion.
  // Si ambas mascotas tienen horarios pendientes, se atenderan
  // en comprobaciones sucesivas, sin mezclar los servomotores.
  for (int i = 0; i < cantidadHorarios; i++) {
    const Horario& h = horarios[i];
    uint16_t minutoProgramado = (uint16_t)h.hora * 60U + h.minuto;

    // Aun no ha llegado el horario de hoy.
    if (minutoActual < minutoProgramado) continue;

    // No repetir una alimentacion programada ya realizada hoy.
    String clave = claveAlimentacion(h);
    if (prefs.getUInt(clave.c_str(), 0) == fecha) continue;

    bool presente = (h.mascota == 1) ? mascota2Detectada : mascota1Detectada;
    if (!presente) continue;

    Serial.printf("Horario pendiente autorizado por YOLO: mascota %u, %02u:%02u\n",
                  (unsigned)h.mascota, (unsigned)h.hora, (unsigned)h.minuto);

    if (alimentarMascota(h.mascota)) {
      // Guardar solo despues de completar el movimiento del servo.
      size_t guardados = prefs.putUInt(clave.c_str(), fecha);
      if (guardados == 0) {
        Serial.println("ADVERTENCIA: no se pudo guardar el registro de comida");
      } else {
        Serial.printf("Comida registrada: mascota %u, %02u:%02u, fecha %lu\n",
                      (unsigned)h.mascota, (unsigned)h.hora,
                      (unsigned)h.minuto, (unsigned long)fecha);
      }
    }
    break;
  }
}

// Consulta al servidor Flask; las detecciones autorizan horarios pendientes.
void consultarYOLO() {
  // Estado seguro si el servidor deja de responder.
  yoloConectado = false;
  mascota1Detectada = false;
  mascota2Detectada = false;
  confianzaMascota1 = 0.0f;
  confianzaMascota2 = 0.0f;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("YOLO: WiFi desconectado");
    return;
  }

  WiFiClient cliente;
  HTTPClient http;
  http.setTimeout(1500);
  if (!http.begin(cliente, URL_DETECCIONES)) {
    Serial.println("YOLO: no se pudo iniciar la consulta");
    return;
  }

  int codigo = http.GET();
  if (codigo == HTTP_CODE_OK) {
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, http.getStream());
    if (!error && doc["camara_conectada"].is<bool>() &&
        doc["deteccion_estable"].is<bool>() &&
        doc["peluche_1"].is<bool>() && doc["peluche_2"].is<bool>()) {
      bool camara = doc["camara_conectada"].as<bool>();
      bool estable = doc["deteccion_estable"].as<bool>();
      yoloConectado = camara && estable;
      if (yoloConectado) {
        mascota1Detectada = doc["peluche_1"].as<bool>();
        mascota2Detectada = doc["peluche_2"].as<bool>();
        confianzaMascota1 = doc["confianza_1"] | 0.0f;
        confianzaMascota2 = doc["confianza_2"] | 0.0f;
        ultimaRespuestaYOLO = millis();
      }
      Serial.printf("YOLO: %s | mascota 1: %s (%.2f) | mascota 2: %s (%.2f)\n",
        yoloConectado ? "LISTO" : "SIN DETECCION ESTABLE",
        mascota1Detectada ? "PRESENTE" : "AUSENTE", confianzaMascota1,
        mascota2Detectada ? "PRESENTE" : "AUSENTE", confianzaMascota2);
    } else {
      Serial.println("YOLO: respuesta JSON invalida");
    }
  } else {
    Serial.printf("YOLO: error HTTP %d\n", codigo);
  }
  http.end();
}

void actualizarOLED() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(4, 0);
  oled.println("COMEDERO INTELIGENTE");
  oled.drawLine(0, 11, 127, 11, SSD1306_WHITE);
  if (rtcDisponible) {
    DateTime t = rtc.now();
    oled.setCursor(3, 15);
    oled.printf("%02d/%02d/%04d", t.day(), t.month(), t.year());
    oled.setCursor(80, 15);
    oled.printf("%02d:%02d", t.hour(), t.minute());
  } else {
    oled.setCursor(3, 15);
    oled.print("RTC no disponible");
  }
  oled.setCursor(3, 29);
  oled.print("Deposito 1: ");
  if (porcentaje1 < 0) oled.println("Sin lectura");
  else oled.printf("%d%%\n", porcentaje1);
  oled.setCursor(3, 41);
  oled.print("Deposito 2: ");
  if (porcentaje2 < 0) oled.println("Sin lectura");
  else oled.printf("%d%%\n", porcentaje2);
  oled.drawLine(0, 53, 127, 53, SSD1306_WHITE);
  oled.setCursor(3, 56);
  if (WiFi.status() != WL_CONNECTED) oled.print("WiFi desconectado");
  else if (ultimoEnvioCorrecto) oled.print("WiFi OK | Envio OK");
  else oled.print("WiFi OK | Sin envio");
  oled.display();
}

// Reconocimiento de hechizos: Arcana -> Fenix abrete / Dragon abrete.
// El servidor debe responder comando="activar", "mascota_1" o "mascota_2".
// Si el servidor devuelve otros identificadores, adaptarlos en Python.
const int UMBRAL_VOZ = 350; // Ajustar con las lecturas de Nivel de sonido.
const unsigned long VENTANA_HECHIZO_MS = 8000UL;
const unsigned long PAUSA_GRABACIONES_MS = 800UL;
bool esperandoHechizo = false;
unsigned long inicioVentanaHechizo = 0;
unsigned long ultimaGrabacion = 0;

void probarVozMAX9814() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Voz: WiFi desconectado");
    return;
  }

  const uint32_t frecuencia = 16000;
  const uint32_t muestras = frecuencia * 5 / 2;
  const size_t bytesAudio = muestras * sizeof(int16_t);
  int16_t* audio = (int16_t*)malloc(bytesAudio);
  if (!audio) {
    Serial.println("Voz: memoria insuficiente para grabar");
    return;
  }

  int32_t suma = 0;
  for (int i = 0; i < 256; i++) {
    suma += analogRead(MICROFONO);
    delayMicroseconds(100);
  }
  const int32_t centro = suma / 256;
  Serial.println(esperandoHechizo ? "Escuchando hechizo..." : "Escuchando ARCANA...");
  const uint32_t inicioAudio = micros();
  uint32_t retrasos = 0;
  for (uint32_t i = 0; i < muestras; i++) {
    uint32_t siguiente = inicioAudio + (uint32_t)((uint64_t)i * 1000000ULL / frecuencia);
    while ((int32_t)(micros() - siguiente) < 0) { }
    if ((int32_t)(micros() - siguiente) > 62) retrasos++;
    int32_t valor = ((int32_t)analogRead(MICROFONO) - centro) * 16;
    valor = constrain(valor, -32768L, 32767L);
    audio[i] = (int16_t)valor;
  }
  Serial.printf("Grabacion terminada. Muestras con retraso: %lu\n", (unsigned long)retrasos);

  HTTPClient http;
  http.begin(URL_RECONOCER);
  http.setTimeout(15000);
  http.addHeader("Content-Type", "application/octet-stream");
  int codigo = http.POST((uint8_t*)audio, bytesAudio);
  free(audio);
  Serial.printf("Vosk HTTP: %d\n", codigo);
  if (codigo == 200) {
    String respuesta = http.getString();
    Serial.println(respuesta);
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, respuesta)) {
      Serial.println("JSON de voz invalido");
    } else {
      String comando = doc["comando"] | "";
      // En reposo, solo ARCANA puede habilitar los comandos de alimentacion.
      if (!esperandoHechizo) {
        if (comando == "activar") {
          esperandoHechizo = true;
          inicioVentanaHechizo = millis();
          Serial.println("ARCANA reconocida: di FENIX ABRETE o DRAGON ABRETE");
        } else {
          Serial.println("En reposo: comando ignorado (di ARCANA primero)");
        }
      } else {
        if (comando == "mascota_1" || comando == "mascota_2") {
          // Cerrar la ventana ANTES de mover el servo, para evitar duplicados.
          esperandoHechizo = false;
          uint8_t mascota = (comando == "mascota_1") ? 1 : 2;
          Serial.printf("Hechizo aceptado: mascota %d\n", mascota);
          if (!alimentarMascota(mascota)) Serial.println("Error al enviar alimentacion");
        } else if (comando == "activar") {
          Serial.println("ARCANA ya activada: esperando hechizo");
        } else {
          Serial.println("Hechizo no reconocido; intenta nuevamente");
        }
      }
    }
  } else {
    Serial.println(http.errorToString(codigo));
  }
  http.end();
  ultimaGrabacion = millis();
}

void probarServidorPython() {
  Serial.println("\n=== PRUEBA SERVIDOR PYTHON ===");

  Serial.print("Estado WiFi: ");
  Serial.println(WiFi.status());

  Serial.print("IP de ESP32: ");
  Serial.println(WiFi.localIP());

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERROR: ESP32 no conectada al WiFi");
    return;
  }

  HTTPClient http;
  http.begin("http://192.168.4.3:5000/");
  http.setTimeout(5000);

  int codigo = http.GET();

  Serial.printf("Codigo HTTP: %d\n", codigo);

  if (codigo == 200) {
    Serial.println("CONEXION EXITOSA");
    Serial.println(http.getString());
  } else {
    Serial.println("ERROR: No se pudo conectar al servidor");
    Serial.println(http.errorToString(codigo));
  }

  http.end();
}


void setup() {
  Serial.begin(115200);
  delay(500);

  // ===== LED INDICADORES =====
  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_AMARILLO, OUTPUT);
  pinMode(LED_ROJO, OUTPUT);
  digitalWrite(LED_VERDE, LOW);
  digitalWrite(LED_AMARILLO, LOW);
  digitalWrite(LED_ROJO, LOW);
  // ===========================

  // ===== CONFIGURACION MICROFONO MAX9814 =====
  pinMode(MICROFONO, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(MICROFONO, ADC_11db);
  // ==========================================

  // ===== CONFIGURACION SERVOMOTORES MG996R =====
  servo1.setPeriodHertz(50);
  servo2.setPeriodHertz(50);

  servo1.attach(SERVO_1_PIN, 500, 2400);
  servo2.attach(SERVO_2_PIN, 500, 2400);

  // Colocar ambos servos en posicion cerrada
  servo1.write(POS_CERRADO);
  servo2.write(POS_CERRADO);

  Serial.println("Servomotores inicializados");
  // ============================================

  // ===== SENSORES ULTRASONICOS =====
  pinMode(TRIG_1, OUTPUT);
  pinMode(ECHO_1, INPUT);
  pinMode(TRIG_2, OUTPUT);
  pinMode(ECHO_2, INPUT);

  digitalWrite(TRIG_1, LOW);
  digitalWrite(TRIG_2, LOW);

  // ===== PANTALLA OLED =====
  Wire.begin(OLED_SDA, OLED_SCL);

  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("ERROR: OLED no detectada");
    while (true) delay(1000);
  }

  // ===== RELOJ RTC =====
  rtcDisponible = rtc.begin();

  if (!rtcDisponible) {
    Serial.println("ERROR: RTC no detectado");
  }
  else if (rtc.lostPower()) {
    Serial.println("RTC perdio alimentacion: esperando hora de CAM");
  }

  // ===== HORARIOS DE ALIMENTACION =====
  prefs.begin("comidas", false);
  cargarHorariosLocales();

  // ===== CONEXION WIFI =====
  WiFi.mode(WIFI_STA);
  // IP fija en la red propia de la ESP32-CAM
  if (!WiFi.config(IP_LOCAL, GATEWAY_LOCAL, SUBNET_LOCAL)) {
    Serial.println("ERROR: No se pudo configurar IP fija");
  }
  conectarWiFi();

  actualizarOLED();

  Serial.println("Sistema iniciado: di ARCANA para despertar el comedero");
  Serial.println("Esperando conexion WiFi...");

  unsigned long inicio = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - inicio < 15000) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  // ===== SERVIDOR PYTHON =====
  probarServidorPython();

  // Atender botones de alimentacion manual de la pagina web
  configurarServidorLocal();
}


void loop() {
  actualizarLEDs();
  // Procesar solicitudes HTTP de la ESP32-CAM
  servidorLocal.handleClient();

  // V permite probar manualmente; la deteccion automatica funciona tambien.
  if (Serial.available()) {
    char orden = (char)Serial.read();
    if (orden == 'V' || orden == 'v') probarVozMAX9814();
  }
  unsigned long ahora = millis();
  if (WiFi.status() != WL_CONNECTED && ahora - ultimoIntentoWiFi >= INTERVALO_WIFI) {
    ultimoIntentoWiFi = ahora;
    conectarWiFi();
  }
  if (rtcDisponible && WiFi.status() == WL_CONNECTED &&
      (!rtcSincronizado || ahora - ultimaSincronizacion >= INTERVALO_RTC) &&
      (primerIntentoRTC || ahora - ultimoIntentoRTC >= REINTENTO_RTC)) {
    primerIntentoRTC = false;
    ultimoIntentoRTC = ahora;
    if (sincronizarRTCdesdeCAM()) {
      rtcSincronizado = true;
      ultimaSincronizacion = millis();
    }
  }
  // Consultar YOLO antes de verificar los horarios pendientes.
  if (primeraConsultaYOLO || ahora - ultimaConsultaYOLO >= INTERVALO_YOLO) {
    primeraConsultaYOLO = false;
    ultimaConsultaYOLO = ahora;
    consultarYOLO();
  }
  if (ahora - ultimaConsultaHorarios >= INTERVALO_HORARIOS) {
    ultimaConsultaHorarios = ahora;
    descargarHorarios();
  }
  if (ahora - ultimoEnvio >= INTERVALO_NIVELES) {
    ultimoEnvio = ahora;
    actualizarSensores();
    actualizarLEDs();
    enviarNiveles();
  }
  if (ahora - ultimaVerificacion >= INTERVALO_VERIFICACION) {
    ultimaVerificacion = ahora;
    verificarHorarios();
  }
  if (ahora - ultimaPantalla >= INTERVALO_PANTALLA) {
    ultimaPantalla = ahora;
    actualizarOLED();
  }
  // Expirar la ventana de hechizo si no se dio una orden valida.
  if (esperandoHechizo && millis() - inicioVentanaHechizo >= VENTANA_HECHIZO_MS) {
    esperandoHechizo = false;
    Serial.println("Tiempo agotado: vuelve a decir ARCANA");
  }

  // Detectar actividad sonora sin enviar audio constantemente a Python.
  // El umbral no reconoce palabras: Vosk valida ARCANA y los hechizos.
  static unsigned long ultimoSonido = 0;
  if (WiFi.status() == WL_CONNECTED &&
      millis() - ultimoSonido >= 250 &&
      millis() - ultimaGrabacion >= PAUSA_GRABACIONES_MS) {
    ultimoSonido = millis();
    int sonido = medirSonido();
    Serial.printf("Nivel de sonido: %d\n", sonido);
    if (sonido >= UMBRAL_VOZ) {
      probarVozMAX9814();
    }
  }
  actualizarLEDs();
  delay(10);
}

