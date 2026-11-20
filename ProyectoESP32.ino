// ============================================================
//  Mini Invernadero Inteligente IoT - Evaluación N°2
//  Seguridad y almacenamiento de datos con ESP32 y Supabase
//  Placa: ESP32 DevKit V1
//
//  La página web está en la carpeta data/ y se sube a SPIFFS:
//    login.html, index.html, style.css, script.js
//
//  Librerías necesarias (Administrador de Bibliotecas):
//    - ESPAsyncWebServer  (autor: ESP32Async)
//    - AsyncTCP           (autor: ESP32Async)
//    - ArduinoJson        (autor: Benoit Blanchon)
//    - DHT sensor library (autor: Adafruit)
//    - ESP32Servo
// ============================================================

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <SPIFFS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "mbedtls/sha256.h"
#include <DHT.h>
#include <ESP32Servo.h>

// ------------------------------------------------------------
//  Datos de la red WiFi (cambiar por los de tu red)
// ------------------------------------------------------------
const char* nombreRed = "NOMBRE_DE_TU_RED";
const char* claveRed  = "CLAVE_DE_TU_RED";

// ------------------------------------------------------------
//  Usuario autorizado
//  La contraseña NO se guarda: solo su hash SHA-256.
//  Este hash corresponde a "Admin2026@", la misma clave del simulador.
// ------------------------------------------------------------
const String USUARIO    = "admin";
const String HASH_CLAVE = "03d1048c73d0219c114fd3d93ccf6509b5a98b040e875f7deec52798021dfe81";

// ------------------------------------------------------------
//  Supabase (completar con los datos del proyecto)
//  Mientras diga TU-PROYECTO, el envío queda desactivado.
// ------------------------------------------------------------
const char* SUPABASE_URL   = "https://TU-PROYECTO.supabase.co";
const char* SUPABASE_KEY   = "TU_CLAVE_PUBLICA";
const char* SUPABASE_TABLA = "sensores";
const char* ID_DISPOSITIVO = "ESP32_INVERNADERO";

// ------------------------------------------------------------
//  Pines de la ESP32
// ------------------------------------------------------------
#define PIN_SENSOR_DHT         4
#define PIN_SERVO              13
#define PIN_LED_VERDE          26
#define PIN_LED_AMARILLO_1     32
#define PIN_LED_AMARILLO_2     33
#define PIN_LED_ROJO_TEMP_ALTA 25
#define PIN_LED_ROJO_TEMP_BAJA 27
#define PIN_LED_ROJO_HUM_ALTA  18
#define PIN_LED_ROJO_HUM_BAJA  19

// ------------------------------------------------------------
//  Valores límite del invernadero
// ------------------------------------------------------------
const float TEMPERATURA_MAXIMA = 25;
const float TEMPERATURA_MINIMA = 5;
const float HUMEDAD_MINIMA     = 30;
const float HUMEDAD_MAXIMA     = 80;

// ------------------------------------------------------------
//  Parámetros de seguridad
// ------------------------------------------------------------
const unsigned long TIEMPO_SESION  = 10UL * 60UL * 1000UL;  // 10 minutos de inactividad
const int           MAX_INTENTOS   = 5;                     // intentos fallidos antes del bloqueo
const unsigned long TIEMPO_BLOQUEO = 5UL * 60UL * 1000UL;   // 5 minutos de bloqueo
const char*         ARCHIVO_LOG    = "/access_log.txt";
const size_t        TAMANO_MAX_LOG = 32 * 1024;             // si el registro pasa de 32 KB se reinicia

// Tiempos de las tareas periódicas
const unsigned long INTERVALO_SENSOR    = 2000;    // leer el DHT11 cada 2 s
const unsigned long INTERVALO_SUPABASE  = 60000;   // enviar a Supabase cada 60 s
const unsigned long INTERVALO_WIFI      = 10000;   // revisar la conexión WiFi cada 10 s

// ------------------------------------------------------------
//  Objetos principales
// ------------------------------------------------------------
DHT sensorDHT(PIN_SENSOR_DHT, DHT11);
Servo servoVentana;
AsyncWebServer servidor(80);

// ------------------------------------------------------------
//  Estado del invernadero
// ------------------------------------------------------------
float temperatura = 0;
float humedad     = 0;

bool lecturaCorrecta    = false;
bool ventanaAbierta     = false;
bool luzCalidaEncendida = false;
bool modoAutomatico     = true;

bool alertaTempAlta = false;
bool alertaTempBaja = false;
bool alertaHumAlta  = false;
bool alertaHumBaja  = false;
bool hayAlerta      = false;

bool estadoParpadeo = false;

unsigned long ultimaLectura    = 0;
unsigned long ultimoParpadeo   = 0;
unsigned long ultimoEnvio      = 0;
unsigned long ultimaRevisionWifi = 0;

// Resultado del último envío a Supabase, para mostrarlo en la página
String ultimoEnvioSupabase = "";
bool   ultimoEnvioOk       = false;

// ------------------------------------------------------------
//  Estado de la seguridad
// ------------------------------------------------------------
String sesionID = "";                 // vacío = no hay sesión abierta
unsigned long ultimaActividad = 0;    // momento de la última acción del usuario

int intentosFallidos = 0;
bool bloqueado = false;
unsigned long inicioBloqueo = 0;


// ============================================================
//  FUNCIONES DEL INVERNADERO (mismas del proyecto original)
// ============================================================

// Abre (90°) o cierra (0°) la ventana
void moverVentana(bool abrir) {
  ventanaAbierta = abrir;
  servoVentana.write(abrir ? 90 : 0);
}

// Enciende o apaga los dos LEDs amarillos (luz cálida)
void controlarLuzCalida(bool encender) {
  luzCalidaEncendida = encender;
  digitalWrite(PIN_LED_AMARILLO_1, encender);
  digitalWrite(PIN_LED_AMARILLO_2, encender);
}

// Revisa las lecturas y decide qué hacer con los actuadores
void evaluarCondiciones() {
  if (!lecturaCorrecta) {
    hayAlerta = true;
    digitalWrite(PIN_LED_VERDE, LOW);
    return;
  }

  alertaTempAlta = temperatura > TEMPERATURA_MAXIMA;
  alertaTempBaja = temperatura < TEMPERATURA_MINIMA;
  alertaHumAlta  = humedad > HUMEDAD_MAXIMA;
  alertaHumBaja  = humedad < HUMEDAD_MINIMA;
  hayAlerta = alertaTempAlta || alertaTempBaja || alertaHumAlta || alertaHumBaja;

  if (modoAutomatico) {
    controlarLuzCalida(alertaTempBaja);
    bool debeAbrirVentana = (alertaTempAlta || alertaHumAlta) && !alertaTempBaja;
    moverVentana(debeAbrirVentana);
  }

  digitalWrite(PIN_LED_VERDE, !hayAlerta);
}

// Lee el sensor DHT11 y guarda los valores
void leerSensor() {
  float tempLeida = sensorDHT.readTemperature();
  float humLeida  = sensorDHT.readHumidity();

  if (isnan(tempLeida) || isnan(humLeida)) {
    lecturaCorrecta = false;
  } else {
    temperatura = tempLeida;
    humedad = humLeida;
    lecturaCorrecta = true;
  }
  evaluarCondiciones();
}

// Cada LED rojo parpadea si su condición está activa; si el sensor falla, parpadean los cuatro
void parpadearLedsRojos() {
  if (millis() - ultimoParpadeo >= 500) {
    ultimoParpadeo = millis();
    estadoParpadeo = !estadoParpadeo;
  }
  bool errorSensor = !lecturaCorrecta;
  digitalWrite(PIN_LED_ROJO_TEMP_ALTA, (alertaTempAlta || errorSensor) && estadoParpadeo);
  digitalWrite(PIN_LED_ROJO_TEMP_BAJA, (alertaTempBaja || errorSensor) && estadoParpadeo);
  digitalWrite(PIN_LED_ROJO_HUM_ALTA,  (alertaHumAlta  || errorSensor) && estadoParpadeo);
  digitalWrite(PIN_LED_ROJO_HUM_BAJA,  (alertaHumBaja  || errorSensor) && estadoParpadeo);
}


// ============================================================
//  SEGURIDAD: SHA-256
// ============================================================

// Convierte un texto en su hash SHA-256, escrito en hexadecimal
String sha256(const String& texto) {
  byte hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);   // 0 = SHA-256
  mbedtls_sha256_update(&ctx, (const unsigned char*)texto.c_str(), texto.length());
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);

  String resultado = "";
  for (int i = 0; i < 32; i++) {
    char hex[3];
    sprintf(hex, "%02x", hash[i]);
    resultado += hex;
  }
  return resultado;
}


// ============================================================
//  SEGURIDAD: REGISTRO DE EVENTOS EN SPIFFS
// ============================================================

// Fecha y hora actual, obtenida por NTP
String fechaHora() {
  struct tm t;
  if (!getLocalTime(&t, 50)) return "sin hora";
  char texto[20];
  strftime(texto, sizeof(texto), "%Y-%m-%d %H:%M:%S", &t);
  return String(texto);
}

// Quita saltos de línea y acorta el texto, para que nadie pueda
// escribir líneas falsas en el registro a través del campo usuario
String limpiarTexto(String texto) {
  texto.replace("\n", " ");
  texto.replace("\r", " ");
  if (texto.length() > 32) texto = texto.substring(0, 32);
  return texto;
}

// Agrega una línea al registro: fecha - usuario - evento
void registrarLog(const String& usuario, const String& evento) {
  // Si el archivo creció demasiado se reinicia, para no llenar la memoria
  File revision = SPIFFS.open(ARCHIVO_LOG, FILE_READ);
  if (revision) {
    bool muyGrande = revision.size() > TAMANO_MAX_LOG;
    revision.close();
    if (muyGrande) SPIFFS.remove(ARCHIVO_LOG);
  }

  File archivo = SPIFFS.open(ARCHIVO_LOG, FILE_APPEND);
  if (!archivo) return;
  String linea = fechaHora() + " - " + limpiarTexto(usuario) + " - " + evento;
  archivo.println(linea);
  archivo.close();
  Serial.println("[LOG] " + linea);
}


// ============================================================
//  SEGURIDAD: SESIONES
// ============================================================

// Crea un identificador de sesión aleatorio de 32 caracteres.
// esp_random() usa el generador de números aleatorios por hardware.
String crearSesionID() {
  String id = "";
  for (int i = 0; i < 4; i++) {
    char parte[9];
    sprintf(parte, "%08lx", (unsigned long)esp_random());
    id += parte;
  }
  return id;
}

// Extrae el valor de la cookie SESSIONID que envía el navegador
String leerCookieSesion(AsyncWebServerRequest *request) {
  if (!request->hasHeader("Cookie")) return "";
  String cookies = request->header("Cookie");
  int inicio = cookies.indexOf("SESSIONID=");
  if (inicio < 0) return "";
  inicio += 10;
  int fin = cookies.indexOf(';', inicio);
  return fin < 0 ? cookies.substring(inicio) : cookies.substring(inicio, fin);
}

// Revisa si la petición pertenece a la sesión abierta y si no ha expirado.
// Las lecturas automáticas de la página traen el encabezado X-Auto: esas
// NO cuentan como actividad, para que la sesión expire si el usuario no hace nada.
bool sesionValida(AsyncWebServerRequest *request) {
  if (sesionID == "") return false;

  if (millis() - ultimaActividad > TIEMPO_SESION) {
    registrarLog(USUARIO, "SESSION EXPIRED");
    sesionID = "";
    return false;
  }

  if (leerCookieSesion(request) != sesionID) return false;

  if (!request->hasHeader("X-Auto")) ultimaActividad = millis();
  return true;
}

// Indica si la cuenta sigue bloqueada; al pasar los 5 minutos se desbloquea sola
bool estaBloqueado() {
  if (bloqueado && millis() - inicioBloqueo >= TIEMPO_BLOQUEO) {
    bloqueado = false;
  }
  return bloqueado;
}

// Segundos que faltan para terminar el bloqueo
unsigned long segundosBloqueo() {
  return (TIEMPO_BLOQUEO - (millis() - inicioBloqueo)) / 1000 + 1;
}


// ============================================================
//  DATOS PARA LA PÁGINA
// ============================================================

// Arma el JSON que pide /api/data
String armarJSONDatos() {
  String json = "{";

  // Si el sensor falló se envía null, como pide la página
  if (lecturaCorrecta) {
    json += "\"temperatura\":" + String(temperatura, 1);
    json += ",\"humedad\":" + String(humedad, 0);
  } else {
    json += "\"temperatura\":null,\"humedad\":null";
  }

  json += ",\"ventana\":\"" + String(ventanaAbierta ? "abierta" : "cerrada") + "\"";
  json += ",\"luz\":\"" + String(luzCalidaEncendida ? "on" : "off") + "\"";
  json += ",\"modo\":\"" + String(modoAutomatico ? "auto" : "manual") + "\"";
  json += ",\"estado\":\"" + String(hayAlerta ? "alerta" : "ok") + "\"";

  // Lista con el nombre de cada alerta activa
  json += ",\"alertas\":[";
  bool primera = true;
  auto agregar = [&](bool activa, const char* nombre) {
    if (!activa) return;
    if (!primera) json += ",";
    json += "\"" + String(nombre) + "\"";
    primera = false;
  };
  agregar(alertaTempAlta, "Temp. alta");
  agregar(alertaTempBaja, "Temp. baja");
  agregar(alertaHumAlta,  "Hum. alta");
  agregar(alertaHumBaja,  "Hum. baja");
  json += "]";

  // Resultado del último envío a Supabase
  if (ultimoEnvioSupabase == "") {
    json += ",\"supabase\":null";
  } else {
    json += ",\"supabase\":{\"ultimo_envio\":\"" + ultimoEnvioSupabase + "\",\"ok\":" + String(ultimoEnvioOk ? "true" : "false") + "}";
  }

  json += "}";
  return json;
}


// ============================================================
//  SUPABASE: envío de lecturas por HTTP POST
// ============================================================

// Envía temperatura y humedad como dos registros de la tabla.
// La columna fecha_hora la completa Supabase con la hora de llegada.
void enviarSupabase() {
  if (strstr(SUPABASE_URL, "TU-PROYECTO") != nullptr) return;   // aún no configurado
  if (WiFi.status() != WL_CONNECTED || !lecturaCorrecta) return;

  WiFiClientSecure cliente;
  cliente.setInsecure();   // HTTPS sin verificar el certificado: aceptable solo en un prototipo

  HTTPClient http;
  http.setTimeout(5000);
  http.begin(cliente, String(SUPABASE_URL) + "/rest/v1/" + SUPABASE_TABLA);
  http.addHeader("apikey", SUPABASE_KEY);
  // Las claves antiguas (anon) también van en Authorization; las nuevas (sb_...) no
  if (strncmp(SUPABASE_KEY, "sb_", 3) != 0) {
    http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=minimal");

  String json = "[";
  json += "{\"id_dispositivo\":\"" + String(ID_DISPOSITIVO) + "\",\"tipo_sensor\":\"DHT11_temperatura\",\"valor_sensor\":" + String(temperatura, 1) + "},";
  json += "{\"id_dispositivo\":\"" + String(ID_DISPOSITIVO) + "\",\"tipo_sensor\":\"DHT11_humedad\",\"valor_sensor\":" + String(humedad, 0) + "}";
  json += "]";

  int codigo = http.POST(json);
  http.end();

  ultimoEnvioSupabase = fechaHora();
  ultimoEnvioOk = (codigo == 201);   // 201 = registros creados
  Serial.printf("[SUPABASE] Respuesta %d\n", codigo);
}


// ============================================================
//  RUTAS DEL SERVIDOR WEB
// ============================================================
void configurarRutas() {

  // ---------- Rutas públicas: no necesitan sesión ----------

  // Página de login
  servidor.on("/login", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/login.html", "text/html");
  });
  servidor.on("/login.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->redirect("/login");
  });

  // Estilos y script: no contienen claves ni datos
  servidor.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/style.css", "text/css");
  });
  servidor.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/script.js", "application/javascript");
  });

  // Inicio de sesión: recibe {"usuario": "...", "password": "..."} por POST
  AsyncCallbackJsonWebHandler *rutaLogin = new AsyncCallbackJsonWebHandler("/api/login",
    [](AsyncWebServerRequest *request, JsonVariant &json) {

    // 1. Si la cuenta está bloqueada, no se revisa nada
    if (estaBloqueado()) {
      request->send(403, "application/json",
        "{\"bloqueado\":true,\"segundos\":" + String(segundosBloqueo()) + "}");
      return;
    }

    JsonObject datos = json.as<JsonObject>();
    String usuario  = datos["usuario"]  | "";
    String password = datos["password"] | "";

    // 2. Se compara el usuario y el HASH de la contraseña, nunca la contraseña misma
    if (usuario == USUARIO && sha256(password) == HASH_CLAVE) {
      intentosFallidos = 0;
      sesionID = crearSesionID();
      ultimaActividad = millis();
      registrarLog(usuario, "LOGIN OK");

      // La cookie HttpOnly no se puede leer desde JavaScript
      String cookie = "SESSIONID=" + sesionID + "; Path=/; HttpOnly";
      AsyncWebServerResponse *respuesta = request->beginResponse(200, "application/json", "{\"ok\":true}");
      respuesta->addHeader("Set-Cookie", cookie.c_str());
      request->send(respuesta);
      return;
    }

    // 3. Login incorrecto: se cuenta el intento
    intentosFallidos++;
    registrarLog(usuario, "LOGIN FAILED");

    // 4. Al quinto intento fallido se bloquea la cuenta por 5 minutos
    if (intentosFallidos >= MAX_INTENTOS) {
      bloqueado = true;
      inicioBloqueo = millis();
      intentosFallidos = 0;
      registrarLog(usuario, "ACCOUNT LOCKED");
      request->send(403, "application/json",
        "{\"bloqueado\":true,\"segundos\":" + String(TIEMPO_BLOQUEO / 1000) + "}");
      return;
    }

    request->send(401, "application/json",
      "{\"ok\":false,\"intentos_restantes\":" + String(MAX_INTENTOS - intentosFallidos) + "}");
  });
  servidor.addHandler(rutaLogin);

  // Cierre de sesión
  servidor.on("/logout", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (sesionValida(request)) registrarLog(USUARIO, "LOGOUT");
    sesionID = "";
    AsyncWebServerResponse *respuesta = request->beginResponse(302);
    respuesta->addHeader("Location", "/login");
    respuesta->addHeader("Set-Cookie", "SESSIONID=; Path=/; Max-Age=0");   // borra la cookie
    request->send(respuesta);
  });

  // ---------- Rutas protegidas: necesitan sesión válida ----------

  // Panel principal: sin sesión, vuelve al login
  servidor.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!sesionValida(request)) {
      request->redirect("/login?motivo=denegado");
      return;
    }
    request->send(SPIFFS, "/index.html", "text/html");
  });

  // Lecturas y estado del invernadero
  servidor.on("/api/data", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!sesionValida(request)) {
      request->send(403, "text/plain", "Acceso denegado");
      return;
    }
    request->send(200, "application/json", armarJSONDatos());
  });

  // Registro de accesos guardado en SPIFFS
  servidor.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!sesionValida(request)) {
      request->send(403, "text/plain", "Acceso denegado");
      return;
    }
    if (!SPIFFS.exists(ARCHIVO_LOG)) {
      request->send(200, "text/plain", "");
      return;
    }
    request->send(SPIFFS, ARCHIVO_LOG, "text/plain");
  });

  // Control de actuadores: recibe {"ventana":"abrir"}, {"luz":"on"} o {"modo":"manual"}
  AsyncCallbackJsonWebHandler *rutaControl = new AsyncCallbackJsonWebHandler("/api/control",
    [](AsyncWebServerRequest *request, JsonVariant &json) {

    if (!sesionValida(request)) {
      request->send(403, "text/plain", "Acceso denegado");
      return;
    }

    JsonObject orden = json.as<JsonObject>();
    String modo    = orden["modo"]    | "";
    String ventana = orden["ventana"] | "";
    String luz     = orden["luz"]     | "";

    // Solo se aceptan valores conocidos: cualquier otro se ignora
    if (modo == "auto")   modoAutomatico = true;
    if (modo == "manual") modoAutomatico = false;

    // La ventana y la luz solo se controlan a mano en modo manual
    if (!modoAutomatico) {
      if (ventana == "abrir")  moverVentana(true);
      if (ventana == "cerrar") moverVentana(false);
      if (luz == "on")  controlarLuzCalida(true);
      if (luz == "off") controlarLuzCalida(false);
    }

    evaluarCondiciones();
    request->send(200, "text/plain", "OK");
  });
  servidor.addHandler(rutaControl);

  // Cualquier otra ruta no existe.
  // No se usa serveStatic para no exponer el archivo de registro.
  servidor.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "No encontrado");
  });
}


// ============================================================
//  SETUP: se ejecuta una sola vez al encender
// ============================================================
void setup() {
  Serial.begin(115200);

  // LEDs como salidas
  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_AMARILLO_1, OUTPUT);
  pinMode(PIN_LED_AMARILLO_2, OUTPUT);
  pinMode(PIN_LED_ROJO_TEMP_ALTA, OUTPUT);
  pinMode(PIN_LED_ROJO_TEMP_BAJA, OUTPUT);
  pinMode(PIN_LED_ROJO_HUM_ALTA, OUTPUT);
  pinMode(PIN_LED_ROJO_HUM_BAJA, OUTPUT);

  // Sensor y servo; la ventana parte cerrada
  sensorDHT.begin();
  servoVentana.attach(PIN_SERVO);
  moverVentana(false);

  // Sistema de archivos con la página web y el registro
  if (!SPIFFS.begin(true)) {
    Serial.println("Error al montar SPIFFS");
  }

  // Conexión WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(nombreRed, claveRed);
  Serial.print("Conectando al WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Abrir en el navegador: http://");
  Serial.println(WiFi.localIP());

  // Hora de Chile continental por NTP, para el registro y Supabase
  configTzTime("<-04>4<-03>,M9.1.6/24,M4.1.6/24", "pool.ntp.org");

  // Rutas del servidor
  configurarRutas();
  servidor.begin();

  registrarLog("sistema", "INICIO");
}


// ============================================================
//  LOOP: se repite constantemente
//  El servidor web es asíncrono, por eso no necesita handleClient()
// ============================================================
void loop() {
  // Leer el sensor cada 2 segundos
  if (millis() - ultimaLectura >= INTERVALO_SENSOR) {
    ultimaLectura = millis();
    leerSensor();
  }

  // Parpadeo de los LEDs rojos
  parpadearLedsRojos();

  // Enviar los datos a Supabase cada 60 segundos
  if (millis() - ultimoEnvio >= INTERVALO_SUPABASE) {
    ultimoEnvio = millis();
    enviarSupabase();
  }

  // Reconexión automática si se cae el WiFi
  if (millis() - ultimaRevisionWifi >= INTERVALO_WIFI) {
    ultimaRevisionWifi = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi desconectado, reintentando...");
      WiFi.reconnect();
    }
  }
}
