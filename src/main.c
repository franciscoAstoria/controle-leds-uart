/*
 * Controle de LED RGB pela UART - FRDM-KL25Z + Zephyr (PlatformIO)
 *
 * Pacote fixo de 6 bytes:  ID | R | G | B | ACAO | CHECK
 *   ACAO: 0x01 LIGAR, 0x02 DESLIGAR, 0x03 TOGGLE, 0x04 PISCA 1s, 0x05 PISCA 2s
 *   CHECK: XOR dos 5 primeiros bytes  (ou CRC-8 se USE_CRC8 = 1 -> bonus)
 *
 * UART0 da KL25Z (PTA1 = RX, PTA2 = TX), que chega no PC pela USB "SDA"
 * (porta COM virtual do OpenSDA). 115200 bps, 8N1.
 *
 * LED RGB da placa (anodo comum, ativo em nivel baixo - o devicetree ja trata):
 *   vermelho = PTB18 (led2), verde = PTB19 (led0), azul = PTD1 (led1)
 *
 * Cada canal R/G/B fica aceso se o valor recebido for > 0 (sem PWM).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

/* ---------------- Configuracao ---------------- */
#define USE_CRC8        0      /* 0 = XOR (pedido no enunciado), 1 = CRC-8 (bonus) */
#define DEVICE_ID       0x01   /* ID deste receptor */
#define PKT_LEN         6
#define BYTE_TIMEOUT_MS 20     /* tempo maximo entre bytes de um mesmo pacote */

#define ACT_ON          0x01
#define ACT_OFF         0x02
#define ACT_TOGGLE      0x03
#define ACT_BLINK_1S    0x04
#define ACT_BLINK_2S    0x05

/* ---------------- Hardware ---------------- */
static const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

/* Ordem: R, G, B */
static const struct gpio_dt_spec leds[3] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),   /* vermelho PTB18 */
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),   /* verde    PTB19 */
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),   /* azul     PTD1  */
};

/* Fila de bytes recebidos (preenchida pela ISR da UART) */
K_MSGQ_DEFINE(rx_q, sizeof(uint8_t), 64, 1);

/* ---------------- Estado do LED ---------------- */
static uint8_t cur_rgb[3];            /* cor atual (0 = canal apagado) */
static bool    led_on;                /* LED aceso agora? */
static uint32_t blink_ms;             /* meio-periodo do pisca (0 = sem piscar) */

static struct k_work_delayable blink_work;

/* Contadores para diagnostico */
static uint32_t cnt_ok, cnt_bad_id, cnt_bad_check, cnt_bad_action, cnt_trunc, cnt_overflow;

/* ---------------- Checksum ---------------- */
static uint8_t calc_xor(const uint8_t *d, size_t n)
{
	uint8_t x = 0;

	for (size_t i = 0; i < n; i++) {
		x ^= d[i];
	}
	return x;
}

/* CRC-8, polinomio 0x07 (x^8 + x^2 + x + 1), valor inicial 0x00 */
static uint8_t calc_crc8(const uint8_t *d, size_t n)
{
	uint8_t crc = 0x00;

	for (size_t i = 0; i < n; i++) {
		crc ^= d[i];
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
		}
	}
	return crc;
}

static uint8_t calc_check(const uint8_t *d, size_t n)
{
#if USE_CRC8
	return calc_crc8(d, n);
#else
	return calc_xor(d, n);
#endif
}

/* ---------------- LED ---------------- */
static void led_apply(bool on)
{
	led_on = on;
	for (int i = 0; i < 3; i++) {
		gpio_pin_set_dt(&leds[i], on && cur_rgb[i] > 0);
	}
}

/* ---------------- Pisca ---------------- */
static void blink_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	led_apply(!led_on);
	if (blink_ms) {
		k_work_schedule(&blink_work, K_MSEC(blink_ms));
	}
}

static void blink_stop(void)
{
	struct k_work_sync sync;

	blink_ms = 0;
	k_work_cancel_delayable_sync(&blink_work, &sync);
}

static void blink_start(uint32_t half_period_ms)
{
	blink_stop();
	blink_ms = half_period_ms;
	led_apply(true);
	k_work_schedule(&blink_work, K_MSEC(blink_ms));
}

static void set_color(uint8_t r, uint8_t g, uint8_t b)
{
	cur_rgb[0] = r;
	cur_rgb[1] = g;
	cur_rgb[2] = b;
}

/* ---------------- Processamento do pacote ---------------- */
static void process_packet(const uint8_t *p)
{
	uint8_t id = p[0], r = p[1], g = p[2], b = p[3], act = p[4], chk = p[5];

	if (id != DEVICE_ID) {
		cnt_bad_id++;
		printk("[ERRO] ID invalido: 0x%02X (esperado 0x%02X)\n", id, DEVICE_ID);
		return;
	}

	uint8_t expected = calc_check(p, 5);

	if (chk != expected) {
		cnt_bad_check++;
		printk("[ERRO] CHECK invalido: recebido 0x%02X, calculado 0x%02X\n", chk, expected);
		return;
	}

	switch (act) {
	case ACT_ON:
		blink_stop();
		set_color(r, g, b);
		led_apply(true);
		printk("[OK] LIGAR  RGB=(%u,%u,%u)\n", r, g, b);
		break;
	case ACT_OFF:
		blink_stop();
		led_apply(false);
		printk("[OK] DESLIGAR\n");
		break;
	case ACT_TOGGLE:
		blink_stop();
		set_color(r, g, b);
		led_apply(!led_on);
		printk("[OK] TOGGLE -> %s\n", led_on ? "aceso" : "apagado");
		break;
	case ACT_BLINK_1S:
		set_color(r, g, b);
		blink_start(1000);  /* 1 s aceso, 1 s apagado */
		printk("[OK] PISCA 1s RGB=(%u,%u,%u)\n", r, g, b);
		break;
	case ACT_BLINK_2S:
		set_color(r, g, b);
		blink_start(2000);  /* 2 s aceso, 2 s apagado */
		printk("[OK] PISCA 2s RGB=(%u,%u,%u)\n", r, g, b);
		break;
	default:
		cnt_bad_action++;
		printk("[ERRO] ACAO desconhecida: 0x%02X\n", act);
		return;
	}
	cnt_ok++;
}

/* ---------------- UART (interrupcao) ---------------- */
static void uart_isr(const struct device *dev, void *user_data)
{
	uint8_t c;

	ARG_UNUSED(user_data);

	if (!uart_irq_update(dev)) {
		return;
	}
	if (!uart_irq_rx_ready(dev)) {
		return;
	}
	while (uart_fifo_read(dev, &c, 1) == 1) {
		/* Nunca bloqueia dentro da ISR: se a fila encher, descarta o byte */
		if (k_msgq_put(&rx_q, &c, K_NO_WAIT) != 0) {
			cnt_overflow++;
		}
	}
}

static int uart_setup(void)
{
	if (!device_is_ready(uart_dev)) {
		printk("UART nao esta pronta\n");
		return -ENODEV;
	}

	/* 115200 8N1 ja vem do devicetree da FRDM-KL25Z (current-speed = 115200).
	 * O driver LPSCI pode nao suportar reconfiguracao em tempo real (-ENOSYS),
	 * o que nao e problema. */
	const struct uart_config cfg = {
		.baudrate  = 115200,
		.parity    = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
		.data_bits = UART_CFG_DATA_BITS_8,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
	};
	(void)uart_configure(uart_dev, &cfg);

	int ret = uart_irq_callback_user_data_set(uart_dev, uart_isr, NULL);

	if (ret < 0) {
		printk("Erro ao registrar callback da UART: %d\n", ret);
		return ret;
	}
	uart_irq_rx_enable(uart_dev);
	return 0;
}

static int leds_setup(void)
{
	for (int i = 0; i < 3; i++) {
		if (!device_is_ready(leds[i].port)) {
			printk("GPIO do LED %d nao esta pronto\n", i);
			return -ENODEV;
		}
		gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
	}
	led_apply(false);
	return 0;
}

/* ---------------- main ---------------- */
int main(void)
{
	uint8_t pkt[PKT_LEN];
	size_t idx = 0;

	k_work_init_delayable(&blink_work, blink_handler);

	if (leds_setup() != 0 || uart_setup() != 0) {
		return 0;
	}

	printk("Receptor UART LED RGB (KL25Z) pronto. ID=0x%02X, check=%s\n",
	       DEVICE_ID, USE_CRC8 ? "CRC-8" : "XOR");

	while (1) {
		uint8_t c;
		/* Primeiro byte: espera indefinidamente.
		 * Bytes seguintes: timeout curto -> detecta pacote truncado. */
		k_timeout_t to = (idx == 0) ? K_FOREVER : K_MSEC(BYTE_TIMEOUT_MS);

		if (k_msgq_get(&rx_q, &c, to) != 0) {
			cnt_trunc++;
			printk("[ERRO] Pacote truncado (%u de %d bytes) - descartado\n",
			       (unsigned int)idx, PKT_LEN);
			idx = 0;   /* ressincroniza: proximo byte e inicio de pacote */
			continue;
		}

		pkt[idx++] = c;

		if (idx == PKT_LEN) {
			process_packet(pkt);
			idx = 0;
		}
	}
	return 0;
}
