# 🌱 SmartGrow IoT — Sistema Inteligente de Cultivo Automatizado

> **Pós-Graduação em Internet das Coisas (IoT) — IFSP Câmpus Catanduva (2026)**  
> **Time 10:** Jane de Godoi Lahos & Vinícius Baldo Rocha

O **SmartGrow IoT** é uma plataforma de baixo custo (R$ 87,10) que integra sensoriamento embarcado, atuação automática por histerese com proteção *fail-safe*, comunicação criptografada **MQTT/TLS**, persistência em série temporal (**InfluxDB**), visualização em tempo real (**Grafana**) e **Inteligência Artificial (*Random Forest*)** para prever o estresse hídrico do cultivo com **2 horas de antecedência**.

---

## 🏗️ Arquitetura da Solução

```text
[Sensores: HW-103, DS18B20, DHT11]
               │
               ▼
   [ESP32-S3 N16R8 (Edge)] ──► Controle Local (Histerese + Fail-Safe) + Display OLED
               │
        MQTT sobre TLS (Porta 8883 / SmartGrow Root CA)
               │
               ▼
[Oracle Cloud Infrastructure (OCI)]
   ├── Broker Eclipse Mosquitto (TLS)
   ├── Node-RED (Ingestão e tratamento JSON)
   ├── InfluxDB (Banco de dados de séries temporais)
   ├── Modelo de IA (Random Forest Regressor - Horizonte +2h)
   └── Grafana (Dashboard operacional, alertas e curvas preditivas)
