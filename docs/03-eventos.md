# Eventos e Automações

## Eventos gerados no ESP32

| Tipo publicado em `.../evento` | Gatilho | Ação |
|---|---|---|
| `BOOT` | Conexão ao broker | Registra a entrada em operação |
| `ARMANDO` | Senha ou comando ARMAR | Inicia delay de saída de 15 s |
| `ARMADO` | Fim do delay de saída | Ativa a vigilância |
| `ENTRADA_DETECTADA` | PIR ou porta com sistema armado | Inicia delay de entrada de 10 s + alerta |
| `ALARME` | Delay de entrada esgotado ou pânico | Liga relé, buzzer bitonal, LED vermelho |
| `DESARMADO` | Senha correta | Desliga tudo e zera tentativas |
| `SENHA_INCORRETA` | Senha errada no teclado ou no dashboard | Bipe de erro e incremento do contador |
| `BLOQUEIO` | 3 tentativas incorretas | Trava o teclado por 30 s + alerta |
| `DESBLOQUEIO` | Fim dos 30 s | Libera o teclado |
| `SIRENE_SILENCIADA` | Comando SILENCIAR | Corta a sirene, mantém o alarme |
| `SIRENE_ENCERRADA` | 60 s de sirene | Corta a sirene automaticamente |

## Automações no Node-RED

**Watchdog de comunicação.** Verifica a cada 3 s se houve heartbeat nos últimos 10 s. Na transição de online para offline, dispara uma notificação. É o que atende ao requisito de "status da comunicação".

**Escalonamento de alarme.** Quando o estado muda para `ALARME`, um nó `trigger` aguarda 30 s. Se ao fim desse tempo o estado ainda for `ALARME`, o fluxo gera um alerta de escalonamento — em produção esse ponto seria o disparo de e-mail, Telegram ou chamada para a central de monitoramento.

**Histórico rolante.** Alertas e eventos alimentam uma lista mantida em `flow context` com as 12 ocorrências mais recentes, exibida no dashboard. Ela sobrevive ao refresh da página, ao contrário de um log preso ao navegador.

**Tradução de estado para nível numérico.** O estado textual é convertido em uma escala de 0 a 4 para alimentar o gráfico de nível de alerta, permitindo visualizar a evolução do risco ao longo do tempo.
