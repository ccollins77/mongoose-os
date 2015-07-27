#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Driverlib includes */
#include "hw_types.h"

#include "hw_ints.h"
#include "hw_memmap.h"
#include "gpio.h"
#include "interrupt.h"
#include "pin.h"
#include "prcm.h"
#include "rom_map.h"
#include "timer.h"
#include "uart.h"
#include "utils.h"

#include "sj_prompt.h"

#include "v7.h"
#include "config.h"

#define LED_GPIO 11

struct v7 *v7;
static const char *v7_version = "TODO";

static v7_val_t js_usleep(struct v7 *v7, v7_val_t this_obj, v7_val_t args) {
  v7_val_t usecsv = v7_array_get(v7, args, 0);
  int usecs;
  if (!v7_is_number(usecsv)) {
    printf("usecs is not a double\n\r");
    return v7_create_undefined();
  }
  usecs = v7_to_number(usecsv);
  usleep(usecs);
  return v7_create_undefined();
}

void init_v7(void *stack_base) {
  struct v7_create_opts opts;
//  v7_val_t wifi, dht11, gc, debug, os;

  opts.object_arena_size = 164;
  opts.function_arena_size = 26;
  opts.property_arena_size = 400;
  opts.c_stack_base = stack_base;

  v7 = v7_create_opt(opts);

  v7_set(v7, v7_get_global_object(v7), "version", 7, 0,
         v7_create_string(v7, v7_version, strlen(v7_version), 1));
  v7_set_method(v7, v7_get_global_object(v7), "usleep", js_usleep);
#if 0
  v7_set_method(v7, v7_get_global_object(v7), "dsleep", dsleep);
  v7_set_method(v7, v7_get_global_object(v7), "setTimeout", set_timeout);

  v7_set_method(v7, v7_get_global_object(v7), "crash", crash);

  wifi = v7_create_object(v7);
  v7_set(v7, v7_get_global_object(v7), "Wifi", 4, 0, wifi);
  v7_set_method(v7, wifi, "mode", Wifi_mode);
  v7_set_method(v7, wifi, "setup", Wifi_setup);
  v7_set_method(v7, wifi, "disconnect", Wifi_disconnect);
  v7_set_method(v7, wifi, "connect", Wifi_connect);
  v7_set_method(v7, wifi, "status", Wifi_status);
  v7_set_method(v7, wifi, "ip", Wifi_ip);
  v7_set_method(v7, wifi, "scan", Wifi_scan);
  v7_set_method(v7, wifi, "changed", Wifi_changed);
  v7_set_method(v7, wifi, "show", Wifi_show);

#if V7_ESP_ENABLE__DHT11
  dht11 = v7_create_object(v7);
  v7_set(v7, v7_get_global_object(v7), "DHT11", 5, 0, dht11);
  v7_set_method(v7, dht11, "read", DHT11_read);
#endif /* V7_ESP_ENABLE__DHT11 */

  gc = v7_create_object(v7);
  v7_set(v7, v7_get_global_object(v7), "GC", 2, 0, gc);
  v7_set_method(v7, gc, "stat", GC_stat);
  v7_set_method(v7, gc, "gc", GC_gc);

  debug = v7_create_object(v7);
  v7_set(v7, v7_get_global_object(v7), "Debug", 5, 0, debug);
  v7_set_method(v7, debug, "mode", Debug_mode);
  v7_set_method(v7, debug, "print", Debug_print);

  v7_init_http_client(v7);

  os = v7_create_object(v7);
  v7_set(v7, v7_get_global_object(v7), "OS", 2, 0, os);
  v7_set_method(v7, os, "prof", OS_prof);
  v7_set_method(v7, os, "wdt_feed", OS_wdt_feed);
  v7_set_method(v7, os, "reset", OS_reset);

  init_i2cjs(v7);
  init_gpiojs(v7);
  init_hspijs(v7);
#endif
  v7_gc(v7, 1);

//  init_conf(v7);
}

void blinkenlights() {
  int n = 0;
  unsigned char lm = 1 << (LED_GPIO % 8);
  unsigned char v = 0;
  while (1) {
    v ^= lm;
    MAP_GPIOPinWrite(GPIOA1_BASE, lm, v);
    usleep(1000000);
/*
    char *p = malloc(1024);
    n += 1024;
    printf("-- %d %p\n", n, p);
    malloc_stats();
*/
  }
}

void uart_int() {
  char c = UARTCharGet(CONSOLE_UART);
  sj_prompt_process_char(c);
  MAP_UARTIntClear(CONSOLE_UART, UART_INT_RX);
}

void timer_int() {
  unsigned long int_status = MAP_TimerIntStatus(TIMERA0_BASE, true);
  MAP_TimerIntClear(TIMERA0_BASE, int_status);
}

/* Int vector table, defined in startup_gcc.c */
extern void (* const g_pfnVectors[256])(void);

int main() {
  int dummy;

  MAP_IntVTableBaseSet((unsigned long)&g_pfnVectors[0]);
  MAP_IntMasterEnable();
  MAP_IntEnable(FAULT_SYSTICK);
  PRCMCC3200MCUInit();

  /* Console UART init. */
  MAP_PRCMPeripheralClkEnable(CONSOLE_UART_PERIPH, PRCM_RUN_MODE_CLK);
  MAP_PinTypeUART(PIN_55, PIN_MODE_3);  /* PIN_55 -> UART0_TX */
  MAP_PinTypeUART(PIN_57, PIN_MODE_3);  /* PIN_57 -> UART0_RX */
  MAP_UARTConfigSetExpClk(
      CONSOLE_UART, MAP_PRCMPeripheralClockGet(CONSOLE_UART_PERIPH),
      CONSOLE_BAUD_RATE,
      (UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE));
  MAP_UARTFIFODisable(CONSOLE_UART);
  MAP_UARTIntRegister(CONSOLE_UART, uart_int);
  MAP_UARTIntEnable(CONSOLE_UART, UART_INT_RX);

  /* GPIO */
  MAP_PRCMPeripheralClkEnable(PRCM_GPIOA1, PRCM_RUN_MODE_CLK);
  MAP_PinTypeGPIO(PIN_02, PIN_MODE_0, false);  /* Green LED */
  MAP_GPIODirModeSet(GPIOA1_BASE, 0x8, GPIO_DIR_MODE_OUT);
  MAP_GPIOPinWrite(GPIOA1_BASE, 1 << (LED_GPIO % 8), 0);

#if 0
  /* Timer init. */
  MAP_PRCMPeripheralClkEnable(PRCM_TIMERA0, PRCM_RUN_MODE_CLK);
  MAP_PRCMPeripheralReset(PRCM_TIMERA0);
  MAP_TimerConfigure(TIMERA0_BASE, TIMER_CFG_PERIODIC);
  MAP_TimerLoadSet(TIMERA0_BASE, TIMER_A, 16000000);
  MAP_TimerIntRegister(TIMERA0_BASE, TIMER_A, timer_int);
  MAP_IntPriorityGroupingSet(3);
  MAP_IntPrioritySet(INT_TIMERA0A, 0xFF);
  MAP_TimerIntEnable(TIMERA0_BASE, TIMER_TIMA_TIMEOUT);
  MAP_TimerEnable(TIMERA0_BASE, TIMER_A);
#endif

  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  printf("\n\nSmart.JS for CC3200\n");

  init_v7(&dummy);

  sj_prompt_init(v7);

  blinkenlights();

  return 0;
}
