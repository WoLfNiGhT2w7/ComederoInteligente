import json
import queue
import threading
import unicodedata
import time

from collections import deque
from pathlib import Path

import cv2
import numpy as np
import requests
import sounddevice as sd

from flask import Flask, request, jsonify, Response
from vosk import Model, KaldiRecognizer, SetLogLevel
from ultralytics import YOLO

# El servidor permite CORS mediante @app.after_request (mas abajo).

# ==========================================
# CONFIGURACION
# ==========================================

app = Flask(__name__)

BASE = Path(__file__).resolve().parent

RUTA_VOSK = BASE / "modelo"
RUTA_YOLO = BASE / "best.pt"

URL_CAMARA = "http://192.168.4.1:81/capture"
URL_ESP32CAM_YOLO = "http://192.168.4.1/api/yolo/actualizar"

FRECUENCIA = 16000
PUERTO = 5000

CONFIANZA = 0.50
INTERVALO_CAMARA = 0.5
VENTANA_DETECCION = 3
TIEMPO_MAXIMO_DATOS = 5.0


# ==========================================
# CARGAR MODELOS
# ==========================================

if not RUTA_VOSK.is_dir():
    raise RuntimeError(
        "No se encuentra la carpeta modelo de Vosk"
    )

if not RUTA_YOLO.is_file():
    raise RuntimeError(
        "No se encuentra best.pt"
    )

SetLogLevel(-1)

modelo_vosk = Model(str(RUTA_VOSK))
modelo_yolo = YOLO(str(RUTA_YOLO))

cola_audio = queue.Queue()

print("Vosk cargado correctamente")
print("YOLO cargado correctamente")


# ==========================================
# ESTADO DE DETECCIONES
# ==========================================

bloqueo = threading.Lock()

estado_yolo = {
    "camara_conectada": False,
    "deteccion_estable": False,
    "peluche_1": False,
    "peluche_2": False,
    "confianza_1": 0.0,
    "confianza_2": 0.0,
    "ultima_actualizacion": None
}

ultimo_frame = 0.0
ultimo_jpeg_yolo = None


# ==========================================
# RECONOCIMIENTO DE VOZ
# ==========================================

def normalizar_texto(texto):
    texto = texto.lower().strip()
    texto = unicodedata.normalize("NFD", texto)

    return "".join(
        c for c in texto
        if unicodedata.category(c) != "Mn"
    )


def identificar_comando(texto):
    texto = normalizar_texto(texto)

    if "arcana" in texto:
        return "activar"

    if "fenix abrete" in texto:
        return "mascota_1"

    if "dragon abrete" in texto:
        return "mascota_2"

    return None


def procesar_audio(audio):
    reconocedor = KaldiRecognizer(
        modelo_vosk,
        FRECUENCIA
    )

    reconocedor.AcceptWaveform(audio)

    resultado = json.loads(
        reconocedor.FinalResult()
    )

    texto = resultado.get("text", "").strip()
    comando = identificar_comando(texto)

    print("Texto reconocido:", texto or "(vacio)")
    print("Comando:", comando or "ninguno")

    return {
        "texto": texto,
        "comando": comando
    }


def recibir_microfono(indata, frames, tiempo, estado):
    if estado:
        print("Audio:", estado)

    cola_audio.put(bytes(indata))


def prueba_microfono():
    print("\nPRUEBA DE VOZ")
    print("Di: Arcana")
    print("Di: Fenix abrete")
    print("Di: Dragon abrete")
    print("Presiona Ctrl+C para salir.\n")

    reconocedor = KaldiRecognizer(
        modelo_vosk,
        FRECUENCIA
    )

    with sd.RawInputStream(
        samplerate=FRECUENCIA,
        blocksize=4000,
        dtype="int16",
        channels=1,
        callback=recibir_microfono
    ):
        while True:
            datos = cola_audio.get()

            if reconocedor.AcceptWaveform(datos):
                resultado = json.loads(
                    reconocedor.Result()
                )

                texto = resultado.get(
                    "text", ""
                ).strip()

                if texto:
                    comando = identificar_comando(texto)

                    print("Escuchado:", texto)
                    print(
                        "Comando:",
                        comando or "ninguno"
                    )


# ==========================================
# RECONOCIMIENTO CON YOLO
# ==========================================

def detectar_peluches():
    global ultimo_frame, ultimo_jpeg_yolo

    historial = deque(
        maxlen=VENTANA_DETECCION
    )

    print("Iniciando reconocimiento YOLO...")
    ultimo_envio_cam = 0.0
    ultimo_aviso_cam = 0.0

    with requests.Session() as sesion:

        while True:
            try:
                respuesta = sesion.get(
                    URL_CAMARA,
                    timeout=5
                )

                respuesta.raise_for_status()

                imagen = cv2.imdecode(
                    np.frombuffer(
                        respuesta.content,
                        dtype=np.uint8
                    ),
                    cv2.IMREAD_COLOR
                )

                if imagen is None:
                    raise ValueError(
                        "Imagen invalida"
                    )

                resultados = modelo_yolo.predict(
                    source=imagen,
                    conf=CONFIANZA,
                    imgsz=640,
                    verbose=False
                )

                resultado = resultados[0]

                # Dibujar recuadros y nombres de los peluches
                imagen_detectada = resultado.plot()

                # Compartir la imagen YA procesada con la pagina web.
                # No ejecutamos YOLO dos veces.
                correcto_jpeg, buffer_jpeg = cv2.imencode(
                    ".jpg", imagen_detectada,
                    [int(cv2.IMWRITE_JPEG_QUALITY), 75]
                )
                if correcto_jpeg:
                    with bloqueo:
                        ultimo_jpeg_yolo = buffer_jpeg.tobytes()

                # Mostrar la camara con YOLO
                cv2.imshow(
                    "Comedero Inteligente - YOLO + Vosk",
                    imagen_detectada
                )

                # Actualizar ventana
                cv2.waitKey(1)

                presentes = {
                    "peluche_1": False,
                    "peluche_2": False
                }

                confianzas = {
                    "peluche_1": 0.0,
                    "peluche_2": 0.0
                }

                cajas_web = []
                alto_imagen, ancho_imagen = imagen.shape[:2]
                for caja in resultado.boxes:
                    clase = int(caja.cls[0])
                    nombre = modelo_yolo.names[clase]
                    confianza = float(caja.conf[0])

                    if nombre in presentes:
                        # Coordenadas normalizadas (0-1000) para cualquier tamano de video.
                        x1, y1, x2, y2 = [float(v) for v in caja.xyxy[0].tolist()]
                        if len(cajas_web) < 6:
                            valores = [
                                round(max(0, min(1000, x1 / ancho_imagen * 1000))),
                                round(max(0, min(1000, y1 / alto_imagen * 1000))),
                                round(max(0, min(1000, x2 / ancho_imagen * 1000))),
                                round(max(0, min(1000, y2 / alto_imagen * 1000))),
                                1 if nombre == "peluche_1" else 2,
                                round(confianza * 1000),
                            ]
                            cajas_web.append(",".join(map(str, valores)))
                        presentes[nombre] = True

                        confianzas[nombre] = max(
                            confianzas[nombre],
                            confianza
                        )

                historial.append(presentes)

                # Confirmar cada peluche si aparece en 2 de 3 capturas.
                estable = len(historial) == VENTANA_DETECCION

                detectados = {
                    "peluche_1": False,
                    "peluche_2": False
                }

                for nombre in detectados:
                    cantidad_detecciones = sum(
                        1 for registro in historial
                        if registro.get(nombre, False)
                    )
                    detectados[nombre] = (
                        estable and cantidad_detecciones >= 2
                    )

                with bloqueo:
                    estado_yolo.update({
                        "camara_conectada": True,
                        "deteccion_estable": estable,
                        "peluche_1": detectados.get("peluche_1", False),
                        "peluche_2": detectados.get("peluche_2", False),
                        "confianza_1": confianzas["peluche_1"],
                        "confianza_2": confianzas["peluche_2"],
                        "ultima_actualizacion": time.time()
                    })

                    ultimo_frame = time.monotonic()

                # Publicar los estados estables en la ESP32-CAM.
                # Si falla la publicacion, YOLO, Flask y Vosk siguen activos.
                ahora_envio = time.monotonic()
                if ahora_envio - ultimo_envio_cam >= 1.0:
                    ultimo_envio_cam = ahora_envio
                    try:
                        envio = sesion.post(
                            URL_ESP32CAM_YOLO,
                            data={
                                "camara": "1",
                                "estable": "1" if estable else "0",
                                "p1": "1" if detectados["peluche_1"] else "0",
                                "p2": "1" if detectados["peluche_2"] else "0",
                                "c1": f'{confianzas["peluche_1"]:.4f}',
                                "c2": f'{confianzas["peluche_2"]:.4f}',
                                "cajas": ";".join(cajas_web),
                            },
                            timeout=1.0,
                        )
                        envio.raise_for_status()
                    except requests.RequestException as error_envio:
                        if ahora_envio - ultimo_aviso_cam >= 10.0:
                            print("Aviso: no se pudieron publicar detecciones en ESP32-CAM:", error_envio)
                            ultimo_aviso_cam = ahora_envio

            except Exception as error:

                print("Error YOLO/camara:", error)

                historial.clear()
                with bloqueo:
                    ultimo_jpeg_yolo = None

                with bloqueo:
                    estado_yolo.update({
                        "camara_conectada": False,
                        "deteccion_estable": False,
                        "peluche_1": False,
                        "peluche_2": False,
                        "confianza_1": 0.0,
                        "confianza_2": 0.0,
                        "ultima_actualizacion": None
                    })

                    ultimo_frame = 0.0

                time.sleep(2)
                continue

            time.sleep(INTERVALO_CAMARA)


# ==========================================
# ENDPOINTS FLASK
# ==========================================

@app.after_request
def permitir_consultas_desde_web(respuesta):
    # La pagina web se sirve desde la ESP32-CAM (192.168.4.1),
    # mientras Flask esta en la PC (192.168.4.3).
    respuesta.headers["Access-Control-Allow-Origin"] = "*"
    return respuesta


@app.get("/")
def inicio():
    return jsonify({
        "servidor": "Comedero inteligente",
        "estado": "activo",
        "voz": "Vosk",
        "vision": "YOLO11"
    })


@app.post("/reconocer")
def reconocer():
    if request.mimetype != "application/octet-stream":
        return jsonify({
            "error": "Se requiere audio PCM"
        }), 415

    audio = request.get_data(
        cache=False,
        as_text=False
    )

    if len(audio) < 3200 or len(audio) > 160000:
        return jsonify({
            "error": "Duracion de audio invalida"
        }), 400

    if len(audio) % 2 != 0:
        return jsonify({
            "error": "Audio PCM incompleto"
        }), 400

    return jsonify(procesar_audio(audio))


@app.get("/video-yolo")
def video_yolo():
    # Si YOLO aun no ha producido una imagen, devolver error para
    # que la web pueda usar el streaming original de la ESP32-CAM.
    with bloqueo:
        disponible = (ultimo_jpeg_yolo is not None and
                      ultimo_frame > 0 and
                      time.monotonic() - ultimo_frame <= TIEMPO_MAXIMO_DATOS)
    if not disponible:
        return "YOLO sin imagen disponible", 503

    def generar():
        anterior = None
        while True:
            with bloqueo:
                vigente = (ultimo_frame > 0 and
                           time.monotonic() - ultimo_frame <= TIEMPO_MAXIMO_DATOS)
                fotograma = ultimo_jpeg_yolo if vigente else None
            if fotograma is None:
                break
            if fotograma is not anterior:
                yield (b"--frame\r\n"
                       b"Content-Type: image/jpeg\r\n"
                       b"Content-Length: " + str(len(fotograma)).encode() +
                       b"\r\n\r\n" + fotograma + b"\r\n")
                anterior = fotograma
            time.sleep(0.12)

    return Response(generar(), mimetype="multipart/x-mixed-replace; boundary=frame",
                    headers={"Cache-Control": "no-store",
                             "Access-Control-Allow-Origin": "*"})


@app.get("/detecciones")
def detecciones():
    with bloqueo:
        datos = estado_yolo.copy()

        reciente = (
            ultimo_frame > 0
            and time.monotonic() - ultimo_frame
            <= TIEMPO_MAXIMO_DATOS
        )

    if not reciente:
        datos.update({
            "camara_conectada": False,
            "deteccion_estable": False,
            "peluche_1": False,
            "peluche_2": False,
            "confianza_1": 0.0,
            "confianza_2": 0.0
        })

    return jsonify(datos)


# ==========================================
# INICIAR SERVIDOR
# ==========================================

if __name__ == "__main__":
    hilo_flask = threading.Thread(
        target=lambda: app.run(
            host="0.0.0.0",
            port=PUERTO,
            debug=False,
            use_reloader=False,
            threaded=True
        ),
        daemon=True
    )
    hilo_flask.start()

    hilo_voz = threading.Thread(
        target=prueba_microfono,
        daemon=True
    )
    hilo_voz.start()

    print("\nServidor Flask activo")
    print("Puerto:", PUERTO)
    print("Endpoint: /detecciones")
    print("Endpoint: /video-yolo")
    print("Endpoint: /reconocer")

    # OpenCV muestra la ventana desde el hilo principal.
    try:
        detectar_peluches()
    except KeyboardInterrupt:
        print("\nServidor detenido")
    finally:
        cv2.destroyAllWindows()
