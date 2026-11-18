#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include <math.h>
#include <time.h>

// ============================================
// CONFIGURACION ESP32-CAM AI-THINKER
// ============================================

// Pines de la camara OV2640
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22


// IP fija prevista para la ESP32 principal conectada al AP de esta CAM.
// Debe configurarse la misma IP en la ESP32 principal.
const char* URL_ESP32_PRINCIPAL = "http://192.168.4.2/alimentar";

bool activarDispensador(int numero) {
    if (numero != 1 && numero != 2) return false;

    HTTPClient http;
    String url = String(URL_ESP32_PRINCIPAL) + "?n=" + String(numero);
    http.begin(url);
    http.setTimeout(4000);
    int codigo = http.POST("");
    http.end();

    Serial.printf("Solicitud a ESP32 principal, mascota %d: HTTP %d\n", numero, codigo);
    return codigo == 200;
}

// ============================================
// WIFI
// ============================================

const char* AP_SSID = "Comedero_ESP32";
const char* AP_PASS = "Comedero123";

IPAddress IP_AP(192, 168, 4, 1);
IPAddress GATEWAY_AP(192, 168, 4, 1);
IPAddress SUBNET_AP(255, 255, 255, 0);

WebServer servidor(80);

// Horarios persistentes (administrados desde la pagina web)
Preferences preferenciasHorarios;
const int MAX_HORARIOS = 16;
struct HorarioAlimentacion {
    uint8_t mascota;
    uint8_t hora;
    uint8_t minuto;
};
HorarioAlimentacion horarios[MAX_HORARIOS];
int cantidadHorarios = 0;

void guardarHorarios() {
    preferenciasHorarios.putBytes("lista", horarios,
                                 cantidadHorarios * sizeof(HorarioAlimentacion));
}

void cargarHorarios() {
    size_t n = preferenciasHorarios.getBytesLength("lista");
    if (n == 0 || n > sizeof(horarios) ||
        n % sizeof(HorarioAlimentacion) != 0) return;
    HorarioAlimentacion temporal[MAX_HORARIOS] = {};
    preferenciasHorarios.getBytes("lista", temporal, n);
    for (size_t i = 0; i < n / sizeof(HorarioAlimentacion); ++i) {
        if (temporal[i].mascota < 1 || temporal[i].mascota > 2 ||
            temporal[i].hora > 23 || temporal[i].minuto > 59) return;
    }
    memcpy(horarios, temporal, n);
    cantidadHorarios = n / sizeof(HorarioAlimentacion);
}

void listarHorarios() {
    String json = "[";
    for (int i = 0; i < cantidadHorarios; ++i) {
        if (i) json += ",";
        char item[75];
        snprintf(item, sizeof(item),
                 "{\"id\":%d,\"mascota\":%u,\"hora\":\"%02u:%02u\"}",
                 i, horarios[i].mascota, horarios[i].hora, horarios[i].minuto);
        json += item;
    }
    json += "]";
    servidor.sendHeader("Cache-Control", "no-store");
    servidor.send(200, "application/json", json);
}

void agregarHorario() {
    if (!servidor.hasArg("mascota") || !servidor.hasArg("hora")) {
        servidor.send(400, "text/plain", "Faltan datos"); return;
    }
    String mascotaTexto = servidor.arg("mascota");
    String horaTexto = servidor.arg("hora");
    int m = mascotaTexto.toInt();
    int h = -1, min = -1;
    if ((mascotaTexto != "1" && mascotaTexto != "2") ||
        horaTexto.length() != 5 || horaTexto[2] != ':' ||
        horaTexto[0] < '0' || horaTexto[0] > '9' ||
        horaTexto[1] < '0' || horaTexto[1] > '9' ||
        horaTexto[3] < '0' || horaTexto[3] > '9' ||
        horaTexto[4] < '0' || horaTexto[4] > '9') {
        servidor.send(400, "text/plain", "Datos invalidos"); return;
    }
    h = (horaTexto[0]-'0')*10 + horaTexto[1]-'0';
    min = (horaTexto[3]-'0')*10 + horaTexto[4]-'0';
    if (h > 23 || min > 59) {
        servidor.send(400, "text/plain", "Hora invalida"); return;
    }
    for (int i = 0; i < cantidadHorarios; ++i) {
        if (horarios[i].mascota == m && horarios[i].hora == h &&
            horarios[i].minuto == min) {
            servidor.send(409, "text/plain", "Ese horario ya existe"); return;
        }
    }
    if (cantidadHorarios >= MAX_HORARIOS) {
        servidor.send(409, "text/plain", "Limite de 16 horarios"); return;
    }
    horarios[cantidadHorarios++] = {(uint8_t)m, (uint8_t)h, (uint8_t)min};
    guardarHorarios();
    servidor.send(200, "text/plain", "Horario guardado");
}

void eliminarHorario() {
    if (!servidor.hasArg("id")) {
        servidor.send(400, "text/plain", "Falta id"); return;
    }
    String texto = servidor.arg("id");
    if (texto.isEmpty()) {
        servidor.send(400, "text/plain", "ID invalido"); return;
    }
    for (size_t j=0; j<texto.length(); ++j) {
        if (texto[j] < '0' || texto[j] > '9') {
            servidor.send(400, "text/plain", "ID invalido"); return;
        }
    }
    int id = texto.toInt();
    if (id < 0 || id >= cantidadHorarios) {
        servidor.send(404, "text/plain", "Horario inexistente"); return;
    }
    for (int i=id; i<cantidadHorarios-1; ++i) horarios[i]=horarios[i+1];
    --cantidadHorarios;
    guardarHorarios();
    servidor.send(200, "text/plain", "Horario eliminado");
}


unsigned long ultimoIntentoWiFi = 0;
const unsigned long INTERVALO_WIFI = 30000;

// ============================================
// EEPROM
// ============================================

#define EEPROM_SIZE 256
#define EEPROM_MARCA 0xA5

#define ADDR_MARCA 0

#define ADDR_SSID1 1
#define ADDR_PASS1 34

#define ADDR_SSID2 99
#define ADDR_PASS2 132

String ssid1 = "";
String pass1 = "";

String ssid2 = "";
String pass2 = "";

int redActual = 0;

// ============================================
// SENSORES HC-SR04
// ============================================

float distanciaDeposito1 = -1.0;
float distanciaDeposito2 = -1.0;

unsigned long ultimaRecepcionNiveles = 0;

const unsigned long TIMEOUT_NIVELES = 10000;

// CALIBRACION REAL DE AMBOS DEPOSITOS
// 12 cm = vacio (0%); 2 cm = lleno (100%).

const float VACIO_1 = 12.0f;
const float LLENO_1 = 2.0f;

const float VACIO_2 = 12.0f;
const float LLENO_2 = 2.0f;

// ============================================
// CAMARA
// ============================================

httpd_handle_t servidorCamara = NULL;
httpd_handle_t servidorStreaming = NULL;

// ============================================
// ESTADOS YOLO RECIBIDOS DESDE PYTHON (PUENTE ENTRE REDES)
// ============================================
bool yoloCamara = false;
bool yoloEstable = false;
bool yoloPeluche1 = false;
bool yoloPeluche2 = false;
float yoloConfianza1 = 0.0f;
float yoloConfianza2 = 0.0f;
String yoloCajas = ""; // Hasta seis cajas normalizadas recibidas de Python
unsigned long yoloUltimoMensaje = 0;
const unsigned long YOLO_CADUCIDAD_MS = 5000UL;

void recibirYOLO() {
    // Solo aceptar publicaciones procedentes de clientes del AP propio.
    IPAddress cliente = servidor.client().remoteIP();
    if (cliente[0] != 192 || cliente[1] != 168 ||
        cliente[2] != 4 || cliente[3] == 1) {
        servidor.send(403, "text/plain", "Origen no permitido");
        return;
    }
    if (!servidor.hasArg("camara") || !servidor.hasArg("estable") ||
        !servidor.hasArg("p1") || !servidor.hasArg("p2") ||
        !servidor.hasArg("c1") || !servidor.hasArg("c2")) {
        servidor.send(400, "text/plain", "Faltan campos YOLO");
        return;
    }
    yoloCamara = servidor.arg("camara") == "1";
    yoloEstable = servidor.arg("estable") == "1";
    yoloPeluche1 = yoloCamara && yoloEstable && servidor.arg("p1") == "1";
    yoloPeluche2 = yoloCamara && yoloEstable && servidor.arg("p2") == "1";
    yoloConfianza1 = constrain(servidor.arg("c1").toFloat(), 0.0f, 1.0f);
    yoloConfianza2 = constrain(servidor.arg("c2").toFloat(), 0.0f, 1.0f);
    // Formato: x1,y1,x2,y2,clase,confianza;... (enteros 0-1000).
    // Limitar tamano y caracteres para evitar datos malformados.
    String nuevasCajas = servidor.arg("cajas");
    bool valido = nuevasCajas.length() <= 160;
    for (unsigned int i = 0; i < nuevasCajas.length(); i++) {
        char ch = nuevasCajas[i];
        if (!((ch >= '0' && ch <= '9') || ch == ',' || ch == ';')) {
            valido = false;
            break;
        }
    }
    yoloCajas = valido ? nuevasCajas : "";
    yoloUltimoMensaje = millis();
    servidor.send(200, "text/plain", "OK");
}

void consultarYOLOLocal() {
    bool vigente = yoloUltimoMensaje != 0 &&
                   millis() - yoloUltimoMensaje <= YOLO_CADUCIDAD_MS;
    String json = "{";
    json += "\"camara_conectada\":";
    json += (vigente && yoloCamara) ? "true" : "false";
    json += ",\"deteccion_estable\":";
    json += (vigente && yoloEstable) ? "true" : "false";
    json += ",\"peluche_1\":";
    json += (vigente && yoloPeluche1) ? "true" : "false";
    json += ",\"peluche_2\":";
    json += (vigente && yoloPeluche2) ? "true" : "false";
    json += ",\"confianza_1\":" + String(vigente ? yoloConfianza1 : 0.0f, 3);
    json += ",\"confianza_2\":" + String(vigente ? yoloConfianza2 : 0.0f, 3);
    json += ",\"cajas\":[";
    if (vigente && yoloCamara) {
        int inicio = 0;
        int contador = 0;
        while (inicio < (int)yoloCajas.length() && contador < 6) {
            int fin = yoloCajas.indexOf(';', inicio);
            if (fin < 0) fin = yoloCajas.length();
            String registro = yoloCajas.substring(inicio, fin);
            int valores[6] = {0};
            int pos = 0;
            bool correcto = true;
            for (int k = 0; k < 6; k++) {
                int separador = (k == 5) ? registro.length() : registro.indexOf(',', pos);
                if (separador < pos) { correcto = false; break; }
                String numero = registro.substring(pos, separador);
                if (numero.isEmpty() || numero.length() > 4) { correcto = false; break; }
                valores[k] = constrain(numero.toInt(), 0, 1000);
                pos = separador + 1;
            }
            if (correcto && (valores[4] == 1 || valores[4] == 2)) {
                if (contador++) json += ",";
                json += "[";
                for (int k = 0; k < 6; k++) {
                    if (k) json += ",";
                    json += String(valores[k]);
                }
                json += "]";
            }
            inicio = fin + 1;
        }
    }
    json += "]}";
    servidor.sendHeader("Cache-Control", "no-store");
    servidor.send(200, "application/json", json);
}

// ============================================
// FUNCIONES EEPROM
// ============================================

void guardarTexto(int direccion,
                  const String& texto,
                  int longitud) {

    for (int i = 0; i < longitud; i++) {
        char caracter = 0;

        if (i < (int)texto.length()) {
            caracter = texto[i];
        }

        EEPROM.write(direccion + i, caracter);
    }
}

String leerTexto(int direccion, int longitud) {

    String resultado = "";

    for (int i = 0; i < longitud; i++) {

        char caracter =
            (char)EEPROM.read(direccion + i);

        if (caracter == '\0' ||
            (uint8_t)caracter == 0xFF) {
            break;
        }

        resultado += caracter;
    }

    return resultado;
}

void cargarWiFiGuardado() {

    if (EEPROM.read(ADDR_MARCA) != EEPROM_MARCA) {
        Serial.println("No hay redes guardadas");
        return;
    }

    ssid1 = leerTexto(ADDR_SSID1, 33);
    pass1 = leerTexto(ADDR_PASS1, 65);

    ssid2 = leerTexto(ADDR_SSID2, 33);
    pass2 = leerTexto(ADDR_PASS2, 65);

    Serial.println("Configuracion WiFi cargada");
}

void guardarWiFi() {

    EEPROM.write(ADDR_MARCA, EEPROM_MARCA);

    guardarTexto(ADDR_SSID1, ssid1, 33);
    guardarTexto(ADDR_PASS1, pass1, 65);

    guardarTexto(ADDR_SSID2, ssid2, 33);
    guardarTexto(ADDR_PASS2, pass2, 65);

    EEPROM.commit();

    Serial.println("Redes WiFi guardadas");
}

// ============================================
// CONEXION WIFI EXTERNA
// ============================================

void intentarConexionWiFi() {

    if (WiFi.status() == WL_CONNECTED) {
        return;
    }

    String ssidDestino = "";
    String passDestino = "";

    // Alternar entre las redes configuradas
    if (redActual == 0) {

        if (ssid1.length() > 0) {
            ssidDestino = ssid1;
            passDestino = pass1;
            redActual = 1;

        } else if (ssid2.length() > 0) {
            ssidDestino = ssid2;
            passDestino = pass2;
            redActual = 2;
        }

    } else if (redActual == 1) {

        if (ssid2.length() > 0) {
            ssidDestino = ssid2;
            passDestino = pass2;
            redActual = 2;

        } else if (ssid1.length() > 0) {
            ssidDestino = ssid1;
            passDestino = pass1;
        }

    } else {

        if (ssid1.length() > 0) {
            ssidDestino = ssid1;
            passDestino = pass1;
            redActual = 1;

        } else if (ssid2.length() > 0) {
            ssidDestino = ssid2;
            passDestino = pass2;
        }
    }

    if (ssidDestino.length() == 0) {
        return;
    }

    Serial.print("Intentando conectar a: ");
    Serial.println(ssidDestino);

    // Conexion no bloqueante
    WiFi.begin(
        ssidDestino.c_str(),
        passDestino.c_str()
    );
}


// ============================================
// CALCULO DE NIVELES
// ============================================

int calcularPorcentaje(float distancia,
                       float lleno,
                       float vacio) {

    if (distancia < 0 ||
        vacio <= lleno ||
        !isfinite(distancia)) {

        return -1;
    }

    float porcentaje =
        (vacio - distancia) * 100.0f /
        (vacio - lleno);

    return constrain(
        (int)roundf(porcentaje),
        0,
        100
    );
}

bool sensoresConectados() {

    return ultimaRecepcionNiveles != 0 &&
           millis() - ultimaRecepcionNiveles
               < TIMEOUT_NIVELES;
}

// ============================================
// INICIALIZAR CAMARA
// ============================================

bool iniciarCamara() {

    camera_config_t config = {};

    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;

    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;

    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;

    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;

    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;

    config.xclk_freq_hz = 20000000;

    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_QVGA;

    config.jpeg_quality = 15; // JPEG mas ligero para transmitir con mayor fluidez
    // Doble buffer solo si hay PSRAM. Con un buffer sigue siendo compatible.
    if (psramFound()) {
        config.fb_count = 2;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
        config.fb_count = 1;
        Serial.println("Sin PSRAM: se utilizara un solo buffer");
    }

    esp_err_t resultado = esp_camera_init(&config);

    if (resultado != ESP_OK) {

        Serial.printf(
            "Error iniciando camara: 0x%x\n",
            resultado
        );

        return false;
    }

    Serial.println("Camara inicializada");

    return true;
}

// ============================================
// CAPTURA JPEG
// ============================================

static esp_err_t capturarImagen(
    httpd_req_t* req
) {

    camera_fb_t* fb = esp_camera_fb_get();

    if (!fb) {
        return httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Error capturando imagen"
        );
    }

    httpd_resp_set_type(
        req,
        "image/jpeg"
    );

    httpd_resp_set_hdr(
        req,
        "Access-Control-Allow-Origin",
        "*"
    );

    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-store"
    );

    esp_err_t resultado = httpd_resp_send(
        req,
        (const char*)fb->buf,
        fb->len
    );

    esp_camera_fb_return(fb);

    return resultado;
}

// Streaming MJPEG: conexion continua. Servidor separado para que /capture
// siga disponible para la futura captura de imagenes de entrenamiento.
static esp_err_t transmitirVideo(httpd_req_t* req) {
    static const char* TIPO = "multipart/x-mixed-replace;boundary=frame";
    static const char* CABECERA = "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";
    esp_err_t resultado = httpd_resp_set_type(req, TIPO);
    if (resultado != ESP_OK) return resultado;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");

    char encabezado[96];
    while (true) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("Streaming: fallo al capturar fotograma");
            return ESP_FAIL;
        }
        int longitud = snprintf(encabezado, sizeof(encabezado), CABECERA, (unsigned int)fb->len);
        resultado = httpd_resp_send_chunk(req, encabezado, longitud);
        if (resultado == ESP_OK)
            resultado = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
        esp_camera_fb_return(fb);
        if (resultado != ESP_OK) break; // Navegador desconectado
        vTaskDelay(pdMS_TO_TICKS(10)); // Ceder CPU a WiFi y servidor web
    }
    return resultado;
}

void iniciarServidorCamara() {
    // Capturas JPEG independientes en puerto 81 (para Python/YOLO).
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 81;
    config.ctrl_port = 32769;
    httpd_uri_t uriCaptura = {};
    uriCaptura.uri = "/capture";
    uriCaptura.method = HTTP_GET;
    uriCaptura.handler = capturarImagen;
    if (httpd_start(&servidorCamara, &config) == ESP_OK) {
        httpd_register_uri_handler(servidorCamara, &uriCaptura);
        Serial.println("Capturas JPEG: puerto 81 /capture");
    } else {
        Serial.println("Error iniciando servidor de capturas");
    }

    // MJPEG en otro puerto: el flujo no bloquea el endpoint /capture.
    httpd_config_t streamConfig = HTTPD_DEFAULT_CONFIG();
    streamConfig.server_port = 82;
    streamConfig.ctrl_port = 32770;
    streamConfig.stack_size = 8192;
    httpd_uri_t uriStream = {};
    uriStream.uri = "/stream";
    uriStream.method = HTTP_GET;
    uriStream.handler = transmitirVideo;
    if (httpd_start(&servidorStreaming, &streamConfig) == ESP_OK) {
        httpd_register_uri_handler(servidorStreaming, &uriStream);
        Serial.println("Streaming MJPEG: puerto 82 /stream");
    } else {
        Serial.println("Error iniciando servidor MJPEG (revisar memoria)");
    }
}


// ============================================
// PAGINA WEB PRINCIPAL
// ============================================

const char PAGINA_PRINCIPAL[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">

<head>
<meta charset="UTF-8">
<meta name="viewport"
      content="width=device-width, initial-scale=1">

<title>Comedero Inteligente</title>

<style>

* {
    box-sizing: border-box;
}

body {
    font-family: Arial, sans-serif;
    background: #eef2f7;
    margin: 0;
    padding: 18px;
    color: #243447;
}

.contenedor {
    max-width: 650px;
    margin: auto;
}

h1 {
    text-align: center;
    color: #173b64;
}

.tarjeta {
    background: white;
    padding: 20px;
    margin-bottom: 18px;
    border-radius: 14px;
    box-shadow: 0 3px 12px rgba(0,0,0,0.08);
}

h2 {
    margin-top: 0;
    color: #254f79;
}

.marco-camara {
    position: relative;
    width: 100%;
    max-width: 420px;
    margin: auto;
}
#capaYOLO {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    pointer-events: none;
}
.camara {
    width: 100%;
    max-width: 420px;
    display: block;
    margin: auto;
    border-radius: 10px;
    background: #222;
}

.boton {
    display: block;
    width: 100%;
    padding: 14px;
    margin-top: 12px;
    border: none;
    border-radius: 9px;
    background: #2478d4;
    color: white;
    font-size: 16px;
    font-weight: bold;
    cursor: pointer;
}

.boton:disabled {
    opacity: 0.55;
    cursor: not-allowed;
}

.boton2 {
    background: #1c9b72;
}

.config {
    display: block;
    text-align: center;
    padding: 12px;
    color: #2478d4;
}

.barra-fondo {
    background: #e2e7ed;
    height: 25px;
    border-radius: 15px;
    overflow: hidden;
    margin-top: 10px;
}

.barra {
    width: 0%;
    height: 100%;
    background: #20a76d;
    transition: width 0.4s;
}

.nivel {
    font-size: 25px;
    font-weight: bold;
}

.estado {
    color: #52657b;
    font-size: 14px;
}

.alerta {
    color: #d26a17;
    font-weight: bold;
}

</style>
</head>

<body>

<div class="contenedor">

<h1>Comedero Inteligente</h1>

<div class="tarjeta">

<h2>Camara en vivo</h2>

<div class="marco-camara">
<img id="camara"
     class="camara"
     alt="Imagen de la ESP32-CAM">
<canvas id="capaYOLO"></canvas>
</div>

<p class="estado" id="estadoCamara">
    Conectando camara...
</p>
<p class="estado" id="estadoYOLO">YOLO: comprobando servidor...</p>
<p class="estado" id="mascotasYOLO">Mascota 1: -- | Mascota 2: --</p>

</div>

<div class="tarjeta">

<h2>Alimentacion manual</h2>

<button class="boton"
        id="boton1"
        onclick="alimentar(1)">
    Alimentar mascota 1
</button>

<button class="boton boton2"
        id="boton2"
        onclick="alimentar(2)">
    Alimentar mascota 2
</button>

<p class="estado" id="estadoServo">
    Sistema listo
</p>

</div>

<div class="tarjeta">

<h2>Niveles de alimento</h2>

<h3>Deposito 1</h3>

<div class="nivel" id="nivel1">--</div>

<div class="barra-fondo">
    <div class="barra" id="barra1"></div>
</div>

<p class="estado" id="distancia1">
    Esperando medicion...
</p>

<hr>

<h3>Deposito 2</h3>

<div class="nivel" id="nivel2">--</div>

<div class="barra-fondo">
    <div class="barra" id="barra2"></div>
</div>

<p class="estado" id="distancia2">
    Esperando medicion...
</p>

<hr>

<p class="estado" id="estadoSensores">
    Esperando conexion de sensores...
</p>

</div>

<div class="tarjeta">
<h2>Horarios de alimentacion</h2>
<p class="estado">Programa las comidas de cada mascota. Maximo 16 horarios.</p>
<form id="formHorarios" onsubmit="agregarHorarioWeb(event)">
  <label for="mascotaHorario">Mascota</label>
  <select id="mascotaHorario" required style="width:100%;padding:12px;margin:8px 0 12px">
    <option value="1">Mascota 1</option>
    <option value="2">Mascota 2</option>
  </select>
  <label for="horaHorario">Hora</label>
  <input id="horaHorario" type="time" value="08:00" required
         style="width:100%;padding:12px;margin:8px 0 12px">
  <button class="boton" type="submit">Agregar horario</button>
</form>
<p class="estado" id="mensajeHorarios" role="status"></p>
<div id="listaHorarios">Cargando horarios...</div>
</div>

<div class="tarjeta">

<h2>Configuracion</h2>

<a class="config" href="/config">
    Configurar redes Wi-Fi
</a>

</div>

</div>

<script>

// ========================================
// CAMARA
// ========================================

const imagen = document.getElementById("camara");
const estadoCamara = document.getElementById("estadoCamara");
const estadoYOLO = document.getElementById("estadoYOLO");
const mascotasYOLO = document.getElementById("mascotasYOLO");

// Video original y cajas superpuestas desde la propia ESP32-CAM.
// Funciona desde la IP AP o desde la IP STA del router.
const STREAM_ORIGINAL = "http://" + location.hostname + ":82/stream";
const capa = document.getElementById("capaYOLO");
const contexto = capa.getContext("2d");
let reintentoCamara = null;
let cajasActuales = [];

function iniciarCamaraOriginal() {
    clearTimeout(reintentoCamara);
    estadoCamara.textContent = "Conectando video de ESP32-CAM...";
    imagen.src = STREAM_ORIGINAL + "?t=" + Date.now();
}

imagen.onload = function() {
    estadoCamara.textContent = "Video en vivo (recuadros superpuestos por YOLO)";
};
imagen.onerror = function() {
    estadoCamara.textContent = "Sin streaming; reintentando...";
    reintentoCamara = setTimeout(iniciarCamaraOriginal, 3000);
};

function dibujarCajas() {
    const ancho = imagen.clientWidth;
    const alto = imagen.clientHeight;
    if (!ancho || !alto) return;
    const escala = window.devicePixelRatio || 1;
    const anchoPx = Math.round(ancho * escala);
    const altoPx = Math.round(alto * escala);
    if (capa.width !== anchoPx || capa.height !== altoPx) {
        capa.width = anchoPx;
        capa.height = altoPx;
    }
    contexto.setTransform(escala, 0, 0, escala, 0, 0);
    contexto.clearRect(0, 0, ancho, alto);
    contexto.lineWidth = 2;
    contexto.font = "bold 13px sans-serif";
    for (const caja of cajasActuales) {
        if (!Array.isArray(caja) || caja.length !== 6) continue;
        const [x1,y1,x2,y2,clase,conf] = caja;
        if (![x1,y1,x2,y2,clase,conf].every(Number.isFinite)) continue;
        const x = x1 * ancho / 1000;
        const y = y1 * alto / 1000;
        const w = Math.max(0, (x2-x1) * ancho / 1000);
        const h = Math.max(0, (y2-y1) * alto / 1000);
        const color = clase === 1 ? "#00ff99" : "#ffcc33";
        const etiqueta = "Mascota " + clase + " " + Math.round(conf/10) + "%";
        contexto.strokeStyle = color;
        contexto.strokeRect(x,y,w,h);
        const textoAncho = contexto.measureText(etiqueta).width + 10;
        const etiquetaY = y >= 22 ? y - 22 : y;
        contexto.fillStyle = color;
        contexto.fillRect(x, etiquetaY, textoAncho, 21);
        contexto.fillStyle = "#111";
        contexto.fillText(etiqueta, x + 5, etiquetaY + 15);
    }
}

async function actualizarYOLOWeb() {
    try {
        const r = await fetch("/api/yolo", {cache: "no-store"});
        if (!r.ok) throw new Error("HTTP " + r.status);
        const d = await r.json();
        cajasActuales = d.camara_conectada && Array.isArray(d.cajas) ? d.cajas : [];
        dibujarCajas();
        if (!d.camara_conectada) {
            estadoYOLO.textContent = "YOLO: sin datos recientes de Python";
            mascotasYOLO.textContent = "Mascota 1: -- | Mascota 2: --";
        } else if (!d.deteccion_estable) {
            estadoYOLO.textContent = "YOLO: esperando detecciones estables";
            mascotasYOLO.textContent = "Mascota 1: -- | Mascota 2: --";
        } else {
            estadoYOLO.textContent = "YOLO conectado (datos recibidos por ESP32-CAM)";
            mascotasYOLO.textContent =
                "Mascota 1: " + (d.peluche_1 ? "PRESENTE" : "AUSENTE") +
                " | Mascota 2: " + (d.peluche_2 ? "PRESENTE" : "AUSENTE");
        }
    } catch (e) {
        cajasActuales = [];
        dibujarCajas();
        estadoYOLO.textContent = "YOLO: error consultando ESP32-CAM";
        mascotasYOLO.textContent = "Mascota 1: -- | Mascota 2: --";
    }
}

window.addEventListener("resize", dibujarCajas);
iniciarCamaraOriginal();
actualizarYOLOWeb();
setInterval(actualizarYOLOWeb, 1000);

// ========================================
// DISPENSADORES
// ========================================

async function alimentar(numero) {

    const estado =
        document.getElementById("estadoServo");

    const boton1 =
        document.getElementById("boton1");

    const boton2 =
        document.getElementById("boton2");

    boton1.disabled = true;
    boton2.disabled = true;

    estado.textContent =
        "Activando dispensador " + numero + "...";

    try {

        const respuesta = await fetch(
            "/alimentar?n=" + numero,
            { method: "POST" }
        );

        const mensaje = await respuesta.text();

        estado.textContent = mensaje;

    } catch (error) {

        estado.textContent =
            "Error comunicando con el comedero";
    }

    setTimeout(() => {
        boton1.disabled = false;
        boton2.disabled = false;
    }, 1500);
}

// ========================================
// SENSORES
// ========================================

function mostrarNivel(numero, porcentaje, distancia) {

    const nivel =
        document.getElementById("nivel" + numero);

    const barra =
        document.getElementById("barra" + numero);

    const textoDistancia =
        document.getElementById("distancia" + numero);

    if (porcentaje < 0) {

        nivel.textContent = "Sin datos";
        barra.style.width = "0%";

        textoDistancia.textContent =
            "Sin lectura disponible";

        return;
    }

    nivel.textContent = porcentaje + "%";

    barra.style.width = porcentaje + "%";

    if (porcentaje <= 20) {

        barra.style.background = "#e04444";

    } else if (porcentaje <= 40) {

        barra.style.background = "#e5a02a";

    } else {

        barra.style.background = "#20a76d";
    }

    textoDistancia.textContent =
        "Distancia: " +
        Number(distancia).toFixed(2) + " cm";

    if (porcentaje <= 20) {

        textoDistancia.textContent +=
            " - Alimento bajo";
    }
}

async function actualizarNiveles() {

    try {

        const respuesta = await fetch(
            "/estado-niveles",
            { cache: "no-store" }
        );

        if (!respuesta.ok) {
            throw new Error("Error HTTP");
        }

        const datos = await respuesta.json();

        mostrarNivel(
            1,
            datos.deposito1,
            datos.distancia1
        );

        mostrarNivel(
            2,
            datos.deposito2,
            datos.distancia2
        );

        document.getElementById("estadoSensores")
            .textContent = datos.conectado
            ? "Sensores conectados"
            : "Sin comunicacion con sensores";

    } catch (error) {

        document.getElementById("estadoSensores")
            .textContent =
            "Error consultando sensores";

        mostrarNivel(1, -1, 0);
        mostrarNivel(2, -1, 0);
    }
}

actualizarNiveles();

setInterval(actualizarNiveles, 2000);

// ========================================
// HORARIOS DE ALIMENTACION
// ========================================
async function cargarHorariosWeb() {
  const lista = document.getElementById('listaHorarios');
  try {
    const r = await fetch('/api/horarios', {cache:'no-store'});
    if (!r.ok) throw new Error('No se pudieron cargar');
    const horarios = await r.json();
    lista.replaceChildren();
    for (const mascota of [1, 2]) {
      const titulo = document.createElement('h3');
      titulo.textContent = 'Mascota ' + mascota;
      lista.appendChild(titulo);
      const elementos = horarios.filter(x=>x.mascota===mascota)
                                .sort((a,b)=>a.hora.localeCompare(b.hora));
      if (!elementos.length) {
        const p=document.createElement('p');
        p.className='estado'; p.textContent='Sin horarios'; lista.appendChild(p);
      }
      for (const item of elementos) {
        const fila=document.createElement('div');
        fila.style.cssText='display:flex;align-items:center;justify-content:space-between;border-bottom:1px solid #ddd;padding:8px 0';
        const hora=document.createElement('strong'); hora.textContent=item.hora;
        const boton=document.createElement('button');
        boton.textContent='Eliminar'; boton.type='button';
        boton.style.cssText='background:#b72e3d;color:white;border:0;border-radius:7px;padding:9px;cursor:pointer';
        boton.onclick=()=>eliminarHorarioWeb(item.id, item.hora, mascota);
        fila.append(hora,boton); lista.appendChild(fila);
      }
    }
  } catch(e) {lista.textContent='Error consultando horarios';}
}
async function agregarHorarioWeb(event) {
  event.preventDefault();
  const boton=event.target.querySelector('button[type="submit"]');
  const mensaje=document.getElementById('mensajeHorarios');
  boton.disabled=true;
  try {
    const datos=new URLSearchParams({
      mascota:document.getElementById('mascotaHorario').value,
      hora:document.getElementById('horaHorario').value
    });
    const r=await fetch('/api/horarios/agregar', {method:'POST',body:datos});
    const texto=await r.text();
    mensaje.textContent=texto;
    if(r.ok) await cargarHorariosWeb();
  } catch(e) {mensaje.textContent='Error de comunicacion';}
  finally {boton.disabled=false;}
}
async function eliminarHorarioWeb(id,hora,mascota) {
  if(!confirm('Eliminar '+hora+' de mascota '+mascota+'?')) return;
  const mensaje=document.getElementById('mensajeHorarios');
  try {
    const r=await fetch('/api/horarios/eliminar', {
      method:'POST',body:new URLSearchParams({id:String(id)})
    });
    mensaje.textContent=await r.text();
    if(r.ok) await cargarHorariosWeb();
  } catch(e) {mensaje.textContent='Error de comunicacion';}
}
cargarHorariosWeb();



</script>

</body>
</html>
)rawliteral";

// ============================================
// PAGINA CONFIGURACION WIFI
// ============================================

String escaparHTML(const String& entrada) {

    String resultado = entrada;

    resultado.replace("&", "&amp;");
    resultado.replace("\"", "&quot;");
    resultado.replace("<", "&lt;");
    resultado.replace(">", "&gt;");
    resultado.replace("'", "&#39;");

    return resultado;
}

void mostrarConfiguracion() {

    String html = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport"
content="width=device-width, initial-scale=1">

<title>Configuracion WiFi</title>

<style>
body {
    font-family: Arial;
    background: #eef2f7;
    padding: 20px;
}

form {
    max-width: 450px;
    margin: auto;
    background: white;
    padding: 25px;
    border-radius: 12px;
}

input {
    display: block;
    width: 100%;
    box-sizing: border-box;
    padding: 12px;
    margin: 8px 0 18px;
}

button {
    padding: 14px;
    width: 100%;
    background: #2478d4;
    color: white;
    border: none;
    border-radius: 8px;
}

a {
    display: block;
    margin-top: 20px;
    text-align: center;
}
</style>
</head>

<body>

<form action="/guardar" method="POST">

<h2>Configuracion Wi-Fi</h2>

<label>Nombre de red 1</label>
<input name="ssid1" maxlength="32" value=")rawliteral";

    html += escaparHTML(ssid1);

    html += R"rawliteral(">

<label>Contrasena de red 1</label>
<input name="pass1" type="password"
       maxlength="64"
       placeholder="Dejar vacio para conservar">

<label>Nombre de red 2</label>
<input name="ssid2" maxlength="32" value=")rawliteral";

    html += escaparHTML(ssid2);

    html += R"rawliteral(">

<label>Contrasena de red 2</label>
<input name="pass2" type="password"
       maxlength="64"
       placeholder="Dejar vacio para conservar">

<button type="submit">
Guardar configuracion
</button>

<a href="/">Volver al comedero</a>

</form>

</body>
</html>
)rawliteral";

    servidor.send(
        200,
        "text/html; charset=utf-8",
        html
    );
}

void guardarConfiguracionWeb() {

    String nuevoSSID1 =
        servidor.arg("ssid1");

    String nuevoSSID2 =
        servidor.arg("ssid2");

    String nuevaPass1 =
        servidor.arg("pass1");

    String nuevaPass2 =
        servidor.arg("pass2");

    // Si el nombre cambia, exigir nueva clave
    if ((nuevoSSID1 != ssid1 &&
         nuevaPass1.length() == 0 &&
         nuevoSSID1.length() > 0) ||
        (nuevoSSID2 != ssid2 &&
         nuevaPass2.length() == 0 &&
         nuevoSSID2.length() > 0)) {

        servidor.send(
            400,
            "text/plain",
            "Introduce la contrasena de la nueva red"
        );
        return;
    }

    if (nuevoSSID1.length() > 32 ||
        nuevoSSID2.length() > 32 ||
        nuevaPass1.length() > 64 ||
        nuevaPass2.length() > 64) {

        servidor.send(
            400,
            "text/plain",
            "Datos demasiado largos"
        );
        return;
    }

    if (nuevoSSID1.length() == 0) {
        pass1 = "";
    } else if (nuevaPass1.length() > 0) {
        pass1 = nuevaPass1;
    }

    if (nuevoSSID2.length() == 0) {
        pass2 = "";
    } else if (nuevaPass2.length() > 0) {
        pass2 = nuevaPass2;
    }

    ssid1 = nuevoSSID1;
    ssid2 = nuevoSSID2;

    guardarWiFi();

    servidor.send(
        200,
        "text/html; charset=utf-8",
        "<h2>Configuracion guardada</h2>"
        "<p>La conexion se actualizara.</p>"
        "<a href='/'>Volver</a>"
    );

    redActual = 0;

    WiFi.disconnect();
    ultimoIntentoWiFi = millis();

    // Evita intentar una nueva conexion
    // mientras se atiende la solicitud HTTP.
}

// Declaracion anticipada del controlador de hora
void enviarHora();

// ============================================
// CONFIGURACION SERVIDOR WEB
// ============================================

void configurarServidor() {
    // Publicacion desde Python y consulta local desde cualquier interfaz WiFi.
    servidor.on("/api/yolo/actualizar", HTTP_POST, recibirYOLO);
    servidor.on("/api/yolo", HTTP_GET, consultarYOLOLocal);


    // Hora local proporcionada por NTP a la segunda ESP32
    servidor.on("/hora", HTTP_GET, enviarHora);

    // API para administrar horarios desde la pagina web
    servidor.on("/api/horarios", HTTP_GET, listarHorarios);
    servidor.on("/api/horarios/agregar", HTTP_POST, agregarHorario);
    servidor.on("/api/horarios/eliminar", HTTP_POST, eliminarHorario);

    // Pagina principal
    servidor.on("/", HTTP_GET, []() {

        servidor.send_P(
            200,
            "text/html; charset=utf-8",
            PAGINA_PRINCIPAL
        );
    });

    // Configuracion WiFi
    servidor.on(
        "/config",
        HTTP_GET,
        mostrarConfiguracion
    );

    servidor.on(
        "/guardar",
        HTTP_POST,
        guardarConfiguracionWeb
    );

    // ========================================
    // ACTIVAR SERVOMOTOR
    // ========================================

    servidor.on(
        "/alimentar",
        HTTP_POST,
        []() {

            if (!servidor.hasArg("n")) {

                servidor.send(
                    400,
                    "text/plain",
                    "Falta numero de dispensador"
                );

                return;
            }

            int numero =
                servidor.arg("n").toInt();

            if (numero != 1 && numero != 2) {

                servidor.send(
                    400,
                    "text/plain",
                    "Dispensador invalido"
                );

                return;
            }

            if (!activarDispensador(numero)) {

                servidor.send(
                    503,
                    "text/plain",
                    "No se pudo contactar a la ESP32 principal"
                );

                return;
            }

            servidor.send(
                200,
                "text/plain",
                "Alimentando mascota " +
                String(numero)
            );
        }
    );

    // ========================================
    // RECIBIR DISTANCIAS DE SEGUNDA ESP32
    // ========================================

    servidor.on(
        "/niveles",
        HTTP_POST,
        []() {

            if (!servidor.hasArg("d1") ||
                !servidor.hasArg("d2")) {

                servidor.send(
                    400,
                    "text/plain",
                    "Faltan distancias"
                );

                return;
            }

            float d1 =
                servidor.arg("d1").toFloat();

            float d2 =
                servidor.arg("d2").toFloat();

            if (!isfinite(d1) ||
                !isfinite(d2) ||
                d1 < -1 || d1 > 500 ||
                d2 < -1 || d2 > 500) {

                servidor.send(
                    400,
                    "text/plain",
                    "Distancias invalidas"
                );

                return;
            }

            distanciaDeposito1 = d1;
            distanciaDeposito2 = d2;

            ultimaRecepcionNiveles = millis();

            Serial.println();
            Serial.println("Datos recibidos:");

            Serial.print("Deposito 1: ");
            Serial.print(d1);
            Serial.println(" cm");

            Serial.print("Deposito 2: ");
            Serial.print(d2);
            Serial.println(" cm");

            servidor.send(
                200,
                "text/plain",
                "OK"
            );
        }
    );

    // ========================================
    // CONSULTAR PORCENTAJES
    // ========================================

    servidor.on(
        "/estado-niveles",
        HTTP_GET,
        []() {

            bool conectado =
                sensoresConectados();

            int p1 = -1;
            int p2 = -1;

            if (conectado) {

                p1 = calcularPorcentaje(
                    distanciaDeposito1,
                    LLENO_1,
                    VACIO_1
                );

                p2 = calcularPorcentaje(
                    distanciaDeposito2,
                    LLENO_2,
                    VACIO_2
                );
            }

            String json = "{";

            json += "\"conectado\":";
            json += conectado ? "true" : "false";

            json += ",\"deposito1\":";
            json += String(p1);

            json += ",\"deposito2\":";
            json += String(p2);

            json += ",\"distancia1\":";
            json += String(distanciaDeposito1, 2);

            json += ",\"distancia2\":";
            json += String(distanciaDeposito2, 2);

            json += "}";

            servidor.send(
                200,
                "application/json",
                json
            );
        }
    );

    // Ruta inexistente
    servidor.onNotFound([]() {

        servidor.send(
            404,
            "text/plain",
            "Ruta no encontrada"
        );
    });

    servidor.begin();

    Serial.println(
        "Servidor web iniciado en puerto 80"
    );
}
// ============================================
// SINCRONIZACION DE HORA POR INTERNET (NTP)
// ============================================
// Ecuador continental: UTC-5, sin horario de verano.
// El router debe proporcionar Internet a la ESP32-CAM.
bool ntpConfigurado = false;
bool ntpConfirmado = false;

void configurarNTP() {
    if (WiFi.status() != WL_CONNECTED) {
        ntpConfigurado = false;
        return;
    }

    if (!ntpConfigurado) {
        configTime(-5 * 3600, 0,
                   "pool.ntp.org",
                   "time.google.com");
        ntpConfigurado = true;
        Serial.println("NTP iniciado; esperando hora de Internet...");
    }

    // Verificacion no bloqueante: no interrumpe la camara ni los servos.
    time_t instante = time(nullptr);
    if (!ntpConfirmado && instante > 1735689600) {
        ntpConfirmado = true;
        Serial.println("Hora NTP disponible para /hora");
    }
}

void enviarHora() {
    // No devolver la fecha ficticia de arranque si NTP aun no sincronizo.
    if (!ntpConfirmado || time(nullptr) <= 1735689600) {
        servidor.send(503, "text/plain", "Hora no disponible");
        return;
    }

    struct tm tiempo;
    if (!getLocalTime(&tiempo, 100)) {
        servidor.send(503, "text/plain", "Hora no disponible");
        return;
    }

    char respuesta[32];
    strftime(respuesta, sizeof(respuesta),
             "%Y-%m-%d %H:%M:%S", &tiempo);
    servidor.sendHeader("Cache-Control", "no-store");
    servidor.send(200, "text/plain", respuesta);
}

// ============================================
// SETUP
// ============================================

void setup() {

    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("============================");
    Serial.println(" COMEDERO INTELIGENTE ESP32");
    Serial.println("============================");

    // EEPROM
    EEPROM.begin(EEPROM_SIZE);
    cargarWiFiGuardado();
    preferenciasHorarios.begin("horarios", false);
    cargarHorarios();
    Serial.printf("Horarios guardados: %d\n", cantidadHorarios);

    // Camara
    bool camaraOK = iniciarCamara();

    // WiFi AP + STA
    WiFi.mode(WIFI_AP_STA);

    WiFi.softAPConfig(
        IP_AP,
        GATEWAY_AP,
        SUBNET_AP
    );

    WiFi.softAP(
        AP_SSID,
        AP_PASS
    );

    Serial.println();
    Serial.println("Red propia creada:");

    Serial.print("SSID: ");
    Serial.println(AP_SSID);

    Serial.print("IP AP: ");
    Serial.println(WiFi.softAPIP());

    // Servidor web
    configurarServidor();

    // Servidor camara
    if (camaraOK) {
        iniciarServidorCamara();
    }

    // WiFi externo
    ultimoIntentoWiFi = millis();
    intentarConexionWiFi();

    Serial.println();
    Serial.println("Sistema iniciado");

    Serial.println(
        "Pagina: http://192.168.4.1"
    );

    Serial.println(
        "Capturas: http://192.168.4.1:81/capture"
    );

    Serial.println(
        "Video MJPEG: http://192.168.4.1:82/stream"
    );
}

// ============================================
// LOOP PRINCIPAL
// ============================================

void loop() {

    // Atender pagina web
    servidor.handleClient();

    // Sincronizar reloj por Internet cuando haya conexion externa
    configurarNTP();

    // Mostrar IP cuando se conecte al router
    static bool ipMostrada = false;

    if (WiFi.status() == WL_CONNECTED && !ipMostrada) {

        Serial.println();
        Serial.println("========== WIFI CONECTADO ==========");

        Serial.print("Red conectada: ");
        Serial.println(WiFi.SSID());

        Serial.print("IP de la pagina web: http://");
        Serial.println(WiFi.localIP());

        Serial.print("IP de la camara: http://");
        Serial.print(WiFi.localIP());
        Serial.println(":81/capture");
        Serial.print("IP del streaming: http://");
        Serial.print(WiFi.localIP());
        Serial.println(":82/stream");

        Serial.println("====================================");

        ipMostrada = true;
    }

    if (WiFi.status() != WL_CONNECTED) {
        ipMostrada = false;
    }

    // Reconectar a las redes configuradas
    if (WiFi.status() != WL_CONNECTED &&
        millis() - ultimoIntentoWiFi >= INTERVALO_WIFI) {

        ultimoIntentoWiFi = millis();

        intentarConexionWiFi();
    }

    delay(2);
}
