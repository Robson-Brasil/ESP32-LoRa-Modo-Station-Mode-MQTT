# Findings - Análise do Código ESP32 MQTT Broker

## Arquivos Analisados
- `ESP32-LoRa-Modo-Station-Mode-MQTT.ino` (271 linhas)
- `credentials.h` (29 linhas)

---

## ✅ Pontos Fortes

1. **Mutex para proteção multi-core**: O uso de `xSemaphoreCreateMutex()` protege o acesso ao WiFi entre as tasks dos dois cores
2. **Tratamento de reconexão WiFi**: Bom algoritmo com tentativas limitadas e reinício automático após falha crítica
3. **Separação de tasks por core**: Broker MQTT no Core 1, WiFi/OTA no Core 0 - boa distribuição de carga
4. **Feedback visual via LED**: Indicador visual durante OTA
5. **Versionamento de firmware**: Constante FIRMWARE_VERSION para controle de OTA
6. **Configuração de IP estático**: Bem implementada com DNS primário e secundário

---

## ⚠️ Problemas Identificados

### 🔴 CRÍTICO - Segurança

#### 1. Credenciais Hardcoded
- **Arquivo**: `credentials.h`
- **Problema**: Senhas WiFi, MQTT e OTA estão expostas no código
- **Risco**: Exposição de credenciais se o repositório for público
- **Recomendação**: Usar variáveis de ambiente ou servidor de configuração remoto

#### 2. Senhas Iguais MQTT e OTA
- **Arquivo**: `credentials.h` linhas 21 e 27
- **Problema**: `MQTT_PASSWORD` = `OTA_PASSWORD` = "S3nh@S3gur@"
- **Risco**: Se alguém descobrir a senha do OTA, automaticamente tem acesso ao MQTT
- **Recomendação**: Usar senhas diferentes para cada serviço

---

### 🟡 MÉDIO - Código/Arquitetura

#### 3. Nome do Projeto não corresponde à funcionalidade
- **Problema**: Projeto se chama "ESP32-LoRa-Modo-Station-Mode-MQTT" mas não há código LoRa
- **Observação**: O código é puramente um Broker MQTT, sem comunicação LoRa
- **Recomendação**: Renomear para "ESP32-MQTT-Broker" ou similar

#### 4. Stack das Tasks
- **Problema**: `taskBroker` com 4096 bytes pode ser insuficiente
- **Recomendação**: Aumentar para 8192 bytes como `taskWiFiOTA`

#### 5. Ausência de função ledOn()
- **Problema**: Apenas `ledOff()` existe, sem função complementar
- **Recomendação**: Adicionar função `ledOn()` para consistência

---

### 🟢 BAIXO - Melhorias Opcionais

#### 6. Serial.print sem proteção
- Observação: `Serial.begin()` está no início do setup, então é aceitável

#### 7.LED somente usado durante OTA
- Observação: Poderia dar feedback em outras situações (WiFi conectado, cliente MQTT conectado)

---

## Métricas do Código
- **Linguagem**: C++ (Arduino framework)
- **Bibliotecas**: sMQTTBroker, WiFi, ArduinoOTA
- **Configuração de rede**: IP estático (192.168.15.150)
- **Porta MQTT**: 1883 (padrão)
- **Porta OTA**: 3232
- **Versão atual**: 2.0.1