# Memória de Alterações - ESP32 LoRa MQTT Broker

**Data:** 16/05/2026
**Versão do Firmware:** 3.0.3

---

## Problema Original

Após queda de energia, o ESP32 voltava ao WiFi Manager como se as credenciais não tivessem sido salvas. O usuário precisava reconfigurar tudo (WiFi, MQTT login/senha).

### Causa Raiz
1. Após queda de energia, o ESP32 bootava antes do roteador estar pronto
2. `autoConnect()` falhava na primeira tentativa e abria o portal de configuração com `setConfigPortalTimeout(0)` (infinito), ficando preso
3. O Watchdog Timer (30s) resetava o ESP32 enquanto o portal estava aberto, pois a `setup()` ficava bloqueada

---

## Alterações Realizadas

### 1. Correção do Fluxo de Reconexão WiFi (v2.x)
- **Problema:** `autoConnect()` falhava uma vez e abria o portal
- **Solução:** Loop de 3 tentativas de conexão manual com `WiFi.begin()` antes de abrir o portal
- Cada tentativa espera até 10s pela resposta do roteador
- Intervalo de 5s entre tentativas
- `WiFi.disconnect(false)` para manter credenciais salvas no NVS

### 2. Watchdog Timer Desabilitado Durante Portal
- **Problema:** WDT de 30s resetava o ESP32 enquanto `startConfigPortal()` estava bloqueado
- **Solução:** `esp_task_wdt_deinit()` chamado antes do `setupWiFiManager()`
- WDT reconfigurado após o portal fechar com `esp_task_wdt_reconfigure()`

### 3. Portal Sem Timeout
- `setConfigPortalTimeout(0)` — fica aberto indefinidamente até o usuário configurar
- Removido `setBreakAfterConfig(true)` que podia fechar o portal prematuramente

### 4. Eliminação do arquivo `credentials.h` (v3.0.0)
- **Objetivo:** Produção real — usuário define todas as credenciais via portal
- **Removido:** `#include "credentials.h"` e o arquivo `credentials.h`
- **Removido:** `.gitignore` entry para `credentials.h`

### 5. Campo de Senha OTA no WiFi Manager
- Adicionado terceiro campo no portal: `ota_password` — "Senha OTA"
- `saveOTACredentials()` / `loadOTACredentials()` — namespace NVS `ota-creds`
- `setupOTA()` usa senha do NVS; avisa se não configurada

### 6. Broker MQTT sem Fallback
- Broker agora usa **apenas** credenciais do NVS
- Se `mqttUser` ou `mqttPass` estiverem vazios, recusa conexão e loga mensagem
- Removido fallback para `MQTT_LOGIN` / `MQTT_PASSWORD` do `credentials.h`

### 7. Salvamento de Credenciais em 3 Pontos
- `autoConnect` sucesso (linha ~376)
- Portal sucesso via `startConfigPortal` (linha ~426)
- Botão GPIO0 pressionado por 3s (linha ~492)

Todos salvam MQTT login/senha E senha OTA no NVS.

---

## v3.0.3 — Correção Crítica: Reconexão WiFi e Restauração de Serviços (16/05/2026)

### Problema Relatado

O ESP32 **não se reconectava ao WiFi** quando perdia sinal ou quando o sinal ficava intermitente. Como consequência, o broker MQTT parava de funcionar e era impossível se conectar nele.

### Investigação — Análise Sistemática do Código

Foi feita uma análise completa de todos os arquivos do projeto, com foco no mecanismo de reconexão WiFi. A investigação seguiu o método de debugging sistemático:

#### Fase 1: Root Cause Investigation

**Problema 1 — `checkWiFiConnection()` ficava preso por 10 segundos sem fazer nada**

Na versão anterior (v3.0.2), a função verificava:
```cpp
if (WiFi.status() == WL_CONNECTED && !wifiNeedsReconnect) {
    return true;
}
```

Quando o WiFi desconectava e `wifiNeedsReconnect = true` era setado pelo evento `ARDUINO_EVENT_WIFI_STA_DISCONNECTED`, o código passava direto. Depois, travava aqui:
```cpp
if (currentMillis - lastReconnectAttempt < RECONNECT_INTERVAL) {
    return false;  // FICAVA PRESO AQUI POR 10 SEGUNDOS SEM FAZER NADA
}
```

Resultado: **10 segundos completos sem nenhuma tentativa de reconexão**.

**Problema 2 — `WiFi.reconnect()` falha silenciosamente em sinal intermitente**

A linha `WiFi.reconnect()` do ESP32 **não funciona corretamente** em todos os cenários de perda de conexão. Ela depende do estado interno do stack WiFi estar consistente. Se a conexão caiu de forma abrupta ou o sinal está oscilando, `WiFi.reconnect()` pode retornar `false` ou simplesmente não fazer nada — sem log, sem erro, sem efeito.

**Problema 3 — Broker MQTT e mDNS não eram reinicializados após reconexão**

Quando o WiFi finalmente reconectava, o broker MQTT (`sMQTTBroker`) **não era notificado** que a interface de rede mudou. O broker ficava "preso" no IP antigo ou em estado inconsistente. O mesmo acontecia com:
- mDNS (perdia os registros de serviço)
- OTA (perdia a associação com o novo IP)

Não havia nenhum código que reinicializasse esses serviços após a reconexão.

**Problema 4 — `ARDUINO_EVENT_WIFI_STA_DISCONNECTED` não disparava reconexão imediata**

O evento apenas setava `wifiNeedsReconnect = true`, mas não chamava `WiFi.setAutoReconnect(true)` nem iniciava o processo de reconexão imediatamente. O WiFiManager tinha `wm.setWiFiAutoReconnect(true)`, mas isso só funciona para reconexões automáticas do stack, não para o caso onde o estado fica inconsistente.

**Problema 5 — `wm.resetSettings()` destruía credenciais após apenas 10 tentativas**

Após 10 tentativas falhas de reconexão, o código chamava `wm.resetSettings()` — isso **apaga todas as credenciais WiFi salvas no NVS**, forçando o usuário a reconfigurar tudo via portal. Isso é destrutivo e desnecessário para um problema temporário de sinal.

#### O que acontecia na prática (fluxo do bug):

```
1. WiFi perde sinal → evento ARDUINO_EVENT_WIFI_STA_DISCONNECTED dispara
   → wifiNeedsReconnect = true

2. checkWiFiConnection() é chamado no loop
   → ESPERA 10 SEGUNDOS sem fazer nada (apenas return false)

3. Após 10s → chama WiFi.reconnect() UMA ÚNICA VEZ
   → Se WiFi.reconnect() falhar (comum em sinal intermitente)
   → Só tenta de novo após MAIS 10 segundos

4. Se reconectar → broker MQTT não sabe que rede voltou
   → continua inoperante, mDNS perdido, OTA perdido

5. Após 10 tentativas falhas → wm.resetSettings()
   → APAGA TODAS AS CREDENCIAIS WIFI → usuário precisa reconfigurar tudo
```

### Solução Implementada (v3.0.3)

#### Alteração 1 — Backoff Exponencial ao invés de Intervalo Fixo

**Antes:**
```cpp
const unsigned long RECONNECT_INTERVAL     = 10000;  // 10s fixos
const int           MAX_RECONNECT_ATTEMPTS = 10;
```

**Depois:**
```cpp
const unsigned long RECONNECT_BACKOFF_INITIAL  = 2000;   // 2s primeira tentativa
const unsigned long RECONNECT_BACKOFF_MAX      = 30000;  // 30s máximo
const int           MAX_RECONNECT_ATTEMPTS     = 20;     // 20 tentativas
```

O intervalo entre tentativas cresce exponencialmente: **2s → 4s → 8s → 16s → 30s (teto)**. Isso evita flood de tentativas quando o sinal está oscilando e dá tempo ao roteador para se estabilizar.

#### Alteração 2 — Reconexão Robusta (disconnect + begin ao invés de reconnect)

**Antes:**
```cpp
WiFi.reconnect();  // Falha silenciosamente em estado inconsistente
```

**Depois:**
```cpp
WiFi.disconnect(true);  // Limpa estado interno do stack WiFi
delay(500);
WiFi.mode(WIFI_STA);    // Reconfigura modo station
WiFi.begin();           // Reconecta do zero com credenciais salvas no NVS
```

O `WiFi.disconnect(true)` força a limpeza completa do estado interno do stack WiFi. O `WiFi.begin()` sem parâmetros usa as credenciais salvas automaticamente pelo WiFiManager no NVS.

#### Alteração 3 — Detecção de Reconexão Bem-Sucedida

Nova flag `wifiWasReconnected` que é setada quando o WiFi volta:
```cpp
if (WiFi.status() == WL_CONNECTED) {
    if (wifiNeedsReconnect) {
        wifiNeedsReconnect = false;
        reconnectAttempts = 0;
        reconnectBackoff = RECONNECT_BACKOFF_INITIAL;
        wifiWasReconnected = true;  // SINALIZA QUE PRECISA RESTAURAR SERVIÇOS
    }
    return true;
}
```

Isso garante que o sistema **sabe exatamente quando** o WiFi reconectou e pode tomar ações corretivas.

#### Alteração 4 — Restauração Automática de Serviços Após Reconexão

No `loop()`, após detectar `wifiWasReconnected`:
```cpp
if (wifiWasReconnected) {
    wifiWasReconnected = false;
    setupMDNS();              // Re-registra mDNS no novo IP
    ArduinoOTA.end();         // Encerra OTA antigo
    setupOTA();               // Reinicia OTA com novo IP
}
```

O broker MQTT (`sMQTTBroker`) **não precisa ser reinicializado** — ele já gerencia reconexões de clientes internamente através do `broker.update()` no loop. O que precisava ser restaurado eram os serviços de descoberta (mDNS) e atualização (OTA).

#### Alteração 5 — Removido `wm.resetSettings()` Destrutivo

**Antes:**
```cpp
if (reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
    wm.resetSettings();           // APAGA CREDENCIAIS WIFI!
    wm.autoConnect(...);          // Abre portal automaticamente
}
```

**Depois:**
```cpp
if (reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
    ESP.restart();  // Reinício limpo — credenciais NVS preservadas
}
```

Após 20 tentativas falhas (com backoff exponencial isso leva vários minutos), o sistema faz um reinício limpo. As credenciais WiFi e MQTT/OTA no NVS **permanecem intactas**. O ESP32 vai tentar reconectar normalmente no próximo boot.

### Impacto no WiFi Manager — CONFIRMAÇÃO

**Nenhuma alteração foi feita no WiFi Manager.** Todas as correções estão isoladas na função `checkWiFiConnection()` e no `loop()`. O WiFi Manager continua funcionando exatamente como antes:

- **Portal de configuração:** abre normalmente quando não há credenciais salvas
- **Campos MQTT login/senha e OTA password:** funcionam normalmente
- **Salvamento no NVS:** credenciais são salvas e carregadas corretamente
- **Botão GPIO0:** continua abrindo o portal quando pressionado por 3s
- **Fluxo de configuração:** selecionar rede → salvar → conectar → funciona perfeitamente

A única "alteração" relacionada ao WiFi Manager foi **remover** o `wm.resetSettings()` automático — o que na verdade **melhora** a experiência, pois antes ele apagava suas credenciais salvas sem necessidade após falhas temporárias de WiFi.

---

## Estrutura de Armazenamento NVS

| Namespace | Chave | Tipo | Descrição |
|-----------|-------|------|-----------|
| `mqtt-creds` | `mqtt_login` | String | Login do broker MQTT |
| `mqtt-creds` | `mqtt_password` | String | Senha do broker MQTT |
| `ota-creds` | `ota_password` | String | Senha para OTA |
| *(WiFiManager)* | *(interno)* | *(interno)* | Credenciais WiFi (SSID/senha) |

---

## Fluxo Atual do Sistema

### Boot Normal (com credenciais salvas)
```
Boot → loadMQTTCredentials() → loadOTACredentials()
→ 3 tentativas de WiFi.begin() (10s cada)
→ Conectado → setupMDNS() → broker.init() → setupOTA()
→ Tasks criadas (MQTT Core 1, WiFi+OTA Core 0)
```

### Boot Sem Credenciais (ESP32 virgem ou NVS limpo)
```
Boot → loadMQTTCredentials() → vazias
→ 3 tentativas de WiFi.begin() → falham
→ startConfigPortal() → ABERTO INDEFINIDAMENTE
→ Usuário configura WiFi + MQTT + OTA → Save
→ Conecta WiFi → salva credenciais no NVS → continua boot
```

### Após Queda de Energia (roteador demorando para subir)
```
Boot → 3 tentativas de WiFi.begin() (cada uma espera 10s)
→ Se roteador subir em alguma tentativa → conecta normalmente
→ Se todas falharem → abre portal (mas isso é raro se o roteador já estava configurado)
```

### Reconfiguração via Botão GPIO0
```
Segurar GPIO0 por 3s → startConfigPortal() → usuário configura → Save
→ Conecta WiFi → salva credenciais atualizadas no NVS
```

### Falha Crítica de WiFi (20 reconexões falhadas) — NOVO COMPORTAMENTO v3.0.3
```
checkWiFiConnection() → 20 tentativas falhadas (com backoff exponencial: 2s→4s→8s→16s→30s)
→ ESP.restart() — reinício limpo
→ NOTA: Credenciais WiFi, MQTT e OTA NO NVS SÃO PRESERVADAS
→ No próximo boot: tenta reconectar normalmente com credenciais salvas
```

### Reconexão WiFi Bem-Sucedida — NOVO COMPORTAMENTO v3.0.3
```
WiFi perde sinal → ARDUINO_EVENT_WIFI_STA_DISCONNECTED → wifiNeedsReconnect = true
→ checkWiFiConnection() detecta desconexão
→ WiFi.disconnect(true) → delay(500) → WiFi.mode(WIFI_STA) → WiFi.begin()
→ Backoff exponencial entre tentativas (2s, 4s, 8s, 16s, 30s max)
→ WiFi reconecta → GOT_IP → wifiWasReconnected = true
→ loop() detecta wifiWasReconnected → setupMDNS() + ArduinoOTA.end() → setupOTA()
→ Broker MQTT continua operando (broker.update() gerencia clientes)
→ Sistema 100% operacional novamente
```

---

## Configurações do WiFiManager

```cpp
wm.setConfigPortalTimeout(0);      // Sem timeout
wm.setConnectTimeout(60);          // 60s para conectar ao AP
wm.setMinimumSignalQuality(0);     // Aceita qualquer sinal
wm.setWiFiAutoReconnect(true);     // Reconexão automática
wm.setDebugOutput(false);          // Debug desabilitado
```

---

## Constantes de Controle

### Versão Anterior (v3.0.2)
```cpp
RECONNECT_INTERVAL     = 10000   // 10s fixos entre tentativas de reconexão
MAX_RECONNECT_ATTEMPTS = 10      // Máx tentativas antes de reconfigurar
WIFI_CONNECT_TIMEOUT   = 15000   // Timeout no setup
```

### Versão Atual (v3.0.3)
```cpp
RECONNECT_BACKOFF_INITIAL  = 2000    // 2s — primeira tentativa
RECONNECT_BACKOFF_MAX      = 30000   // 30s — teto do backoff exponencial
MAX_RECONNECT_ATTEMPTS     = 20      // Máx tentativas antes de restart
WIFI_CONNECT_TIMEOUT       = 15000   // Timeout no setup
CONFIG_PORTAL_TIMEOUT      = 300000  // Referência (5 min)
```

Sequência de backoff: **2s → 4s → 8s → 16s → 30s → 30s → 30s...** (teto em 30s)

---

## Tasks FreeRTOS

| Task | Core | Stack | Prioridade | Função |
|------|------|-------|------------|--------|
| `taskBroker` | 1 | 8192 | 1 | `broker.update()` + WDT reset |
| `taskWiFiOTA` | 0 | 8192 | 2 | WiFi reconexão + OTA handle + LED + botão |
| `taskConfig` | 0 | 4096 | 1 | LED + botão (sem WiFi) |

---

## Arquivos do Projeto

| Arquivo | Descrição |
|---------|-----------|
| `ESP32-LoRa-Modo-Station-Mode-MQTT.ino` | Código principal (686 linhas — v3.0.3) |
| `.gitignore` | Build artifacts e IDE |
| `credentials.h` | **REMOVIDO** (v3.0.0) |
| `memoria.md` | Este arquivo — histórico de alterações |

---

## Notas Importantes

1. **NVS persiste entre flashes** — credenciais sobrevivem a uploads de firmware via OTA/USB
2. **`wm.resetSettings()`** foi **REMOVIDO** do fluxo de reconexão — não apaga mais credenciais automaticamente
3. **GPIO0** precisa de `INPUT_PULLUP` — botão conecta ao GND quando pressionado
4. **LED_BUILTIN = GPIO2** — HIGH = ligado, LOW = desligado
5. **Portal sem timeout** — só fecha com Save ou reinício físico do ESP32
6. **`CONFIG_PORTAL_TIMEOUT`** mantida como referência — pode ser removida quando estabilizado
7. **Backoff exponencial** — evita flood de reconexões em sinal intermitente; dá tempo ao roteador
8. **Reconexão robusta** — `WiFi.disconnect(true)` + `WiFi.begin()` limpa estado interno e reconecta do zero
9. **Restauração automática** — mDNS e OTA são re-registrados automaticamente após reconexão WiFi
10. **Broker MQTT auto-gerenciável** — `broker.update()` no loop gerencia reconexões de clientes sem intervenção
