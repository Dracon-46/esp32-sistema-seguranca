# Arquitetura do Sistema

## Visão geral em camadas

| Camada | Elemento | Responsabilidade |
|---|---|---|
| Percepção | PIR, reed switch, teclado 4x4 | Capturar eventos físicos do ambiente |
| Controle | ESP32 (máquina de estados) | Decidir localmente quando disparar o alarme |
| Atuação | Relé, buzzer, LED RGB, LCD | Reagir fisicamente e informar quem está no local |
| Comunicação | Wi-Fi + MQTT (HiveMQ) | Transportar estado, telemetria e comandos |
| Processamento | Node-RED | Aplicar regras, manter histórico e watchdog |
| Apresentação | Node-RED Dashboard | Exibir e permitir comandar remotamente |

## Por que a lógica fica no ESP32

A decisão de projeto mais importante foi manter **toda a lógica crítica dentro do microcontrolador**. Se o alarme dependesse do Node-RED para decidir disparar, uma queda de internet desligaria a proteção — exatamente o cenário que um invasor exploraria. O ESP32 arma, detecta, conta os delays e aciona a sirene sozinho. O MQTT é usado apenas para telemetria, alertas e comandos opcionais.

## Máquina de estados

```
                    senha / botao ARMAR
   DESARMADO ─────────────────────────────> SAINDO
       ^                                       │ 15 s
       │                                       v
       │  senha correta                     ARMADO
       ├───────────────────────────────────────┤
       │                                       │ PIR ou porta
       │                                       v
       │            10 s sem senha         ENTRANDO
       │  <──────────── senha ─────────────────┤
       │                                       v
       └──────────── senha ────────────────  ALARME

   3 senhas erradas ──> BLOQUEADO ──30 s──> ARMADO
```

## Loop não bloqueante

O `loop()` não usa `delay()`. Todas as temporizações (delay de saída, delay de entrada, duração da sirene, bloqueio do teclado, piscadas do LED e bipes) são calculadas por comparação de `millis()`. Isso garante que o `mqtt.loop()` seja chamado com frequência suficiente para não perder comandos nem derrubar a conexão por timeout de keepalive.

## Confiabilidade da comunicação

O sistema usa duas camadas para detectar queda de comunicação:

1. **Last Will and Testament (LWT)** — ao conectar, o ESP32 registra no broker a mensagem `offline` retida no tópico `.../conexao`. Se o dispositivo cair sem se despedir, o próprio broker publica esse aviso.
2. **Watchdog no Node-RED** — um `inject` de 3 em 3 segundos verifica se chegou algum heartbeat ou telemetria nos últimos 10 segundos. Se não chegou, o dashboard marca OFFLINE mesmo que o LWT ainda não tenha sido disparado.

A redundância cobre o caso em que a conexão TCP fica pendurada sem fechar corretamente.
