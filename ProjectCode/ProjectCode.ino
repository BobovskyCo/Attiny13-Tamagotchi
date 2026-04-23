#include <util/delay.h>
#include <avr/sleep.h>
#include <avr/wdt.h>
#include <avr/pgmspace.h>
#include <avr/interrupt.h>

#define SDA_PIN  PB0
#define SCL_PIN  PB1
#define BTN_PIN  PB4
#define OLED_ADDR 0x3C

#define TIME_HUNGRY      ((uint16_t)3600)
#define TIME_GROW        ((uint16_t)54000)
#define SCREEN_ON_SECS   ((uint8_t)10)

uint8_t  pet_hunger;
uint8_t  pet_age;
uint16_t ticks;
uint16_t last_fed_tick;
uint16_t grow_tick;

volatile uint8_t wdt_fired = 0;
volatile uint8_t btn_fired = 0;

// Спрайты аобитово
const uint8_t SPR_COFFIN[8]       PROGMEM = {0b10000000, 0b11111100, 0b10000010, 0b10001010, 0b10001010, 0b10000010, 0b11111100, 0b10000000};
const uint8_t SPR_CHILD_IDLE[8]   PROGMEM = {0b00111100, 0b01000010, 0b10000101, 0b10010001, 0b10010001, 0b10000101, 0b01000010, 0b00111100};
const uint8_t SPR_CHILD_BREATH[8] PROGMEM = {0b00111000, 0b01000100, 0b10001010, 0b10100010, 0b10100010, 0b10001010, 0b01000100, 0b00111000};
const uint8_t SPR_CHILD_EAT[8]    PROGMEM = {0b00111100, 0b01000010, 0b10000101, 0b10110001, 0b10110001, 0b10000101, 0b01000010, 0b00111100};
const uint8_t SPR_CHILD_HUNGR[8]  PROGMEM = {0b00111000, 0b01000100, 0b10010010, 0b10100010, 0b10001010, 0b01000100, 0b00111000, 0b00000000};
const uint8_t SPR_ADULT_IDLE[8]   PROGMEM = {0b00000000, 0b00001000, 0b11111111, 0b01111001, 0b01111001, 0b11111111, 0b00001000, 0b00000000};
const uint8_t SPR_ADULT_BREATH[8] PROGMEM = {0b00000000, 0b00010000, 0b11111110, 0b01110010, 0b01110010, 0b11111110, 0b00010000, 0b00000000};
const uint8_t SPR_ADULT_EAT[8]    PROGMEM = {0b00000000, 0b00001000, 0b11111111, 0b01111111, 0b01111111, 0b11111111, 0b00001000, 0b00000000};
const uint8_t SPR_ADULT_HUNGR[8]  PROGMEM = {0b00000000, 0b10100000, 0b11111100, 0b11100100, 0b11100100, 0b11111100, 0b10100000, 0b00000000};

const uint8_t* const sprite_table[8] PROGMEM = {
  SPR_CHILD_IDLE, SPR_CHILD_BREATH, SPR_CHILD_EAT, SPR_CHILD_HUNGR,
  SPR_ADULT_IDLE, SPR_ADULT_BREATH, SPR_ADULT_EAT, SPR_ADULT_HUNGR
};

#define I2C_DELAY() __asm__("nop\nnop\nnop\nnop")
void i2c_sda_low()  { DDRB |=  (1 << SDA_PIN); }
void i2c_sda_high() { DDRB &= ~(1 << SDA_PIN); }

void i2c_start() {
  i2c_sda_high(); PORTB |= (1<<SCL_PIN); I2C_DELAY();
  i2c_sda_low();  I2C_DELAY();
  PORTB &= ~(1<<SCL_PIN);
}
void i2c_stop() {
  i2c_sda_low(); PORTB |= (1<<SCL_PIN); I2C_DELAY();
  i2c_sda_high(); I2C_DELAY();
}
void i2c_write(uint8_t b) {
  for (uint8_t i = 0; i < 8; i++) {
    if (b & 0x80) i2c_sda_high(); else i2c_sda_low();
    PORTB |=  (1<<SCL_PIN); I2C_DELAY();
    PORTB &= ~(1<<SCL_PIN); I2C_DELAY();
    b <<= 1;
  }
  i2c_sda_high();
  PORTB |=  (1<<SCL_PIN); I2C_DELAY();
  PORTB &= ~(1<<SCL_PIN);
}

void oled_cmd(uint8_t cmd) {
  i2c_start();
  i2c_write(OLED_ADDR << 1);
  i2c_write(0x00);
  i2c_write(cmd);
  i2c_stop();
}

static const uint8_t oled_init_seq[] PROGMEM = {
  0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
  0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
  0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF
};

void oled_init() {
  for (uint8_t i = 0; i < sizeof(oled_init_seq); i++)
    oled_cmd(pgm_read_byte(&oled_init_seq[i]));
}
void oled_off() { oled_cmd(0xAE); oled_cmd(0x8D); oled_cmd(0x10); }

void draw_sprite(const uint8_t* spr) {
  for (uint8_t sy = 0; sy < 8; sy++) {
    oled_cmd(0x22); oled_cmd(sy); oled_cmd(sy);
    oled_cmd(0x21); oled_cmd(32); oled_cmd(95);
    i2c_start();
    i2c_write(OLED_ADDR << 1);
    i2c_write(0x40);
    for (uint8_t col = 0; col < 8; col++) {
      uint8_t fill = (pgm_read_byte(&spr[col]) >> sy) & 1 ? 0xFF : 0x00;
      for (uint8_t sx = 0; sx < 8; sx++) i2c_write(fill);
    }
    i2c_stop();
  }
}

// читаем состояние
uint8_t read_button() {
  if (PINB & (1 << BTN_PIN)) return 0;
  _delay_ms(10);                         // дебаунс
  if (PINB & (1 << BTN_PIN)) return 0;
  return 1;                              
}

void pet_update() {
  if (pet_age == 2) return;
  uint16_t since_fed = ticks - last_fed_tick;
  if      (since_fed >= TIME_HUNGRY + TIME_HUNGRY) pet_age = 2;
  else if (since_fed >= TIME_HUNGRY)     pet_hunger = 1;
  else                                   pet_hunger = 0;
  if ((ticks - grow_tick) >= TIME_GROW) {
    if (pet_age == 0) pet_age++;
  }
}

void pet_feed() {
  if (pet_age == 2) return;
  pet_hunger = 0;
  last_fed_tick = ticks;
}

ISR(WDT_vect)   { wdt_fired = 1; }
ISR(PCINT0_vect){ btn_fired  = 1; }

void wdt_setup() {
  cli(); wdt_reset();
  WDTCR = (1 << WDCE) | (1 << WDE);
  WDTCR = (1 << 6) | (1 << WDP3) | (1 << WDP0);
  sei();
}

void go_sleep() {
  GIMSK |= (1 << PCIE);
  PCMSK |= (1 << BTN_PIN);
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep_enable(); sei(); sleep_cpu(); sleep_disable();
  PCMSK &= ~(1 << BTN_PIN);
}

const uint8_t* get_sprite(uint8_t state) {
  uint8_t age = pet_age < 2 ? pet_age : 1; // max age для индекса = 1
  return (const uint8_t*)pgm_read_ptr(&sprite_table[age * 4 + state]);
}

void screen_awake() {
  uint8_t frame = 0;
  uint8_t eating = 0;
  uint8_t ticks_on = 0;

  for (;;) {
    const uint8_t* spr;
    if      (pet_age == 2) spr = SPR_COFFIN;
    else if (eating)       spr = get_sprite(frame & 1 ? 2 : 0);
    else if (pet_hunger)   spr = get_sprite(3);
    else                   spr = get_sprite(frame & 1);

    draw_sprite(spr);

    // вместо _delay_ms(50) — 50 итераций по 1мс с проверкой кнопки
    for (uint8_t d = 0; d < 50; d++) {
      _delay_ms(10);
      if (!(PINB & (1 << BTN_PIN))) {
        // ждём отпускания
        while (!(PINB & (1 << BTN_PIN)));
        if (pet_age < 2) { pet_feed(); eating = 6; frame = 0; }
        ticks_on = 0;
      }
    }

    if (++ticks_on >= SCREEN_ON_SECS + SCREEN_ON_SECS) break;
    if (eating) eating--;
    if (++frame >= 10) frame = 0;
  }
}

void setup() {
  DDRB  &= ~(1 << BTN_PIN);
  PORTB |=  (1 << BTN_PIN);
  PORTB &= ~((1 << SDA_PIN) | (1 << SCL_PIN));
  DDRB  |=  (1 << SCL_PIN);
  pet_hunger = 0; pet_age = 0;
  ticks = 0; last_fed_tick = 0; grow_tick = 0;
  wdt_setup();
  oled_init();
  screen_awake();
  oled_off();
}

void loop() {
  wdt_fired = 0; btn_fired = 0;
  go_sleep();
  if (wdt_fired) { ticks++; wdt_fired = 0; pet_update(); }
  if (btn_fired) {
    btn_fired = 0;
    _delay_ms(50);
    if (!(PINB & (1 << BTN_PIN))) {
      // ждём отпускания кнопки перед показом экрана
      while (!(PINB & (1 << BTN_PIN)));
      oled_init();
      screen_awake();
      oled_off();
    }
  }
}
