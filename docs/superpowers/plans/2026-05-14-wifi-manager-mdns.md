# WiFi Manager + mDNS Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Adicionar WiFi Manager e mDNS ao projeto ESP32 MQTT Broker, permitindo configuração WiFi via portal web e acesso via hostname `roteadoresp32.local`

**Architecture:** Substituir configuração WiFi estática por WiFi Manager com portal de configuração, adicionar mDNS para resolução de hostname, implementar LED status para feedback visual.

**Tech Stack:** ESP32, WiFiManager (tzapu), mdns (nativo ESP32)

---

## Estrutura de Arquivos

- Modify: `ESP32-LoRa-Modo-Station-Mode-MQTT.ino` — arquivo principal com todas as modificações

---

### Task 1: Adicionar Includes e Variáveis do WiFi Manager + mDNS

**Files:**
- Modify: `ESP32-LoRa-Modo-Station-Mode-MQTT.ino:1-55`

- [ ] **Step 1: Adicionar includes necessários**

Adicionar após as includes existentes:
```cpp
#include <WiFiManager.h>  // Biblioteca WiFi Manager
#include <mdns.h>          // Biblioteca mDNS nativa do ESP32
```

- [ ] **Step 2: Adicionar variáveis globais**

Adicionar após as variáveis de controle existentes (após linha 54):
```cpp
// ==========================================
// WiFi Manager
// ==========================================
WiFiManager wm;
bool shouldSaveConfig = false;
bool wifiConfigMode = false;
unsigned long configPortalStart = 0;
const unsigned long CONFIG_PORTAL_TIMEOUT = 300000; // 5 minutos

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
```

- [ ] **Step 3: Commit**

```
git add ESP32-LoRa-Modo-Station-Mode-MQTT.ino
git commit -m "feat: add WiFi Manager and mDNS includes and variables"
```

---

### Task 2: Remover IP Estático e Criar Funções de Configuração

**Files:**
- Modify: `ESP32-LoRa-Modo-Station-Mode-MQTT.ino:160-195`

- [ ] **Step 1: Remover configuração de IP estático**

Remover linhas 33-38 (configuração de IP estático) - serão substituídas pelo WiFi Manager.

- [ ] **Step 2: Criar funções de callback do WiFi Manager**

Adicionar após funções de LED (após linha 99):
```cpp
// ==========================================
// WiFi Manager Callbacks
// ==========================================
void configModeCallback(WiFiManager *wm) {
    wifiConfigMode = true;
    configPortalStart = millis();
    currentLEDState = LED_FAST_BLINK;
    Serial.println("[WiFiManager] Modo de configuração ativado!");
    Serial.println("[WiFiManager] Acesse: http://192.168.4.1");
}

void saveConfigCallback() {
    shouldSaveConfig = true;
    Serial.println("[WiFiManager] Configuração salva!");
}

// ==========================================
// mDNS Setup
// ==========================================
void setupMDNS() {
    if (MDNS.begin("roteadoresp32")) {
        Serial.println("[mDNS] Servidor MDNS iniciado");
        Serial.println("[mDNS] Acesse: http://roteadoresp32.local");
        MDNS.addService("http", "tcp", 3232); // Adiciona serviço HTTP para OTA
    } else {
        Serial.println("[mDNS] ERRO ao iniciar MDNS!");
    }
}
```

- [ ] **Step 3: Criar função de setup do WiFi Manager**

Adicionar após as funções WiFi existentes (antes de setup()):
```cpp
// ==========================================
// Setup WiFi Manager
// ==========================================
bool setupWiFiManager() {
    wm.setDebugOutput(false);
    wm.setAPCallback(configModeCallback);
    wm.setSaveConfigCallback(saveConfigCallback);

    wm.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT);
    wm.setConnectTimeout(30);

    WiFi.mode(WIFI_STA);

    Serial.println("[WiFiManager] Tentando conexão automática...");

    bool connected = wm.autoConnect("ESP32-MQTT-Broker", "12345678");

    if (connected) {
        Serial.printf("[WiFiManager] Conectado! IP: %s\n", WiFi.localIP().toString().c_str());
        currentLEDState = LED_ON;
        wifiConfigMode = false;
        return true;
    } else {
        Serial.println("[WiFiManager] Falha na conexão automática");
        currentLEDState = LED_SLOW_BLINK;
        return false;
    }
}

// ==========================================
// Verificar botão GPIO 0 para ativar portal
// ==========================================
void checkConfigButton() {
    if (digitalRead(0) == LOW) {
        unsigned long pressStart = millis();
        delay(100);
        while (digitalRead(0) == LOW) {
            delay(100);
            if (millis() - pressStart > 3000) {
                Serial.println("[WiFiManager] Botão pressionado por 3s - ativando portal...");
                currentLEDState = LED_FAST_BLINK;
                wifiConfigMode = true;
                wm.startConfigPortal("ESP32-MQTT-Broker", "12345678");
                WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
                currentLEDState = LED_ON;
                wifiConfigMode = false;
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
```

- [ ] **Step 4: Commit**

```
git add ESP32-LoRa-Modo-Station-Mode-MQTT.ino
git commit -m "feat: add WiFi Manager and mDNS setup functions"
```

---

### Task 3: Modificar Setup Principal

**Files:**
- Modify: `ESP32-LoRa-Modo-Station-Mode-MQTT.ino:235-306`

- [ ] **Step 1: Modificar setup() para usar WiFi Manager**

Substituir a seção de configuração WiFi (linhas 260-273) com:
```cpp
    pinMode(0, INPUT_PULLUP);  // GPIO 0 (Botão Boot)

    // --- Configuração WiFi via WiFi Manager ---
    if (setupWiFiManager()) {
        setupMDNS();

        const unsigned short mqttPort = 1883;
        broker.init(mqttPort);
        setupOTA();

        // Task Broker MQTT — Core 1
        xTaskCreatePinnedToCore(
            [](void* param) {
                esp_task_wdt_add(NULL);
                for (;;) {
                    broker.update();
                    esp_task_wdt_reset();
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }, "taskBroker", 8192, nullptr, 1, nullptr, 1
        );

        // Task WiFi + OTA + LED — Core 0
        xTaskCreatePinnedToCore(
            [](void* param) {
                esp_task_wdt_add(NULL);
                for (;;) {
                    checkWiFiConnection();
                    ArduinoOTA.handle();
                    updateLEDStatus();
                    esp_task_wdt_reset();
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
            }, "taskWiFiOTA", 8192, nullptr, 2, nullptr, 0
        );

        Serial.println("[System] Tasks prontas!\n");
    } else {
        Serial.println("[WiFiManager] Sem WiFi - iniciando modo de espera...");

        currentLEDState = LED_SLOW_BLINK;

        // Task only for button + LED (no MQTT without WiFi)
        xTaskCreatePinnedToCore(
            [](void* param) {
                esp_task_wdt_add(NULL);
                for (;;) {
                    checkConfigButton();
                    updateLEDStatus();
                    esp_task_wdt_reset();
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
            }, "taskConfig", 4096, nullptr, 1, nullptr, 0
        );
    }

    vTaskDelete(NULL);
}
```

- [ ] **Step 2: Remover função applyStaticIP()**

A função `applyStaticIP()` (linhas 163-169) não é mais necessária — remover.

- [ ] **Step 3: Commit**

```
git add ESP32-LoRa-Modo-Station-Mode-MQTT.ino
git commit -m "feat: integrate WiFi Manager in main setup"
```

---

### Task 4: Modificar Função checkWiFiConnection

**Files:**
- Modify: `ESP32-LoRa-Modo-Station-Mode-MQTT.ino:194-230`

- [ ] **Step 1: Modificar para usar WiFi reconexão automática**

Atualizar a função para usar comportamento adequado com WiFi Manager (sem IP estático):
```cpp
bool checkWiFiConnection()
{
    esp_task_wdt_reset();

    if (WiFi.status() == WL_CONNECTED && !wifiNeedsReconnect) {
        if (currentLEDState != LED_ON) {
            currentLEDState = LED_ON;
        }
        return true;
    }

    unsigned long currentMillis = millis();
    if (currentMillis - lastReconnectAttempt < RECONNECT_INTERVAL) {
        if (currentLEDState != LED_MEDIUM_BLINK) {
            currentLEDState = LED_MEDIUM_BLINK;
        }
        return false;
    }

    if (xSemaphoreTake(wifiMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }

    Serial.printf("[WiFi] Status atual: %d. Tentativa de reconexão %d/%d...\n",
                  WiFi.status(), reconnectAttempts + 1, MAX_RECONNECT_ATTEMPTS);

    lastReconnectAttempt = currentMillis;
    reconnectAttempts++;

    if (reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
        Serial.println("\n[WiFi] Falha crítica de conexão. Tentando reconfigurar...");
        currentLEDState = LED_SLOW_BLINK;
        wifiConfigMode = false;
        reconnectAttempts = 0;
        WiFi.disconnect();
        delay(1000);
        wm.resetSettings();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    } else {
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }

    xSemaphoreGive(wifiMutex);
    return false;
}
```

- [ ] **Step 2: Remover chamada a applyStaticIP()**

Na task WiFi+OTA (linha 224 na versão original), remover a linha `applyStaticIP();`

- [ ] **Step 3: Commit**

```
git add ESP32-LoRa-Modo-Station-Mode-MQTT.ino
git commit -m "feat: update WiFi reconnection logic for WiFi Manager"
```

---

### Task 5: Testar Compilação

- [ ] **Step 1: Compilar projeto**

Compilar via PlatformIO: `pio run` ou Arduino IDE

- [ ] **Step 2: Verificar erros**

Se houver erros de compilação, corrigir.

- [ ] **Step 3: Commit**

```
git add ESP32-LoRa-Modo-Station-Mode-MQTT.ino
git commit -m "fix: compilation fixes if needed"
```

---

### Task 6: Revisão Final

- [ ] **Step 1: Verificar todas as funcionalidades implementadas**

- WiFi Manager conecta automaticamente
- Botão GPIO 0 ativa portal de configuração após 3 segundos
- mDNS responde em `roteadoresp32.local`
- LED mostra status correto
- MQTT Broker só inicia após WiFi conectado
- OTA continua funcionando

- [ ] **Step 2: Commit final**

```
git add ESP32-LoRa-Modo-Station-Mode-MQTT.ino
git commit -m "feat: complete WiFi Manager and mDNS integration"
```

---

## Notas

1. **Biblioteca WiFiManager:** Precisa ser instalada via Library Manager ou PlatformIO (lib_deps = tzapu/WiFiManager)
2. **Senha do portal:** Definida como "12345678" - alterar se desejado
3. **Tempo do portal:** 5 minutos de timeout para auto-fechamento
4. **Porta MQTT:** Mantida em 1883
5. **Porta OTA:** Mantida em 3232