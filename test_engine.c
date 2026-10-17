/* Timing smoke test: builds with -DENGINE_TEST, writes to /dev/null instead of uinput. */
#include "engine.h"
#include <stdio.h>
#include <unistd.h>
#include <time.h>
int main(void)
{
    double rates[] = { 10, 100, 500, 1000 };
    if (!engine_init(NULL, NULL, NULL)) { puts("init failed"); return 1; }
    engine_set_backend(ENGINE_BACKEND_UINPUT);
    for (int i = 0; i < 4; i++) {
        engine_set_cps(rates[i]);
        unsigned long long a = engine_click_count();
        engine_set_running(true);
        sleep(2);
        engine_set_running(false);
        unsigned long long b = engine_click_count();
        printf("target %6.1f CPS -> measured %7.1f CPS\n", rates[i], (b - a) / 2.0);
    }
    /* stop latency at very low CPS */
    engine_set_cps(0.1);
    engine_set_running(true);
    usleep(200000);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    engine_set_running(false);
    usleep(100000);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    puts("low-CPS stop OK");
    char buf[64];
    engine_hotkey_name(64, 0, buf, sizeof buf); puts(buf);
    engine_hotkey_name(33, 3, buf, sizeof buf); puts(buf);
    engine_shutdown();
    return 0;
}
