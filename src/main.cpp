/******************************************************************************************************************************************
  ESP32 Broker MQTT em modo Station
  Autor : Robson Brasil

  Dispositivos : ESP32 WROOM32
  Versão : 3.0.3
  Última Modificação : 16/05/2026
******************************************************************************************************************************************/

#include <Arduino.h>

// ==========================================
// Includes
// ==========================================
#include <sMQTTBroker.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>

// ==========================================
// Versão do Firmware (controle de OTA)
// ==========================================
#define FIRMWARE_VERSION "3.0.3"

// ==========================================
// Pino do LED built-in para feedback visual
// ==========================================
#ifndef LED_BUILTIN
  #define LED_BUILTIN 2
#endif

// ==========================================
// Constantes de controle
// ==========================================
const unsigned long RECONNECT_BACKOFF_INITIAL  = 2000;
const unsigned long RECONNECT_BACKOFF_MAX      = 30000;
const int           MAX_RECONNECT_ATTEMPTS     = 20;
const unsigned long WIFI_CONNECT_TIMEOUT       = 15000;

// ==========================================
// Variáveis de controle WiFi
// ==========================================
static volatile bool     wifiNeedsReconnect    = false;
static unsigned long     lastReconnectAttempt  = 0;
static int               reconnectAttempts     = 0;
static unsigned long     reconnectBackoff      = RECONNECT_BACKOFF_INITIAL;
static bool              wifiWasReconnected    = false;
static unsigned long     mqttPort              = 1883;

// ==========================================
// WiFi Manager + MQTT Credentials
// ==========================================
#include <Preferences.h>
Preferences preferences;

WiFiManager wm;
WiFiManagerParameter *mqtt_login_param;
WiFiManagerParameter *mqtt_password_param;
WiFiManagerParameter *ota_password_param;
bool shouldSaveConfig = false;
bool wifiConfigMode = false;
unsigned long configPortalStart = 0;
const unsigned long CONFIG_PORTAL_TIMEOUT = 300000;

// Variáveis para credenciais (carregadas do NVS)
String mqttUser = "";
String mqttPass = "";
String otaPass = "";

// ==========================================
// LED Status
// ==========================================
enum LEDState {
    LED_OFF,
    LED_ON,
    LED_SLOW_BLINK,   // 1s - aguardando botão
    LED_FAST_BLINK,   // 200ms - portal ativo
    LED_MEDIUM_BLINK  // 500ms - reconectando
};
LEDState currentLEDState = LED_SLOW_BLINK;

// ==========================================
// Classe Broker MQTT com autenticação
// ==========================================
class MyBroker : public sMQTTBroker
{
public:
    bool onEvent(sMQTTEvent *event) override
    {
        switch (event->Type())
        {
        case NewClient_sMQTTEventType:
        {
            sMQTTNewClientEvent *e = (sMQTTNewClientEvent *)event;

            if (mqttUser.length() == 0 || mqttPass.length() == 0) {
                Serial.println("[MQTT] Nenhuma credencial configurada. Configure via WiFi Manager.");
                return false;
            }

            Serial.printf("[MQTT] Usando credenciais - Login: %s\n", mqttUser.c_str());

            String loginStr = String(e->Login().c_str());
            String passStr = String(e->Password().c_str());

            if ((loginStr != mqttUser) || (passStr != mqttPass))
            {
                Serial.println("[MQTT] Login ou senha inválido(s)");
                Serial.printf("[MQTT] Recebido: %s / %s\n", loginStr.c_str(), passStr.c_str());
                return false;
            }
            Serial.println("[MQTT] Novo cliente autenticado");
        }
        break;

        case LostConnect_sMQTTEventType:
            Serial.println("[MQTT] Conexão perdida — sinalizando reconexão");
            wifiNeedsReconnect = true;
            break;

        case UnSubscribe_sMQTTEventType:
        case Subscribe_sMQTTEventType:
            break;
        }
        return true;
    }
};

MyBroker broker;

// ==========================================
// Hostname do dispositivo (usado em WiFi, mDNS, OTA)
// ==========================================
const char* DEVICE_HOSTNAME = "brokermqtt";

// ==========================================
// LED feedback helpers
// ==========================================
static bool otaInProgress = false;
void ledOff()  { digitalWrite(LED_BUILTIN, LOW); }
void ledOn()   { digitalWrite(LED_BUILTIN, HIGH); }

// ==========================================
// Configuração OTA
// ==========================================
void setupOTA()
{
    ArduinoOTA.setPort(3233);
    ArduinoOTA.setHostname(DEVICE_HOSTNAME);

    if (otaPass.length() > 0) {
        ArduinoOTA.setPassword(otaPass.c_str());
        Serial.println("[OTA] Senha OTA carregada do NVS");
    } else {
        Serial.println("[OTA] AVISO: Nenhuma senha OTA configurada!");
    }

    ArduinoOTA
        .onStart([]() {
            otaInProgress = true;
            String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
            
            Serial.println("\n[OTA] ========================================");
            Serial.println("[OTA] Iniciando atualização: " + type);
            Serial.println("[OTA] Firmware atual: v" FIRMWARE_VERSION);
            Serial.println("[OTA] ========================================");
        })
        .onEnd([]() {
            otaInProgress = false;
            ledOff();
            
            Serial.println("\n[OTA] ========================================");
            Serial.println("[OTA] Atualização concluída com sucesso!");
            Serial.println("[OTA] Reiniciando hardware agora...");
            Serial.println("[OTA] ========================================");
            
            Serial.flush();
            delay(1000);
        })
        .onProgress([](unsigned int progress, unsigned int total) {
            static bool ledState = false;
            ledState = !ledState;
            digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);

            if (total != 0) {
                unsigned int percent = (unsigned long)progress * 100UL / (unsigned long)total;
                Serial.printf("[OTA] Progresso: %u%%\r", percent);
            }
        })
        .onError([](ota_error_t error) {
            otaInProgress = false;
            ledOff();
            Serial.printf("\n[OTA] ERRO[%u]: ", error);
            switch (error) {
                case OTA_AUTH_ERROR:    Serial.println("Falha na autenticação"); break;
                case OTA_BEGIN_ERROR:   Serial.println("Falha ao iniciar"); break;
                case OTA_CONNECT_ERROR: Serial.println("Falha na conexão"); break;
                case OTA_RECEIVE_ERROR: Serial.println("Falha no recebimento"); break;
                case OTA_END_ERROR:     Serial.println("Falha ao finalizar"); break;
            }
        });

    ArduinoOTA.begin();
    Serial.println("[OTA] Servico pronto na porta 3233");
}

// ==========================================
// WiFi Manager Callbacks
// ==========================================
void configModeCallback(WiFiManager *wm) {
    wifiConfigMode = true;
    configPortalStart = millis();
    currentLEDState = LED_FAST_BLINK;
    Serial.println("[WiFiManager] === MODO CONFIG ATIVADO ===");
    Serial.println("[WiFiManager] Conecte-se ao WiFi: ESP32-MQTT-Broker");
    Serial.println("[WiFiManager] Senha WiFi: 12345678");
    Serial.println("[WiFiManager] Acesse: http://192.168.4.1");
    Serial.println("[WiFiManager] ==============================");
}

void saveConfigCallback() {
    shouldSaveConfig = true;
    Serial.println("[WiFiManager] !!! CONFIG SALVA !!!");
}

// ==========================================
// Gerenciamento de Credenciais (Preferences)
// ==========================================
void saveMQTTCredentials(const char* login, const char* password) {
    preferences.begin("mqtt-creds", false);
    preferences.putString("mqtt_login", login);
    preferences.putString("mqtt_password", password);
    preferences.end();
    Serial.println("[Preferences] Credenciais MQTT salvas!");
}

void saveOTACredentials(const char* password) {
    preferences.begin("ota-creds", false);
    preferences.putString("ota_password", password);
    preferences.end();
    Serial.println("[Preferences] Senha OTA salva!");
}

void loadMQTTCredentials() {
    preferences.begin("mqtt-creds", true);
    mqttUser = preferences.getString("mqtt_login", "");
    mqttPass = preferences.getString("mqtt_password", "");
    preferences.end();

    if (mqttUser.length() > 0 && mqttPass.length() > 0) {
        Serial.println("[Preferences] Credenciais MQTT carregadas do NVS");
    } else {
        Serial.println("[Preferences] Nenhuma credencial MQTT encontrada");
    }
}

void loadOTACredentials() {
    preferences.begin("ota-creds", true);
    otaPass = preferences.getString("ota_password", "");
    preferences.end();

    if (otaPass.length() > 0) {
        Serial.println("[Preferences] Senha OTA carregada do NVS");
    } else {
        Serial.println("[Preferences] Nenhuma senha OTA encontrada");
    }
}

// ==========================================
// mDNS Setup
// ==========================================
void setupMDNS() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[mDNS] WiFi nao conectado, abortando...");
        return;
    }

    MDNS.end();
    WiFi.setHostname(DEVICE_HOSTNAME);

    delay(500);
    Serial.println("[mDNS] Iniciando...");

    if (MDNS.begin(DEVICE_HOSTNAME)) {
        Serial.printf("[mDNS] Respondendo em: http://%s.local\n", DEVICE_HOSTNAME);
        Serial.printf("[mDNS] Broker MQTT: %s.local:1883\n", DEVICE_HOSTNAME);
        Serial.printf("[mDNS] OTA: %s.local:3233\n", DEVICE_HOSTNAME);

        MDNS.addService("mqtt", "tcp", 1883);
        MDNS.addService("http", "tcp", 3233);
    } else {
        Serial.println("[mDNS] FALHA!");
    }
}

// ==========================================
// Setup WiFi Manager
// ==========================================
bool setupWiFiManager() {
    loadMQTTCredentials();
    loadOTACredentials();

    wm.setDebugOutput(false);
    wm.setAPCallback(configModeCallback);
    wm.setSaveConfigCallback(saveConfigCallback);

    wm.setConfigPortalTimeout(0);
    wm.setConnectTimeout(60);
    wm.setMinimumSignalQuality(0);
    wm.setWiFiAutoReconnect(true);

    static char mqttLoginBuffer[32];
    static char mqttPasswordBuffer[32];
    static char otaPasswordBuffer[32];

    mqtt_login_param = new WiFiManagerParameter(
        "mqtt_login", "Login MQTT",
        mqttUser.length() > 0 ? mqttUser.c_str() : "",
        sizeof(mqttLoginBuffer),
        "placeholder='Digite o login do MQTT'"
    );

    mqtt_password_param = new WiFiManagerParameter(
        "mqtt_password", "Senha MQTT",
        mqttPass.length() > 0 ? mqttPass.c_str() : "",
        sizeof(mqttPasswordBuffer),
        "type='password' placeholder='Digite a senha do MQTT'"
    );

    ota_password_param = new WiFiManagerParameter(
        "ota_password", "Senha OTA",
        otaPass.length() > 0 ? otaPass.c_str() : "",
        sizeof(otaPasswordBuffer),
        "type='password' placeholder='Digite a senha do OTA'"
    );

    wm.addParameter(mqtt_login_param);
    wm.addParameter(mqtt_password_param);
    wm.addParameter(ota_password_param);

    WiFi.mode(WIFI_STA);

    Serial.println("[WiFiManager] Tentando conexão automática...");

    bool connected = false;
    for (int attempt = 1; attempt <= 3; attempt++) {
        Serial.printf("[WiFiManager] Tentativa de conexão %d/3...\n", attempt);

        WiFi.begin();

        int waitCount = 0;
        while (WiFi.status() != WL_CONNECTED && waitCount < 20) {
            delay(500);
            waitCount++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("[WiFiManager] Conectado com sucesso!");
            connected = true;
            break;
        }

        Serial.printf("[WiFiManager] Falha na tentativa %d. Aguardando 5s...\n", attempt);
        WiFi.disconnect(false);
        delay(5000);
    }

    if (connected) {
        if (shouldSaveConfig) {
            const char* mqttLoginValue = mqtt_login_param->getValue();
            const char* mqttPasswordValue = mqtt_password_param->getValue();
            if (strlen(mqttLoginValue) > 0 && strlen(mqttPasswordValue) > 0) {
                saveMQTTCredentials(mqttLoginValue, mqttPasswordValue);
                loadMQTTCredentials();
            }
            const char* otaPasswordValue = ota_password_param->getValue();
            if (strlen(otaPasswordValue) > 0) {
                saveOTACredentials(otaPasswordValue);
                loadOTACredentials();
            }
        }

        Serial.printf("[WiFiManager] Conectado! IP: %s\n", WiFi.localIP().toString().c_str());
        currentLEDState = LED_ON;
        wifiConfigMode = false;
        return true;
    }

    Serial.println("[WiFiManager] Sem WiFi conectado. Abrindo portal de configuração...");
    Serial.println("[WiFiManager] O portal ficará aberto até você configurar.");
    currentLEDState = LED_FAST_BLINK;
    wifiConfigMode = true;

    bool portalResult = wm.startConfigPortal("ESP32-MQTT-Broker", "12345678");

    if (portalResult) {
        Serial.println("[WiFiManager] Portal fechado. Conectando ao WiFi...");

        WiFi.mode(WIFI_STA);
        WiFi.begin();

        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 30) {
            delay(500);
            Serial.print(".");
            attempts++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\n[WiFiManager] Conectado! IP: %s\n", WiFi.localIP().toString().c_str());
        } else {
            Serial.println("\n[WiFiManager] Falha ao conectar!");
        }

        if (shouldSaveConfig) {
            const char* mqttLoginValue = mqtt_login_param->getValue();
            const char* mqttPasswordValue = mqtt_password_param->getValue();
            if (strlen(mqttLoginValue) > 0 && strlen(mqttPasswordValue) > 0) {
                saveMQTTCredentials(mqttLoginValue, mqttPasswordValue);
                loadMQTTCredentials();
            }
            const char* otaPasswordValue = ota_password_param->getValue();
            if (strlen(otaPasswordValue) > 0) {
                saveOTACredentials(otaPasswordValue);
                loadOTACredentials();
            }
        }
        currentLEDState = LED_ON;
        wifiConfigMode = false;
        return (WiFi.status() == WL_CONNECTED);
    } else {
        Serial.println("[WiFiManager] Portal expirado/cancelado. Reiniciando para tentar novamente...");
        currentLEDState = LED_SLOW_BLINK;
        delay(1000);
        ESP.restart();
        return false;
    }
}

// ==========================================
// Verificar botão GPIO 0 (Botão PRG) para ativar portal
// ==========================================
void checkConfigButton() {
    if (digitalRead(0) == LOW) {
        unsigned long pressStart = millis();
        while (digitalRead(0) == LOW) {
            delay(50);
            if (millis() - pressStart > 3000) {
                Serial.println("\n[WiFiManager] ========== ABRINDO PORTAL ==========");
                Serial.println("[WiFiManager] Mantenha press. 3s para ativar");
                Serial.println("[WiFiManager] =======================================");

                currentLEDState = LED_FAST_BLINK;
                wifiConfigMode = true;

                bool portalResult = wm.startConfigPortal("ESP32-MQTT-Broker", "12345678");

                wifiConfigMode = false;

                if (portalResult) {
                    Serial.println("[WiFiManager] Portal fechado. Conectando ao WiFi...");

                    WiFi.mode(WIFI_STA);
                    WiFi.begin();

                    int attempts = 0;
                    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
                        delay(500);
                        Serial.print(".");
                        attempts++;
                    }

                    if (WiFi.status() == WL_CONNECTED) {
                        Serial.printf("\n[WiFiManager] Conectado! IP: %s\n", WiFi.localIP().toString().c_str());
                    } else {
                        Serial.println("\n[WiFiManager] Falha ao conectar!");
                    }

                    if (shouldSaveConfig) {
                        const char* mqttLoginValue = mqtt_login_param->getValue();
                        const char* mqttPasswordValue = mqtt_password_param->getValue();
                        if (strlen(mqttLoginValue) > 0 && strlen(mqttPasswordValue) > 0) {
                            saveMQTTCredentials(mqttLoginValue, mqttPasswordValue);
                            loadMQTTCredentials();
                            Serial.println("[WiFiManager] Credenciais MQTT atualizadas!");
                        }
                        const char* otaPasswordValue = ota_password_param->getValue();
                        if (strlen(otaPasswordValue) > 0) {
                            saveOTACredentials(otaPasswordValue);
                            loadOTACredentials();
                            Serial.println("[WiFiManager] Senha OTA atualizada!");
                        }
                        shouldSaveConfig = false;
                    }
                } else {
                    Serial.println("[WiFiManager] Portal fechado");
                }

                currentLEDState = LED_ON;
                Serial.println("[WiFiManager] ========== FIM ==========\n");
                break;
            }
        }
    }
}

// ==========================================
// Update LED Status
// ==========================================
void updateLEDStatus() {
    static unsigned long lastToggle = 0;
    unsigned long interval;

    switch (currentLEDState) {
        case LED_OFF:
            digitalWrite(LED_BUILTIN, LOW);
            break;
        case LED_ON:
            digitalWrite(LED_BUILTIN, HIGH);
            break;
        case LED_SLOW_BLINK:
            interval = 1000;
            break;
        case LED_FAST_BLINK:
            interval = 200;
            break;
        case LED_MEDIUM_BLINK:
            interval = 500;
            break;
    }

    if (currentLEDState >= LED_SLOW_BLINK) {
        if (millis() - lastToggle >= interval) {
            static bool ledState = false;
            ledState = !ledState;
            digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
            lastToggle = millis();
        }
    }
}

// ==========================================
// WiFi Event Handler
// ==========================================
void onWiFiEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_START:
            Serial.println("[WiFi] Station iniciado.");
            break;
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            Serial.println("[WiFi] Conectado ao AP. Aguardando IP...");
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.print("[WiFi] IP Obtido: ");
            Serial.println(WiFi.localIP());
            reconnectAttempts = 0;
            wifiNeedsReconnect = false;
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            Serial.println("[WiFi] Conexão perdida. Tentando reconectar...");
            wifiNeedsReconnect = true;
            break;
        default: break;
    }
}

// ==========================================
// Verificar e reconectar WiFi
// ==========================================
bool checkWiFiConnection()
{
    if (WiFi.status() == WL_CONNECTED) {
        if (wifiNeedsReconnect) {
            wifiNeedsReconnect = false;
            reconnectAttempts = 0;
            reconnectBackoff = RECONNECT_BACKOFF_INITIAL;
            Serial.println("[WiFi] Reconexão confirmada com IP válido.");
            wifiWasReconnected = true;
        }
        if (currentLEDState != LED_ON) {
            currentLEDState = LED_ON;
        }
        return true;
    }

    currentLEDState = LED_MEDIUM_BLINK;

    unsigned long currentMillis = millis();
    if (currentMillis - lastReconnectAttempt < reconnectBackoff) {
        return false;
    }

    Serial.printf("[WiFi] Status: %d. Tentativa %d/%d (backoff: %lums)...\n",
                  WiFi.status(), reconnectAttempts + 1, MAX_RECONNECT_ATTEMPTS, reconnectBackoff);

    lastReconnectAttempt = currentMillis;
    reconnectAttempts++;

    if (reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
        Serial.println("[WiFi] Muitas falhas consecutivas. Reiniciando sistema...");
        currentLEDState = LED_FAST_BLINK;
        delay(1000);
        ESP.restart();
        return false;
    }

    WiFi.disconnect(true);
    delay(500);
    WiFi.mode(WIFI_STA);
    WiFi.begin();

    reconnectBackoff *= 2;
    if (reconnectBackoff > RECONNECT_BACKOFF_MAX) {
        reconnectBackoff = RECONNECT_BACKOFF_MAX;
    }

    return false;
}

// ==========================================
// Variáveis de controle
// ==========================================
bool mqttInitialized = false;

// ==========================================
// Setup principal
// ==========================================
void setup()
{
    Serial.begin(115200);
    delay(100);

    Serial.println("\n==========================================");
    Serial.println("  ESP32 Broker MQTT - Station Mode");
    Serial.printf("  Firmware: v%s\n", FIRMWARE_VERSION);
    Serial.println("==========================================\n");

    pinMode(LED_BUILTIN, OUTPUT);
    ledOff();

    pinMode(0, INPUT_PULLUP);

    if (setupWiFiManager()) {
        setupMDNS();

        const unsigned short mqttPortVal = 1883;
        mqttPort = mqttPortVal;
        broker.init(mqttPortVal);
        mqttInitialized = true;

        setupOTA();

        Serial.println("[System] Tudo pronto!\n");
    } else {
        Serial.println("[WiFiManager] Sem WiFi - iniciando modo de espera...");
        currentLEDState = LED_SLOW_BLINK;
    }
}

// ==========================================
// Loop principal
// ==========================================
void loop()
{
    if (mqttInitialized) {
        broker.update();

        if (!wifiConfigMode) {
            checkWiFiConnection();
            ArduinoOTA.handle();
        }

        if (wifiWasReconnected) {
            wifiWasReconnected = false;
            Serial.println("[System] WiFi reconectado — reinicializando mDNS e OTA...");
            setupMDNS();
            ArduinoOTA.end();
            setupOTA();
            Serial.println("[System] Serviços restaurados.");
        }
    }

    checkConfigButton();
    updateLEDStatus();

    delay(50);
}
