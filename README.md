# Controle de LED RGB pela UART - FRDM-KL25Z + Zephyr

Esse projeto é um receptor UART rodando na KL25Z com Zephyr. A placa recebe pacotes de 6 bytes pela serial e, dependendo do que chegar, liga, desliga, inverte ou faz piscar o LED RGB que já vem na própria Freedom. Para testar, tem um script em Python que manda os pacotes pelo computador.

## Como a comunicação acontece

A UART é uma comunicação serial: os bytes vão um bit de cada vez por um fio. O TX é o pino que envia e o RX é o pino que recebe. Como não tem fio de clock, os dois lados precisam combinar a velocidade antes. Aqui usamos 115200 bps no formato 8N1 (8 bits de dados, sem paridade e 1 stop bit).

Não precisei ligar nenhum fio, porque a Freedom tem dois chips: a KL25Z, que roda o nosso código, e um chip auxiliar chamado OpenSDA, que fica do lado da porta USB "SDA". Na própria placa, a UART0 da KL25Z (PTA1 é o RX e PTA2 é o TX) já está ligada nesse chip. Ele converte a UART para USB, e o computador enxerga a placa como uma porta COM (no meu caso, a COM5). Então, com o USB na porta SDA, o PC já está conversando com a KL25Z por UART.

    PC (Python) --USB--> OpenSDA --UART--> PTA1 (RX da KL25Z)
    PC          <--USB-- OpenSDA <--UART-- PTA2 (TX da KL25Z)   (mensagens do printk)

## O pacote (protocolo)

Todo pacote tem exatamente 6 bytes:

    ID | R | G | B | ACAO | CHECK

- ID: endereço da placa. A nossa é 0x01. Se chegar outro ID, a placa ignora o pacote.
- R, G, B: dizem quais cores acendem. Se o valor for maior que 0, aquela cor acende. Como não usei PWM, não dá para controlar a intensidade, só ligar e desligar cada cor.
- ACAO: o que a placa deve fazer.
  - 0x01 = liga
  - 0x02 = desliga
  - 0x03 = toggle (inverte: se está aceso apaga, se está apagado acende)
  - 0x04 = pisca a cada 1 s
  - 0x05 = pisca a cada 2 s
- CHECK: XOR dos 5 primeiros bytes. Serve para saber se o pacote chegou certo. Quem manda calcula o XOR e coloca no fim; a placa calcula de novo e compara. Se der diferente, algum byte veio errado e o pacote é jogado fora.

Exemplo: `01 FF 00 00 01 FF` liga o LED em vermelho (ID 1, R=255, G=0, B=0, ação liga, e 01^FF^00^00^01 = FF).

## Lógica do código da placa (src/main.c)

A ideia geral é: a interrupção da UART só guarda os bytes que chegam, e o main monta o pacote e decide o que fazer.

1. Interrupção da UART: toda vez que chega um byte, a função `uart_isr` é chamada e coloca esse byte numa fila (`k_msgq`). Ela não processa nada, porque uma interrupção tem que ser rápida. Se a fila encher, o byte é descartado em vez de travar.

2. Main montando o pacote: o loop principal tira os bytes da fila um por um e guarda num vetor de 6 posições.
   - Para o primeiro byte, ele espera o tempo que for preciso.
   - Do segundo em diante, ele espera no máximo 20 ms. Se passar disso, o pacote chegou incompleto (truncado). O código descarta o que tinha e volta a esperar um pacote novo do zero. É assim que a placa se ressincroniza sem travar.

3. Validação: quando juntam 6 bytes, a função `process_packet` confere, nessa ordem:
   - se o ID é 0x01;
   - se o CHECK bate com o XOR calculado;
   - se a ação é uma das 5 conhecidas.

   Se alguma coisa estiver errada, a placa imprime `[ERRO] ...` na serial, ignora o pacote e continua esperando o próximo.

4. Execução: se o pacote está certo, a placa executa a ação e imprime `[OK] ...`. Os LEDs são controlados por GPIO: vermelho no PTB18, verde no PTB19 e azul no PTD1. O LED da Freedom acende com nível baixo, mas o devicetree do Zephyr já cuida disso, então no código "1" quer dizer aceso.

5. Pisca: usei um `k_work_delayable`, que é uma tarefa que o Zephyr executa depois de um tempo. Ela inverte o LED e se agenda de novo para daqui a 1 s (ou 2 s). Quando chega outro comando, o pisca é cancelado. Assim o main não fica preso num `sleep` e continua recebendo pacotes enquanto o LED pisca.

## Lógica do script Python (test_uart.py)

O script faz o papel de "controle remoto" no PC:

1. Abre a porta serial (COM5) com a mesma configuração da placa: 115200, 8N1.
2. A função `make_packet` monta os 6 bytes: junta ID, R, G, B e ação e calcula o XOR para colocar no fim.
3. A função `send` manda o pacote, espera um pouquinho, lê o que a placa respondeu (as mensagens `[OK]`/`[ERRO]`) e mostra na tela.
4. Primeiro ele manda comandos válidos: liga vermelho, liga verde, toggle, pisca 1 s e pisca 2 s.
5. Depois manda pacotes errados de propósito: ID errado, CHECK errado, ação que não existe e um pacote cortado com só 3 bytes. No fim, manda um comando válido de novo para provar que a placa não travou.

## Como rodar

1. Ligue o USB na porta SDA da Freedom e dê Upload pelo PlatformIO.
2. Feche o Serial Monitor, porque só um programa consegue usar a porta de cada vez.
3. Instale a biblioteca serial (só na primeira vez):

       python -m pip install pyserial

4. Rode o script com a sua porta:

       python test_uart.py COM5

Obs.: não esquece do `zephyr/prj.conf`. Sem a linha `CONFIG_UART_INTERRUPT_DRIVEN=y`, a interrupção da UART não funciona e a placa não recebe nada. Eu caí nessa :)

    CONFIG_SERIAL=y
    CONFIG_UART_INTERRUPT_DRIVEN=y
    CONFIG_UART_USE_RUNTIME_CONFIGURE=y
    CONFIG_GPIO=y
    CONFIG_PRINTK=y
