/*
 / _____)             _              | |
( (____  _____ ____ _| |_ _____  ____| |__
 \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 _____) ) ____| | | || |_| ____( (___| | | |
(______/|_____)_|_|_| \__)_____)\____)_| |_|
  (C)2013 Semtech-Cycleo

Description:
    LoRa concentrator : Timer synchronization
        Provides synchronization between unix, concentrator and gps clocks

License: Revised BSD License, see LICENSE.TXT file include in the project
Maintainer: Michael Coracin
*/

/* -------------------------------------------------------------------------- */
/* --- DEPENDANCIES --------------------------------------------------------- */

#include <pthread.h>

#include "logger.h"
#include "gwcfg.h"
#include "timersync.h"
#include "loragw_hal.h"
#include "loragw_reg.h"
#include "loragw_aux.h"

/* -------------------------------------------------------------------------- */
/* --- PRIVATE CONSTANTS & TYPES -------------------------------------------- */

/* -------------------------------------------------------------------------- */
/* --- PRIVATE MACROS ------------------------------------------------------- */

#define timer_sub(a, b, result)                                                \
  do {                                                                        \
    (result)->tv_sec = (a)->tv_sec - (b)->tv_sec;                             \
    (result)->tv_usec = (a)->tv_usec - (b)->tv_usec;                          \
    if ((result)->tv_usec < 0) {                                              \
      --(result)->tv_sec;                                                     \
      (result)->tv_usec += 1000000;                                           \
    }                                                                         \
  } while (0)

/* -------------------------------------------------------------------------- */
/* --- PRIVATE VARIABLES (GLOBAL) ------------------------------------------- */

static pthread_mutex_t mx_timersync = PTHREAD_MUTEX_INITIALIZER; /* control access to timer sync offsets */
static struct timeval offset_unix_concent = {0,0}; /* timer offset between unix host and concentrator */

/* -------------------------------------------------------------------------- */
/* --- PRIVATE SHARED VARIABLES (GLOBAL) ------------------------------------ */
extern bool exit_sig;
extern bool quit_sig;

/* -------------------------------------------------------------------------- */
/* --- PUBLIC FUNCTIONS DEFINITION ------------------------------------------ */

DECLARE_GW;

int get_concentrator_time(uint32_t* concent_time) {
    struct timeval local_timeval;
    struct timeval unix_time;

    gettimeofday(&unix_time, NULL);
    //clock_gettime(CLOCK_MONOTONIC, &unix_time);

    if (concent_time == NULL) {
        lgw_log(LOG_ERROR, "ERROR: %s invalid parameter\n", __FUNCTION__);
        return -1;
    }

    pthread_mutex_lock(&mx_timersync); /* protect global variable access */
    timer_sub(&unix_time, &offset_unix_concent, &local_timeval);
    pthread_mutex_unlock(&mx_timersync);

    *concent_time = local_timeval.tv_sec * 1000000UL + local_timeval.tv_usec;
    //lgw_log(LOG_TIMERSYNC, "INFO~ [timesync] localtime: %u\n", *concent_time);

    lgw_log(LOG_TIMERSYNC, " --> TIME: unix current time is   %ld,%ld\n", unix_time.tv_sec, unix_time.tv_usec);
    lgw_log(LOG_TIMERSYNC, "           offset is              %ld,%ld\n", offset_unix_concent.tv_sec, offset_unix_concent.tv_usec);
    lgw_log(LOG_TIMERSYNC, "           sx130x current time is %ld,%ld\n", local_timeval.tv_sec, local_timeval.tv_usec);

    return 0;
}

/* ---------------------------------------------------------------------------------------------- */
/* --- THREAD 6: REGULARLAY MONITOR THE OFFSET BETWEEN UNIX CLOCK AND CONCENTRATOR CLOCK -------- */

void thread_timersync(void) {
    struct timeval unix_timeval;
    struct timeval concentrator_timeval;
    uint32_t sx130x_timecount = 0;
    struct timeval offset_previous = {0,0};
    struct timeval offset_drift = {0,0}; /* delta between current and previous offset */

    lgw_log(LOG_INFO, "\nINFO~ [TimerSync] Timesysnc thread start...\n");
    while (!exit_sig && !quit_sig) {
        /* Regularly disable GPS mode of concentrator's counter, in order to get
            real timer value for synchronizing with host's unix timer */
#ifdef SX1301MOD
        lgw_log(LOG_TIMERSYNC, "\nINFO~ [TimerSync] Disabling GPS mode for concentrator's counter...\n");
        pthread_mutex_lock(&GW.hal.mx_concent);
        lgw_reg_w(LGW_GPS_EN, 0);
        pthread_mutex_unlock(&GW.hal.mx_concent);
#endif

        /* Get current unix time */
        gettimeofday(&unix_timeval, NULL);
        //clock_gettime(CLOCK_MONOTONIC, &unix_timeval);

        /* Get current concentrator counter value (1MHz) */
        pthread_mutex_lock(&GW.hal.mx_concent);
        lgw_get_trigcnt(&sx130x_timecount);
        pthread_mutex_unlock(&GW.hal.mx_concent);

        concentrator_timeval.tv_sec = sx130x_timecount / 1000000UL;
        concentrator_timeval.tv_usec = sx130x_timecount - (concentrator_timeval.tv_sec * 1000000UL);

        /* Compute offset between unix and concentrator timers, with microsecond precision */
        offset_previous.tv_sec = offset_unix_concent.tv_sec;
        offset_previous.tv_usec = offset_unix_concent.tv_usec;

        /* TODO: handle sx130x coutner wrap-up */
        pthread_mutex_lock(&mx_timersync); /* protect global variable access */
        timer_sub(&unix_timeval, &concentrator_timeval, &offset_unix_concent);
        pthread_mutex_unlock(&mx_timersync);

        //printf("#DEBUG#DEBUG# offset=%ld,%ld\n", offset_unix_concent.tv_sec, offset_unix_concent.tv_usec);

        timer_sub(&offset_unix_concent, &offset_previous, &offset_drift);

        lgw_log(LOG_TIMERSYNC, "  sx130x = %u (µs) - timeval (%ld,%ld)\n",
                                          sx130x_timecount,
                                          concentrator_timeval.tv_sec,
                                          concentrator_timeval.tv_usec);
        lgw_log(LOG_TIMERSYNC, "  unix_timeval = %ld,%ld\n", unix_timeval.tv_sec, unix_timeval.tv_usec);

        lgw_log(LOG_TIMERSYNC, "INFO~ [TimerSync] host/sx130x time offset=(%lds:%ldµs) - drift=%ldµs\n",
                            offset_unix_concent.tv_sec,
                            offset_unix_concent.tv_usec,
                            offset_drift.tv_sec * 1000000UL + offset_drift.tv_usec);
#ifdef SX1301MOD
        lgw_log(LOG_TIMERSYNC, "INFO~ [TimerSync] Enabling GPS mode for concentrator's counter.\n\n");
        pthread_mutex_lock(&GW.hal.mx_concent); /* TODO: Is it necessary to protect here? */
        lgw_reg_w(LGW_GPS_EN, 1);
        pthread_mutex_unlock(&GW.hal.mx_concent);
#endif

        /* delay next sync */
        /* If we consider a crystal oscillator precision of about 20ppm worst case, and a clock
            running at 1MHz, this would mean 1µs drift every 50000µs (10000000/20).
            As here the time precision is not critical, we should be able to cope with at least 1ms drift,
            which should occur after 50s (50000µs * 1000).
            Let's set the thread sleep to 1 minute for now */
        wait_ms(60000);
    }
    lgw_log(LOG_INFO, "\nINFO~ [TimerSync] END of Timesysnc thread! \n");
}
