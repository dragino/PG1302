/*
 *  ____  ____      _    ____ ___ _   _  ___  
 *  |  _ \|  _ \    / \  / ___|_ _| \ | |/ _ \ 
 *  | | | | |_) |  / _ \| |  _ | ||  \| | | | |
 *  | |_| |  _ <  / ___ \ |_| || || |\  | |_| |
 *  |____/|_| \_\/_/   \_\____|___|_| \_|\___/ 
 *
 * Dragino_gw_fwd -- An opensource lora gateway forward 
 *
 * See http://www.dragino.com for more information about
 * the lora gateway project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 *
 * Maintainer: skerlan
 *
 */

/*!
 * \file
 * \brief lora packages dispatch 
 */

/* -------------------------------------------------------------------------- */
/* --- DEPENDANCIES --------------------------------------------------------- */

/* fix an issue between POSIX and C99 */
#if __STDC_VERSION__ >= 199901L
    #define _XOPEN_SOURCE 600
#else
    #define _XOPEN_SOURCE 500
#endif

#include <stdbool.h>			/* bool type */
#include <string.h>				/* memset, strerror */
#include <inttypes.h>           /* PRIx64, PRIu64... */
#include <strings.h>				
#include <signal.h>				/* sigaction */
#include <time.h>				/* time, clock_gettime, strftime, gmtime */
#include <unistd.h>				/* getopt, access */
#include <stdlib.h>				/* atoi, exit */
#include <errno.h>				/* error messages */
#include <math.h>				/* modf */
#include <assert.h>

#include <sys/socket.h>			/* socket specific definitions */
#include <netinet/in.h>			/* INET constants and stuff */
#include <arpa/inet.h>			/* IP address conversion stuff */
#include <netdb.h>				/* gai_strerror */

#include <getopt.h>
#include <limits.h>
#include <semaphore.h>

#include "fwd.h"
#include "parson.h"
#include "base64.h"
#include "ghost.h"
#include "service.h"
#include "stats.h"
#include "timersync.h"

#include "loragw_gps.h"
#include "loragw_aux.h"
#include "loragw_hal.h"

/* signal handling variables */
volatile bool exit_sig = false;	/* 1 -> application terminates cleanly (shut down hardware, close open files, etc) */
volatile bool quit_sig = false;	/* 1 -> application terminates without shutting down the hardware */

/* -------------------------------------------------------------------------- */
/* --- privite DECLARATION ---------------------------------------- */

/* -------------------------------------------------------------------------- */
/* --- PUBLIC DECLARATION ---------------------------------------- */
uint8_t LOG_INFO = 1;
uint8_t LOG_PKT = 1;
uint8_t LOG_WARNING = 1;
uint8_t LOG_ERROR = 1;
uint8_t LOG_REPORT = 1;
uint8_t LOG_JIT = 0;
uint8_t LOG_JIT_ERROR = 0;
uint8_t LOG_BEACON = 0;
uint8_t LOG_DEBUG = 0;
uint8_t LOG_TIMERSYNC = 0;
uint8_t LOG_MEM = 0;

/* -------------------------------------------------------------------------- */
/* --- PUBLIC DECLARATION ---------------------------------------- */

// initialize GW
INIT_GW;

/* -------------------------------------------------------------------------- */
/* --- PRIVATE FUNCTIONS DECLARATION ---------------------------------------- */
void stop_clean_service(void);

struct lgw_atexit {
    void (*func)(void);
    int is_cleanup;
    LGW_LIST_ENTRY(lgw_atexit) list;
};

static LGW_LIST_HEAD_STATIC(atexits, lgw_atexit);

static void lgw_run_atexits(int run_cleanups)
{
    struct lgw_atexit *ae;

    LGW_LIST_LOCK(&atexits);
    while ((ae = LGW_LIST_REMOVE_HEAD(&atexits, list))) {
        if (ae->func && (!ae->is_cleanup || run_cleanups)) {
            ae->func();
        }
        lgw_free(ae);
    }
    LGW_LIST_UNLOCK(&atexits);
}

static void __lgw_unregister_atexit(void (*func)(void))
{
    struct lgw_atexit *ae;

    LGW_LIST_TRAVERSE_SAFE_BEGIN(&atexits, ae, list) {
        if (ae->func == func) {
            LGW_LIST_REMOVE_CURRENT(list);
            lgw_free(ae);
            break;
        }
    }
    LGW_LIST_TRAVERSE_SAFE_END;
}

static int register_atexit(void (*func)(void), int is_cleanup)
{
    struct lgw_atexit *ae;

    ae = lgw_calloc(1, sizeof(*ae));
    if (!ae) {
        return -1;
    }
    ae->func = func;
    ae->is_cleanup = is_cleanup;
    LGW_LIST_LOCK(&atexits);
    __lgw_unregister_atexit(func);
    LGW_LIST_INSERT_HEAD(&atexits, ae, list);
    LGW_LIST_UNLOCK(&atexits);

    return 0;
}

int lgw_register_atexit(void (*func)(void))
{
    return register_atexit(func, 0);
}

int lgw_register_cleanup(void (*func)(void))
{
    return register_atexit(func, 1);
}

void lgw_unregister_atexit(void (*func)(void))
{
    LGW_LIST_LOCK(&atexits);
    __lgw_unregister_atexit(func);
    LGW_LIST_UNLOCK(&atexits);
}

static void sig_handler(int sigio);

/* threads */
static void thread_up(void);
static void thread_gps(void);
static void thread_valid(void);
static void thread_jit(void);
static void thread_watchdog(void);
static void thread_rxpkt_recycle(void);

#ifdef SX1302MOD
static void thread_spectral_scan(void);
#endif

/* -------------------------------------------------------------------------- */
/* --- PRIVATE FUNCTIONS DEFINITION ----------------------------------------- */

static void usage( void )
{
    printf("~~~ Library version string~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
    printf(" %s\n", lgw_version_info());
    printf("~~~ Available options ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
    printf(" -h  print this help\n");
    printf(" -c <filename>  use config file other than 'gwcfg.json'\n");
    printf(" -d radio module [sx1301, sx1302, sx1308]'\n");
    printf("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
}

static void sig_handler(int sigio) {
	if (sigio == SIGQUIT) {
		quit_sig = true;
		exit_sig = true;
	} else if ((sigio == SIGINT) || (sigio == SIGTERM)) {
		exit_sig = true;
	}
	return;
}

void stop_clean_service(void) {
    serv_s* serv_entry = NULL;  

    service_stop();

    LGW_LIST_TRAVERSE_SAFE_BEGIN(&GW.serv_list, serv_entry, list) {
        LGW_LIST_REMOVE_CURRENT(list);
        GW.serv_list.size--;
        if (NULL != serv_entry->net) {
            if (NULL != serv_entry->net->mqtt)
                lgw_free(serv_entry->net->mqtt);
            lgw_free(serv_entry->net);
        }
        if (NULL != serv_entry->report)
            lgw_free(serv_entry->report);
        if (NULL != serv_entry) {
            sem_destroy(&serv_entry->thread.sema);
            lgw_free(serv_entry);
        }
    }
    LGW_LIST_TRAVERSE_SAFE_END;
}

double difftimespec(struct timespec end, struct timespec beginning) {
    double x;

    x = 1E-9 * (double)(end.tv_nsec - beginning.tv_nsec);
    x += (double)(end.tv_sec - beginning.tv_sec);
    return x;
}

int get_tx_gain_lut_index(uint8_t rf_chain, int8_t rf_power, uint8_t * lut_index) {
    uint8_t pow_index;
    int current_best_index = -1;
    uint8_t current_best_match = 0xFF;
    int diff;

    /* Check input parameters */
    if (lut_index == NULL) {
        lgw_log(LOG_ERROR, "ERROR~ %s - wrong parameter\n", __FUNCTION__);
        return -1;
    }

    /* Search requested power in TX gain LUT */
    for (pow_index = 0; pow_index < GW.tx.txlut[rf_chain].size; pow_index++) {
        diff = rf_power - GW.tx.txlut[rf_chain].lut[pow_index].rf_power;
        if (diff < 0) {
            /* The selected power must be lower or equal to requested one */
            continue;
        } else {
            /* Record the index corresponding to the closest rf_power available in LUT */
            if ((current_best_index == -1) || (diff < current_best_match)) {
                current_best_match = diff;
                current_best_index = pow_index;
            }
        }
    }

    /* Return corresponding index */
    if (current_best_index > -1) {
        *lut_index = (uint8_t)current_best_index;
    } else {
        *lut_index = 0;
        lgw_log(LOG_ERROR, "ERROR~ %s - failed to find tx gain lut index\n", __FUNCTION__);
        return -1;
    }

    return 0;
}

int send_tx_ack(serv_s* serv, uint8_t token_h, uint8_t token_l, enum jit_error_e error, int32_t error_value) {
    uint8_t buff_ack[ACK_BUFF_SIZE]; /* buffer to give feedback to server */
    int buff_index;
    int j;

    /* reset buffer */
    memset(&buff_ack, 0, sizeof buff_ack);

    /* Prepare downlink feedback to be sent to server */
    buff_ack[0] = PROTOCOL_VERSION;
    buff_ack[1] = token_h;
    buff_ack[2] = token_l;
    buff_ack[3] = PKT_TX_ACK;
    *(uint32_t *)(buff_ack + 4) = GW.info.net_mac_h;
    *(uint32_t *)(buff_ack + 8) = GW.info.net_mac_l;
    buff_index = 12; /* 12-byte header */

    /* Put no JSON string if there is nothing to report */
    if (error != JIT_ERROR_OK) {
        /* start of JSON structure */
        memcpy((void *)(buff_ack + buff_index), (void *)"{\"txpk_ack\":{", 13);
        buff_index += 13;
        /* set downlink error/warning status in JSON structure */
        switch( error ) {
            case JIT_ERROR_TX_POWER:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"warn\":", 7);
                buff_index += 7;
                break;
            default:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"error\":", 8);
                buff_index += 8;
                break;
        }
        /* set error/warning type in JSON structure */
        switch (error) {
            case JIT_ERROR_FULL:
            case JIT_ERROR_COLLISION_PACKET:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"COLLISION_PACKET\"", 18);
                buff_index += 18;
                /* update stats */
                pthread_mutex_lock(&serv->report->mx_report);
                serv->report->stat_down.meas_nb_tx_rejected_collision_packet += 1;
                pthread_mutex_unlock(&serv->report->mx_report);
                break;
            case JIT_ERROR_TOO_LATE:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"TOO_LATE\"", 10);
                buff_index += 10;
                /* update stats */
                pthread_mutex_lock(&serv->report->mx_report);
                serv->report->stat_down.meas_nb_tx_rejected_too_late += 1;
                pthread_mutex_unlock(&serv->report->mx_report);
                break;
            case JIT_ERROR_TOO_EARLY:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"TOO_EARLY\"", 11);
                buff_index += 11;
                /* update stats */
                pthread_mutex_lock(&serv->report->mx_report);
                serv->report->stat_down.meas_nb_tx_rejected_too_early += 1;
                pthread_mutex_unlock(&serv->report->mx_report);
                break;
            case JIT_ERROR_COLLISION_BEACON:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"COLLISION_BEACON\"", 18);
                buff_index += 18;
                /* update stats */
                pthread_mutex_lock(&serv->report->mx_report);
                serv->report->stat_down.meas_nb_tx_rejected_collision_beacon += 1;
                pthread_mutex_unlock(&serv->report->mx_report);
                break;
            case JIT_ERROR_TX_FREQ:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"TX_FREQ\"", 9);
                buff_index += 9;
                break;
            case JIT_ERROR_TX_POWER:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"TX_POWER\"", 10);
                buff_index += 10;
                break;
            case JIT_ERROR_GPS_UNLOCKED:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"GPS_UNLOCKED\"", 14);
                buff_index += 14;
                break;
            default:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"UNKNOWN\"", 9);
                buff_index += 9;
                break;
        }
        /* set error/warning details in JSON structure */
        switch (error) {
            case JIT_ERROR_TX_POWER:
                j = snprintf((char *)(buff_ack + buff_index), ACK_BUFF_SIZE-buff_index, ",\"value\":%d", error_value);
                if (j > 0) {
                    buff_index += j;
                } else {
                    lgw_log(LOG_ERROR, "ERROR~ [%s-up] snprintf failed line %u\n", serv->info.name, (__LINE__ - 4));
                    break;
                }
                break;
            default:
                /* Do nothing */
                break;
        }
        /* end of JSON structure */
        memcpy((void *)(buff_ack + buff_index), (void *)"}}", 2);
        buff_index += 2;
    }

    buff_ack[buff_index] = 0; /* add string terminator, for safety */

    /* send datagram to server */
    return send(serv->net->sock_up, (void *)buff_ack, buff_index, 0);
}

void print_tx_status(uint8_t tx_status) {
	switch (tx_status) {
	case TX_OFF:
		lgw_log(LOG_INFO, "INFO~ [jit] lgw_status returned TX_OFF\n");
		break;
	case TX_FREE:
		lgw_log(LOG_INFO, "INFO~ [jit] lgw_status returned TX_FREE\n");
		break;
	case TX_EMITTING:
		lgw_log(LOG_INFO, "INFO~ [jit] lgw_status returned TX_EMITTING\n");
		break;
	case TX_SCHEDULED:
		lgw_log(LOG_INFO, "INFO~ [jit] lgw_status returned TX_SCHEDULED\n");
		break;
	default:
		lgw_log(LOG_INFO, "INFO~ [jit] lgw_status returned UNKNOWN (%d)\n", tx_status);
		break;
	}
}

int get_rxpkt(serv_s* serv) {
    int ret = 0;
    rxpkts_s* rxpkt_entry;

    //LGW_LIST_LOCK(&GW.rxpkts_list);
    LGW_LIST_TRAVERSE(&GW.rxpkts_list, rxpkt_entry, list) {
        ret = 0;
        if (NULL == rxpkt_entry)
            continue;
        if (serv->info.stamp == (serv->info.stamp & rxpkt_entry->stamps))
            continue;
        
        memset(serv->rxpkt, 0, sizeof(serv->rxpkt));
        memcpy(serv->rxpkt, rxpkt_entry->rxpkt, sizeof(struct lgw_pkt_rx_s) * rxpkt_entry->nb_pkt);
        ret = rxpkt_entry->nb_pkt;
        pthread_mutex_lock(&GW.mx_bind_lock);
        rxpkt_entry->stamps |= serv->info.stamp;
        rxpkt_entry->bind--;
        pthread_mutex_unlock(&GW.mx_bind_lock);
        break;
    }
    //LGW_LIST_UNLOCK(&GW.rxpkts_list);
    return ret;
}

/* -------------------------------------------------------------------------- */
/* --- MAIN FUNCTION -------------------------------------------------------- */

int main(int argc, char *argv[]) {
    int i;						/* loop variable and temporary variable for return value */
    struct sigaction sigact;	/* SIGQUIT&SIGINT&SIGTERM signal handling */

    serv_s* serv_entry = NULL;  

	/* threads */
	pthread_t thrid_up;
	pthread_t thrid_gps;
	pthread_t thrid_valid;
	pthread_t thrid_jit;
	pthread_t thrid_ss;
	pthread_t thrid_timersync;
	pthread_t thrid_watchdog;

    /* Parse command line options */
    while( (i = getopt( argc, argv, "hc:" )) != -1 )
    {
        switch( i )
        {
        case 'h':
            usage( );
            return EXIT_SUCCESS;
            break;

        case 'c':
            if (NULL != optarg) 
                strncpy(GW.hal.confs.gwcfg, optarg, sizeof(GW.hal.confs.gwcfg));
            break;

        default:
            break;
        }
    }

#ifdef SX1301MOD
    strcpy(GW.hal.board, "sx1301");
#else
    strcpy(GW.hal.board, "sx1302");
#endif

	/* display version informations */
	lgw_log(LOG_INFO, "*** Dragino Packet Forwarder for Lora Gateway ***\n");
	lgw_log(LOG_INFO, "*** LoRa concentrator HAL library version info %s ***\n", lgw_version_info());
    lgw_log(LOG_INFO, "*** LoRa radio type of board is: %s ***\n", GW.hal.board);

	/* configure signal handling */
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigact.sa_handler = sig_handler;
	sigaction(SIGQUIT, &sigact, NULL);	/* Ctrl-\ */
	sigaction(SIGINT, &sigact, NULL);	/* Ctrl-C */
	sigaction(SIGTERM, &sigact, NULL);	/* default "kill" command */
	sigaction(SIGQUIT, &sigact, NULL);	/* Ctrl-\ */

    /* staring database for temporary data */
    if (lgw_db_init()) {
        lgw_log(LOG_ERROR, "[main] ERROR~ Can't initiate sqlite3 database, EXIT!\n");
        exit(EXIT_FAILURE);
    }

    /* 创建一个默认的service，用来基本包处理：包解析、包解码、包保存、自定义下发等 */
    serv_entry = (serv_s*)lgw_malloc(sizeof(serv_s));
    if (NULL == serv_entry) {
        lgw_log(LOG_ERROR, "[main] ERROR~ Can't allocate pkt service, EXIT!\n");
        exit(EXIT_FAILURE);
    }
    serv_entry->list.next = NULL;

    serv_entry->net = NULL;
    serv_entry->report = NULL;

    serv_entry->state.live = false;

    serv_entry->info.type = pkt;
    serv_entry->info.stamp = 1;     //将PKT服务标记为 1 
    strcpy(serv_entry->info.name, "PKT_SERV");
    //serv_entry->rxpkts_list->first = NULL;
    //serv_entry->rxpkts_list->last = NULL;
    //serv_entry->rxpkts_list->size = 0;
    //pthread_mutex_init(&serv_entry->rxpkts_list->lock, NULL);

    if (sem_init(&serv_entry->thread.sema, 0, 0) != 0) {
        lgw_log(LOG_ERROR, "[main] ERROR~ initializes the unnamed semaphore, EXIT!\n");
        lgw_free(serv_entry);
        exit(EXIT_FAILURE);
    }

    serv_entry->thread.stop_sig = false;

    LGW_LIST_INSERT_TAIL(&GW.serv_list, serv_entry, list);

    if (access(GW.hal.confs.gwcfg, R_OK) == 0 && access(GW.hal.confs.sxcfg, R_OK) == 0) { /* if there is a global conf, parse it  */
        if (parsecfg()) {
            lgw_log(LOG_ERROR, "ERROR~ [main] failed to parse configuration file\n");
            exit(EXIT_FAILURE);
        }
    } else {
        lgw_log(LOG_ERROR, "ERROR~ [main] failed to find any configuration file: %s %s\n", GW.hal.confs.gwcfg, GW.hal.confs.sxcfg);
        exit(EXIT_FAILURE);
    }

	/* Start GPS a.s.a.p., to allow it to lock */
    if (GW.gps.gps_tty_path[0] != '\0') { /* do not try to open GPS device if no path set */
        i = lgw_gps_enable(GW.gps.gps_tty_path, "ubx7", 0, &GW.gps.gps_tty_fd); /* HAL only supports u-blox 7 for now */
        if (i != LGW_GPS_SUCCESS) {
            lgw_log(LOG_WARNING, "WARNING~ [main] impossible to open %s for GPS sync (check permissions)\n", GW.gps.gps_tty_path);
            GW.gps.gps_enabled = false;
            GW.gps.gps_ref_valid = false;
        } else {
            lgw_log(LOG_INFO, "INFO~ [main] TTY port %s open for GPS synchronization\n", GW.gps.gps_tty_path);
            GW.gps.gps_enabled = true;
            GW.gps.gps_ref_valid = false;
        }
    }

	/* get timezone info */
	tzset();

    /* starting ghost service */
	if (GW.cfg.ghoststream_enabled == true) {
        ghost_start(GW.cfg.ghost_host, GW.cfg.ghost_port, GW.gps.reference_coord, GW.info.gateway_id);
        lgw_register_atexit(ghost_stop);
        lgw_db_put("loraradio", "ghoststream", "running");
        lgw_log(LOG_INFO, "INFO~ [main] Ghost listener started, ghost packets can now be received.\n");
    }

	/* starting the concentrator */
	if (GW.cfg.radiostream_enabled == true) {
		lgw_log(LOG_INFO, "INFO~ [main] Starting the concentrator\n");
        if (system("/usr/bin/reset_lgw.sh start") != 0) {
            lgw_log(LOG_ERROR, "ERROR~ [main] failed to start SX130X, Please start again!\n");
            exit(EXIT_FAILURE);
        } 
		i = lgw_start();
		if (i == LGW_HAL_SUCCESS) {
            lgw_db_put("loraradio", "radiostream", "running");
			lgw_log(LOG_INFO, "INFO~ [main] concentrator started, radio packets can now be received.\n");
		} else {
            lgw_db_put("loraradio", "radiostream", "hangup");
			lgw_log(LOG_ERROR, "ERROR~ [main] failed to start the concentrator\n");
            if (system("/usr/bin/reset_lgw.sh stop") != 0) {
                lgw_log(LOG_ERROR, "ERROR~ [main] failed to stop SX130X, run script reset_lgw.sh stop!\n");
            exit(EXIT_FAILURE);
        } 
			exit(EXIT_FAILURE);
		}
        if (!strncasecmp(GW.hal.board, "sx1302", 6)) {   
            uint64_t eui;
            i = lgw_get_eui(&eui);
            if (i == LGW_HAL_SUCCESS) {
                lgw_log(LOG_INFO, "INFO~ [main] concentrator EUI 0x%016" PRIx64 "\n", eui);
            }
        }
	} else {
		lgw_log(LOG_WARNING, "WARNING~ Radio is disabled, radio packets cannot be sent or received.\n");
	}

    if (lgw_pthread_create(&thrid_up, NULL, (void *(*)(void *))thread_up, NULL))
		lgw_log(LOG_ERROR, "ERROR~ [main] impossible to create data up thread\n");
    else
        lgw_db_put("thread", "thread_up", "running");

    /* JIT queue initialization */
    jit_queue_init(&GW.tx.jit_queue[0]);
    jit_queue_init(&GW.tx.jit_queue[1]);

	// Timer synchronization needed for downstream ...
#ifdef SX1301MOD
        if (lgw_pthread_create(&thrid_timersync, NULL, (void *(*)(void *))thread_timersync, NULL))
            lgw_log(LOG_ERROR, "ERROR~ [main] impossible to create Timer Sync thread\n");
        else
            lgw_db_put("thread", "thread_timersync", "running");
#endif

    if (lgw_pthread_create(&thrid_jit, NULL, (void *(*)(void *))thread_jit, NULL)) {
        lgw_log(LOG_ERROR, "ERROR~ [main] impossible to create JIT thread\n");
        exit(EXIT_FAILURE);
	}
    else
        lgw_db_put("thread", "thread_jit", "running");

#ifdef SX1302MOD
    /* spawn thread for background spectral scan */
    if (GW.spectral_scan_params.enable == true) {
        i = lgw_pthread_create(&thrid_ss, NULL, (void * (*)(void *))thread_spectral_scan, NULL);
        if (i != 0) {
            lgw_log(LOG_ERROR, "ERROR~ [main] impossible to create Spectral Scan thread\n");
            exit(EXIT_FAILURE);
        }
    }
#endif

	/* spawn thread to manage GPS */
	if (GW.gps.gps_enabled == true) {
		if (lgw_pthread_create(&thrid_gps, NULL, (void *(*)(void *))thread_gps, NULL))
			lgw_log(LOG_ERROR, "ERROR~ [main] impossible to create GPS thread\n");
        else
            lgw_db_put("thread", "thread_gps", "running");
		if (pthread_create(&thrid_valid, NULL, (void *(*)(void *))thread_valid, NULL))
			lgw_log(LOG_ERROR, "ERROR~ [main] impossible to create validation thread\n");
        else
            lgw_db_put("thread", "thread_valid", "running");
	}

	/* spawn thread for watchdog */
	if (GW.cfg.wd_enabled == true) {
        GW.cfg.last_loop = time(NULL);
		if (lgw_pthread_create(&thrid_watchdog, NULL, (void *(*)(void *))thread_watchdog, NULL))
			lgw_log(LOG_ERROR, "ERROR~ [main] impossible to create watchdog thread\n");
        else
            lgw_db_put("thread", "thread_watchdog", "running");
	}

    /* for debug
    LGW_LIST_TRAVERSE(&GW.serv_list, serv_entry, list) { 
        printf("servname: %s\n", serv_entry->info.name);
    }
    */

	/* initialize protocol stacks */
	service_start();
    //LGW_LIST_TRAVERSE(&GW.serv_list, serv_entry, list) {
	//    service_start(serv_entry);
    //}

    //lgw_register_atexit(stop_clean_service);

	/* main loop task : statistics transmission */
	while (!exit_sig && !quit_sig) {
        GW.cfg.last_loop = time(NULL);  // time of gateway last loop

		/* wait for next reporting interval */
		wait_ms(1000 * GW.cfg.time_interval);

		if (exit_sig || quit_sig) {
			break;
		}
		// Create statistics report
		report_start();

		/* Exit strategies. */
		/* Server that are 'off-line may be a reason to exit */

	}

	/* end of the loop ready to exit */

	/* disable watchdog */
	if (GW.cfg.wd_enabled == true)
		pthread_cancel(thrid_watchdog);

	//TODO: Dit heeft nawerk nodig / This needs some more work
	pthread_cancel(thrid_jit);	/* don't wait for jit thread */
	if ((i = pthread_join(thrid_up, NULL)) != 0)
        lgw_log(LOG_ERROR, "ERROR~ failed to join Spectral Scan thread with %d - %s\n", i, strerror(errno));

	if (GW.gps.gps_enabled == true) {
		pthread_cancel(thrid_gps);	        /* don't wait for GPS thread */
		pthread_cancel(thrid_valid);	    /* don't wait for validation thread */
        i = lgw_gps_disable(GW.gps.gps_tty_fd);
        if (i == LGW_HAL_SUCCESS) {
            lgw_log(LOG_INFO, "INFO~ GPS closed successfully\n");
        } else 
            lgw_log(LOG_INFO, "WARNING~ failed to close GPS successfully\n");
	}

#ifdef SX1301MOD
		pthread_cancel(thrid_timersync);	/* don't wait for timer sync thread */
#endif

#ifdef SX1302MOD
    if (GW.spectral_scan_params.enable == true) {
        if ((i = pthread_join(thrid_ss, NULL)) != 0)
            lgw_log(LOG_ERROR, "ERROR~ failed to join Spectral Scan thread with %d - %s\n", i, strerror(errno));
    }
#endif

	//if (GW.cfg.ghoststream_enabled == true) {
    //    ghost_stop();
    //}

    stop_clean_service();

    lgw_run_atexits(1);

    thread_rxpkt_recycle();

	/* if an exit signal was received, try to quit properly */
	if (exit_sig || quit_sig) {
		/* stop the hardware */
		if (GW.cfg.radiostream_enabled == true) {
			i = lgw_stop();
			if (i == LGW_HAL_SUCCESS) {
                if (system("/usr/bin/reset_lgw.sh stop") != 0) {
                    lgw_log(LOG_ERROR, "ERROR~ [main] failed to stop SX1302\n");
                } else 
				    lgw_log(LOG_ERROR, "INFO~ [main] concentrator stopped successfully\n");
			} else {
				lgw_log(LOG_WARNING, "WARNING~ [main] failed to stop concentrator successfully\n");
			}
		}

	}

	printf("INFO~ Exiting packet forwarder program\n");
	exit(EXIT_SUCCESS);
}

/* -------------------------------------------------------------------------- */
/* --- THREAD 1: RECEIVING PACKETS AND FORWARDING THEM ---------------------- */

static void thread_up(void) {

	/* allocate memory for packet fetching and processing */
	struct lgw_pkt_rx_s rxpkt[NB_PKT_MAX];	/* array containing inbound packets + metadata */
	int nb_pkt;

    rxpkts_s *rxpkt_entry = NULL;
    serv_s* serv_entry = NULL;

	pthread_t thrid_recycle;

	lgw_log(LOG_INFO, "INFO~ [main-up] Thread activated for all servers.\n");

	while (!exit_sig && !quit_sig) {

		/* fetch packets */

		if (GW.cfg.radiostream_enabled == true) {
		    pthread_mutex_lock(&GW.hal.mx_concent);
			nb_pkt = lgw_receive(NB_PKT_MAX, rxpkt);
		    pthread_mutex_unlock(&GW.hal.mx_concent);
        } else
			nb_pkt = 0;

		if (nb_pkt == LGW_HAL_ERROR) {
			lgw_log(LOG_ERROR, "ERROR~ [main-up] HAL receive failed, try restart HAL\n");
            /*
		    pthread_mutex_lock(&GW.hal.mx_concent);
			lgw_log(LOG_ERROR, "[main-up] ReStarting... HAL\n");
			if (HAL.lgw_stop() == LGW_HAL_SUCCESS) {
                if (system("/usr/bin/reset_lgw.sh stop") != 0) {
                    lgw_log(LOG_ERROR, "ERROR~ [main-up] restart HAL failed!\n");
                    GW.cfg.radiostream_enabled = false;
                    lgw_log(LOG_WARNING, "WARNING~ [main-up] close radiostream!\n");
                } else if (system("/usr/bin/reset_lgw.sh start") != 0) { 
                    lgw_log(LOG_ERROR, "ERROR~ [main-up] restart HAL failed!\n");
                    GW.cfg.radiostream_enabled = false;
                    lgw_log(LOG_WARNING, "WARNING~ [main-up] close radiostream!\n");
                } else if (HAL.lgw_start() != LGW_HAL_SUCCESS) {
                    lgw_log(LOG_ERROR, "ERROR~ [main-up] restart HAL failed!\n");
                    GW.cfg.radiostream_enabled = false;
                    lgw_log(LOG_WARNING, "WARNING~ [main-up] close radiostream!\n");
                } else 
                    lgw_log(LOG_INFO, "INFO~ [main-up] Restar radio HAL successful!\n");
			} else {
				lgw_log(LOG_WARNING, "WARNING~ [main-up] failed to restart HAL\n");
                GW.cfg.radiostream_enabled = false;
			}
		    pthread_mutex_unlock(&GW.hal.mx_concent);
            */
			exit(EXIT_FAILURE);
		}

		if (GW.cfg.ghoststream_enabled == true)
			nb_pkt = ghost_get(NB_PKT_MAX - nb_pkt, &rxpkt[nb_pkt]) + nb_pkt;

		/* wait a short time if no packets, nor status report */
		if (nb_pkt == 0) {
			wait_ms(DEFAULT_FETCH_SLEEP_MS);
			continue;
		}

        rxpkt_entry = lgw_malloc(sizeof(rxpkts_s));     //rxpkts结构体包含有一个lora_pkt_rx_s结构数组

        if (NULL == rxpkt_entry) {
			wait_ms(DEFAULT_FETCH_SLEEP_MS);
            continue;
        }

        rxpkt_entry->list.next = NULL;
        rxpkt_entry->stamps = 0;
        rxpkt_entry->nb_pkt = nb_pkt;
        rxpkt_entry->bind = GW.serv_list.size;
        memcpy(rxpkt_entry->rxpkt, rxpkt, sizeof(struct lgw_pkt_rx_s) * nb_pkt);

        LGW_LIST_LOCK(&GW.rxpkts_list);
        LGW_LIST_INSERT_HEAD(&GW.rxpkts_list, rxpkt_entry, list);
        LGW_LIST_UNLOCK(&GW.rxpkts_list);

	    lgw_log(LOG_DEBUG, "DEBUG~ [main-up] Size of package list is %d\n", GW.rxpkts_list.size);
        
        LGW_LIST_TRAVERSE(&GW.serv_list, serv_entry, list) {
            if (sem_post(&serv_entry->thread.sema))
	            lgw_log(LOG_DEBUG, "DEBUG~ [%s-up] %s\n", serv_entry->info.name, strerror(errno));
        }

        //service_handle_rxpkt(rxpkt_entry);
        //lgw_free(rxpkt_entry);

        if (GW.rxpkts_list.size > DEFAULT_RXPKTS_LIST_SIZE)    // if number of head list greater than LIST_SIZE  
        //if (GW.rxpkts_list.size > 0)    //debug 
           lgw_pthread_create_detached_background(&thrid_recycle, NULL, (void *(*)(void *))thread_rxpkt_recycle, NULL); 
           //thread_rxpkt_recycle();
	}

	lgw_log(LOG_INFO, "INFO~ [main-up] End of upstream thread\n");
}

/* -------------------------------------------------------------------------- */
/* --- THREAD : recycle rxpkts --------------------- */

static void thread_rxpkt_recycle(void) {
    rxpkts_s* rxpkt_entry = NULL;

	lgw_log(LOG_DEBUG, "\nDEBUG~ [MAIN] running packages recycle thread\n");

    LGW_LIST_LOCK(&GW.rxpkts_list);
    LGW_LIST_TRAVERSE_SAFE_BEGIN(&GW.rxpkts_list, rxpkt_entry, list) {
	    lgw_log(LOG_DEBUG, "\nDEBUG~ [MAIN] recycle thread start traverse, bind=%d\n", rxpkt_entry->bind);
        if (rxpkt_entry->bind < 1 || exit_sig) {
            LGW_LIST_REMOVE_CURRENT(list);
            lgw_free(rxpkt_entry);
            GW.rxpkts_list.size--;
        }
    }
    LGW_LIST_TRAVERSE_SAFE_END;
    LGW_LIST_UNLOCK(&GW.rxpkts_list);
    return;
}

/* -------------------------------------------------------------------------- */
/* --- THREAD 3: CHECKING PACKETS TO BE SENT FROM JIT QUEUE AND SEND THEM --- */
static void thread_jit(void) {
    int result = LGW_HAL_SUCCESS;
    struct lgw_pkt_tx_s pkt;
    int pkt_index = -1;
    uint32_t current_concentrator_time;
    enum jit_error_e jit_result;
    enum jit_pkt_type_e pkt_type;
    uint8_t tx_status;
    int i;

	lgw_log(LOG_INFO, "INFO~ [JIT] starting JIT thread...\n");

    while (!exit_sig && !quit_sig) {
        wait_ms(10);

        for (i = 0; i < LGW_RF_CHAIN_NB; i++) {
            /* transfer data and metadata to the concentrator, and schedule TX */
#ifdef SX1302MOD
            pthread_mutex_lock(&GW.hal.mx_concent);
            lgw_get_instcnt(&current_concentrator_time);
            pthread_mutex_unlock(&GW.hal.mx_concent);
#else
            get_concentrator_time(&current_concentrator_time);
#endif
            jit_result = jit_peek(&GW.tx.jit_queue[i], current_concentrator_time, &pkt_index);
            if (jit_result == JIT_ERROR_OK) {
                if (pkt_index > -1) {
                    jit_result = jit_dequeue(&GW.tx.jit_queue[i], pkt_index, &pkt, &pkt_type);
                    if (jit_result == JIT_ERROR_OK) {
                        /* update beacon stats */
                        if (pkt_type == JIT_PKT_TYPE_BEACON) {
                            /* Compensate breacon frequency with xtal error */
                            pthread_mutex_lock(&GW.hal.mx_xcorr);
                            pkt.freq_hz = (uint32_t)(GW.hal.xtal_correct * (double)pkt.freq_hz);
                            lgw_log(LOG_BEACON, "DEBUG~ [JIT] beacon_pkt.freq_hz=%u (xtal_correct=%.15lf)\n", pkt.freq_hz, GW.hal.xtal_correct);
                            pthread_mutex_unlock(&GW.hal.mx_xcorr);

                            /* Update statistics */
                            pthread_mutex_lock(&GW.log.mx_report);
                            GW.beacon.meas_nb_beacon_sent += 1;
                            pthread_mutex_unlock(&GW.log.mx_report);
                            lgw_log(LOG_INFO, "INFO~ [JIT] Beacon dequeued (count_us=%u)\n", pkt.count_us);
                        }

                        /* check if concentrator is free for sending new packet */
                        pthread_mutex_lock(&GW.hal.mx_concent); /* may have to wait for a fetch to finish */
                        result = lgw_status(pkt.rf_chain, TX_STATUS, &tx_status);
                        pthread_mutex_unlock(&GW.hal.mx_concent); /* free concentrator ASAP */
                        if (result == LGW_HAL_ERROR) {
                            lgw_log(LOG_WARNING, "WARNING~ [JIT] jit_queue[%d] lgw_status failed\n", i);
                        } else {
                            if (tx_status == TX_EMITTING) {
                                lgw_log(LOG_ERROR, "ERROR~ [JIT] concentrator is currently emitting on rf_chain %d\n", i);
                                print_tx_status(tx_status);
                                continue;
                            } else if (tx_status == TX_SCHEDULED) {
                                lgw_log(LOG_WARNING, "WARNING~ [JIT] a downlink was already scheduled on rf_chain %d, overwritting it...\n", i);
                                print_tx_status(tx_status);
                            } else {
                                /* Nothing to do */
                            }
                        }

                        /* send packet to concentrator */
                        pthread_mutex_lock(&GW.hal.mx_concent); /* may have to wait for a fetch to finish */
                        result = lgw_send(&pkt);
                        pthread_mutex_unlock(&GW.hal.mx_concent); /* free concentrator ASAP */
                        if (result == LGW_HAL_ERROR) {
                            pthread_mutex_lock(&GW.log.mx_report);
                            GW.log.stat_dw.meas_nb_tx_fail += 1;
                            pthread_mutex_unlock(&GW.log.mx_report);
                            lgw_log(LOG_INFO, "WARNING~ [JIT] lgw_send failed on rf_chain %d\n", i);
                            continue;
                        } else {
                            pthread_mutex_lock(&GW.log.mx_report);
                            GW.log.stat_dw.meas_nb_tx_ok += 1;
                            pthread_mutex_unlock(&GW.log.mx_report);
                            lgw_log(LOG_INFO, "INFO~ [JIT] send done on rf_chain %d in count_us=%u with freq=%u, dr=%u\n", i, pkt.count_us, pkt.freq_hz, pkt.datarate);
                        }
                    } else {
                        lgw_log(LOG_ERROR, "ERROR~ [JIT] jit_dequeue failed on rf_chain %d with %d\n", i, jit_result);
                    }
                }
            } else if (jit_result == JIT_ERROR_EMPTY) {
                /* Do nothing, it can happen */
            } else {
                lgw_log(LOG_ERROR, "ERROR~ [JIT] jit_peek failed on rf_chain %d with %d\n", i, jit_result);
            }
        }
    }

	lgw_log(LOG_INFO, "INFO~ [JIT] END of JIT thread...\n");
}


/* -------------------------------------------------------------------------- */
/* --- THREAD 4: PARSE GPS MESSAGE AND KEEP GATEWAY IN SYNC ----------------- */

static void gps_process_sync(void) {
	struct timespec gps_time;
	struct timespec utc;
	uint32_t trig_tstamp;		/* concentrator timestamp associated with PPM pulse */
	int i = lgw_gps_get(&utc, &gps_time, NULL, NULL);

	/* get GPS time for synchronization */
	if (i != LGW_GPS_SUCCESS) {
		lgw_log(LOG_WARNING, "WARNING~ [GPS] could not get GPS time from GPS\n");
		return;
	}

	/* get timestamp captured on PPM pulse  */
	pthread_mutex_lock(&GW.hal.mx_concent);
	i = lgw_get_trigcnt(&trig_tstamp);
	pthread_mutex_unlock(&GW.hal.mx_concent);
	if (i != LGW_HAL_SUCCESS) {
		lgw_log(LOG_WARNING, "WARNING~ [GPS] failed to read concentrator timestamp\n");
		return;
	}

	/* try to update time reference with the new GPS time & timestamp */
	pthread_mutex_lock(&GW.gps.mx_timeref);
	i = lgw_gps_sync(&GW.gps.time_reference_gps, trig_tstamp, utc, gps_time);
	pthread_mutex_unlock(&GW.gps.mx_timeref);
	if (i != LGW_GPS_SUCCESS) {
		lgw_log(LOG_WARNING, "WARNING~ [GPS] GPS out of sync, keeping previous time reference\n");
	}
}

static void gps_process_coords(void) {
	/* position variable */
	struct coord_s coord;
	struct coord_s gpserr;
	int i = lgw_gps_get(NULL, NULL, &coord, &gpserr);

	/* update gateway coordinates */
	pthread_mutex_lock(&GW.gps.mx_meas_gps);
	if (i == LGW_GPS_SUCCESS) {
		GW.gps.gps_coord_valid = true;
		GW.gps.meas_gps_coord = coord;
		GW.gps.meas_gps_err = gpserr;
		// TODO: report other GPS statistics (typ. signal quality & integrity)
	} else {
		GW.gps.gps_coord_valid = false;
	}
	pthread_mutex_unlock(&GW.gps.mx_meas_gps);
}

static void thread_gps(void) {
	/* serial variables */
	char serial_buff[128];		/* buffer to receive GPS data */
	size_t wr_idx = 0;			/* pointer to end of chars in buffer */

	/* variables for PPM pulse GPS synchronization */
	enum gps_msg latest_msg;	/* keep track of latest NMEA message parsed */

	lgw_log(LOG_INFO, "INFO~ [GPS] GPS thread starting\n");
	/* initialize some variables before loop */
	memset(serial_buff, 0, sizeof serial_buff);

	while (!exit_sig && !quit_sig) {
		size_t rd_idx = 0;
		size_t frame_end_idx = 0;

		/* blocking non-canonical read on serial port */
		ssize_t nb_char = read(GW.gps.gps_tty_fd, serial_buff + wr_idx, LGW_GPS_MIN_MSG_SIZE);
		if (nb_char <= 0) {
			lgw_log(LOG_WARNING, "WARNING~ [GPS] read() returned value %d\n", nb_char);
			continue;
		}
		wr_idx += (size_t) nb_char;

		 /*******************************************
         * Scan buffer for UBX/NMEA sync chars and *
         * attempt to decode frame if one is found *
         *******************************************/
		while (rd_idx < wr_idx) {
			size_t frame_size = 0;

			/* Scan buffer for UBX sync char */
			if (serial_buff[rd_idx] == (char)LGW_GPS_UBX_SYNC_CHAR) {

		/***********************
                 * Found UBX sync char *
                 ***********************/
				latest_msg = lgw_parse_ubx(&serial_buff[rd_idx], (wr_idx - rd_idx), &frame_size);

				if (frame_size > 0) {
					if (latest_msg == INCOMPLETE) {
						/* UBX header found but frame appears to be missing bytes */
						frame_size = 0;
					} else if (latest_msg == INVALID) {
						/* message header received but message appears to be corrupted */
						lgw_log(LOG_WARNING, "WARNING~ [GPS] could not get a valid message from GPS (no time)\n");
						frame_size = 0;
					} else if (latest_msg == UBX_NAV_TIMEGPS) {
						gps_process_sync();
					}
				}
			} else if (serial_buff[rd_idx] == LGW_GPS_NMEA_SYNC_CHAR) {
		        /************************
                 * Found NMEA sync char *
                 ************************/
				/* scan for NMEA end marker (LF = 0x0a) */
				char *nmea_end_ptr = memchr(&serial_buff[rd_idx], (int)0x0a, (wr_idx - rd_idx));

				if (nmea_end_ptr) {
					/* found end marker */
					frame_size = nmea_end_ptr - &serial_buff[rd_idx] + 1;
					latest_msg = lgw_parse_nmea(&serial_buff[rd_idx], frame_size);

					if (latest_msg == INVALID || latest_msg == UNKNOWN) {
						/* checksum failed */
						frame_size = 0;
					} else if (latest_msg == NMEA_GGA) {	/* Get location from GGA frames */
						gps_process_coords();
					} else if (latest_msg == NMEA_RMC) {	/* Get time/date from RMC frames */
						gps_process_sync();
					}
				}
			}

			if (frame_size > 0) {
				/* At this point message is a checksum verified frame
				   we're processed or ignored. Remove frame from buffer */
				rd_idx += frame_size;
				frame_end_idx = rd_idx;
			} else {
				rd_idx++;
			}
		}/* ...for(rd_idx = 0... */

		if (frame_end_idx) {
			/* Frames have been processed. Remove bytes to end of last processed frame */
			memcpy(serial_buff, &serial_buff[frame_end_idx], wr_idx - frame_end_idx);
			wr_idx -= frame_end_idx;
		}

		/* ...for(rd_idx = 0... */
		/* Prevent buffer overflow */
		if ((sizeof(serial_buff) - wr_idx) < LGW_GPS_MIN_MSG_SIZE) {
			memcpy(serial_buff, &serial_buff[LGW_GPS_MIN_MSG_SIZE], wr_idx - LGW_GPS_MIN_MSG_SIZE);
			wr_idx -= LGW_GPS_MIN_MSG_SIZE;
		}
	}
	lgw_log(LOG_INFO, "INFO~ [GPS] End of GPS thread\n");
}

/* -------------------------------------------------------------------------- */
/* --- THREAD 5: CHECK TIME REFERENCE AND CALCULATE XTAL CORRECTION --------- */

static void thread_valid(void) {

	/* GPS reference validation variables */
	long gps_ref_age = 0;
	bool ref_valid_local = false;
	double xtal_err_cpy;

	/* variables for XTAL correction averaging */
	unsigned init_cpt = 0;
	double init_acc = 0.0;
	double x;

	lgw_log(LOG_INFO, "INFO~ [valid] Validation thread activated.\n");

	/* main loop task */
	while (!exit_sig && !quit_sig) {
		wait_ms(1000);

		/* calculate when the time reference was last updated */
		pthread_mutex_lock(&GW.gps.mx_timeref);
		gps_ref_age = (long)difftime(time(NULL), GW.gps.time_reference_gps.systime);
		if ((gps_ref_age >= 0) && (gps_ref_age <= GPS_REF_MAX_AGE)) {
			/* time ref is ok, validate and  */
			GW.gps.gps_ref_valid = true;
			ref_valid_local = true;
			xtal_err_cpy = GW.gps.time_reference_gps.xtal_err;
		} else {
			/* time ref is too old, invalidate */
			GW.gps.gps_ref_valid = false;
			ref_valid_local = false;
		}
		pthread_mutex_unlock(&GW.gps.mx_timeref);

		/* manage XTAL correction */
		if (ref_valid_local == false) {
			/* couldn't sync, or sync too old -> invalidate XTAL correction */
			pthread_mutex_lock(&GW.hal.mx_xcorr);
			GW.hal.xtal_correct_ok = false;
			GW.hal.xtal_correct = 1.0;
			pthread_mutex_unlock(&GW.hal.mx_xcorr);
			init_cpt = 0;
			init_acc = 0.0;
		} else {
			if (init_cpt < XERR_INIT_AVG) {
				/* initial accumulation */
				init_acc += xtal_err_cpy;
				++init_cpt;
			} else if (init_cpt == XERR_INIT_AVG) {
				/* initial average calculation */
				pthread_mutex_lock(&GW.hal.mx_xcorr);
				GW.hal.xtal_correct = (double)(XERR_INIT_AVG) / init_acc;
				GW.hal.xtal_correct_ok = true;
				pthread_mutex_unlock(&GW.hal.mx_xcorr);
				++init_cpt;
				// fprintf(log_file,"%.18lf,\"average\"\n", GW.hal.xtal_correct); // DEBUG
			} else {
				/* tracking with low-pass filter */
				x = 1 / xtal_err_cpy;
				pthread_mutex_lock(&GW.hal.mx_xcorr);
				GW.hal.xtal_correct = GW.hal.xtal_correct - GW.hal.xtal_correct / XERR_FILT_COEF + x / XERR_FILT_COEF;
				pthread_mutex_unlock(&GW.hal.mx_xcorr);
				// fprintf(log_file,"%.18lf,\"track\"\n", GW.hal.xtal_correct); // DEBUG
			}
		}
		lgw_log(LOG_INFO, "Time ref: %s, XTAL correct: %s (%.15lf)\n", 
                ref_valid_local ? "valid" : "invalid", GW.hal.xtal_correct_ok ? "valid" : "invalid", GW.hal.xtal_correct);	// DEBUG
	}
	lgw_log(LOG_INFO, "INFO~ [valid] End of validation thread\n");
}

/* -------------------------------------------------------------------------- */
/* --- THREAD 6: WATCHDOG TO CHECK IF THE SOFTWARE ACTUALLY FORWARDS DATA --- */

static void thread_watchdog(void) {
	/* main loop task */
	lgw_log(LOG_INFO, "INFO~ [WD] Watchdog starting...\n");
	while (!exit_sig && !quit_sig) {
		wait_ms(30000);
		// timestamp updated within the last 3 stat intervals? If not assume something is wrong and exit
		if ((time(NULL) - GW.cfg.last_loop) > (long int)((GW.cfg.time_interval * 3) + 5)) {
			lgw_log(LOG_ERROR, "ERROR~ [WD] Watchdog timer expired!\n");
			exit(254);
		}
	}
	lgw_log(LOG_INFO, "INFO~ [WD] Watchdog ENDED!\n");
}

/* -------------------------------------------------------------------------- */
/* --- THREAD : BACKGROUND SPECTRAL SCAN                                  --- */
#ifdef SX1302MOD
static void thread_spectral_scan(void) {
    int i, x;
    uint32_t freq_hz = GW.spectral_scan_params.freq_hz_start;
    uint32_t freq_hz_stop = GW.spectral_scan_params.freq_hz_start + GW.spectral_scan_params.nb_chan * 200E3;
    int16_t levels[LGW_SPECTRAL_SCAN_RESULT_SIZE];
    uint16_t results[LGW_SPECTRAL_SCAN_RESULT_SIZE];
    struct timeval tm_start;
    lgw_spectral_scan_status_t status;
    uint8_t tx_status = TX_FREE;
    bool spectral_scan_started;
    bool exit_thread = false;

    /* main loop task */
    while (!exit_sig && !quit_sig) {
        /* Pace the scan thread (1 sec min), and avoid waiting several seconds when exit */
        for (i = 0; i < (int)(GW.spectral_scan_params.pace_s ? GW.spectral_scan_params.pace_s : 1); i++) {
            if (exit_sig || quit_sig) {
                exit_thread = true;
                break;
            }
            wait_ms(1000);
        }
        if (exit_thread == true) {
            break;
        }

        spectral_scan_started = false;

        /* Start spectral scan (if no downlink programmed) */
        pthread_mutex_lock(&GW.hal.mx_concent);
        /* -- Check if there is a downlink programmed */
        for (i = 0; i < LGW_RF_CHAIN_NB; i++) {
            if (GW.tx.tx_enable[i] == true) {
                x = lgw_status((uint8_t)i, TX_STATUS, &tx_status);
                if (x != LGW_HAL_SUCCESS) {
                    lgw_log(LOG_ERROR, "ERROR~ [scan] failed to get TX status on chain %d\n", i);
                } else {
                    if (tx_status == TX_SCHEDULED || tx_status == TX_EMITTING) {
                        lgw_log(LOG_INFO, "INFO~ [scan] skip spectral scan (downlink programmed on RF chain %d)\n", i);
                        break; /* exit for loop */
                    }
                }
            }
        }
        if (tx_status != TX_SCHEDULED && tx_status != TX_EMITTING) {
            x = lgw_spectral_scan_start(freq_hz, GW.spectral_scan_params.nb_scan);
            if (x != 0) {
                lgw_log(LOG_ERROR, "ERROR~ [scan] spectral scan start failed\n");
                pthread_mutex_unlock(&GW.hal.mx_concent);
                continue; /* main while loop */
            }
            spectral_scan_started = true;
        }
        pthread_mutex_unlock(&GW.hal.mx_concent);

        if (spectral_scan_started == true) {
            /* Wait for scan to be completed */
            status = LGW_SPECTRAL_SCAN_STATUS_UNKNOWN;
            timeout_start(&tm_start);
            do {
                /* handle timeout */
                if (timeout_check(tm_start, 2000) != 0) {
                    lgw_log(LOG_ERROR, "ERROR~ %s: [scan] TIMEOUT on Spectral Scan\n", __FUNCTION__);
                    break;  /* do while */
                }

                /* get spectral scan status */
                pthread_mutex_lock(&GW.hal.mx_concent);
                x = lgw_spectral_scan_get_status(&status);
                pthread_mutex_unlock(&GW.hal.mx_concent);
                if (x != 0) {
                    lgw_log(LOG_ERROR, "ERROR~ [scan] spectral scan status failed\n");
                    break; /* do while */
                }

                /* wait a bit before checking status again */
                wait_ms(10);
            } while (status != LGW_SPECTRAL_SCAN_STATUS_COMPLETED && status != LGW_SPECTRAL_SCAN_STATUS_ABORTED);

            if (status == LGW_SPECTRAL_SCAN_STATUS_COMPLETED) {
                /* Get spectral scan results */
                memset(levels, 0, sizeof levels);
                memset(results, 0, sizeof results);
                pthread_mutex_lock(&GW.hal.mx_concent);
                x = lgw_spectral_scan_get_results(levels, results);
                pthread_mutex_unlock(&GW.hal.mx_concent);
                if (x != 0) {
                    lgw_log(LOG_ERROR, "ERROR~ [scan] spectral scan get results failed\n");
                    continue; /* main while loop */
                }

                /* print results */
                lgw_log(LOG_INFO, "INFO~ [scan] SPECTRAL SCAN - %u Hz: ", freq_hz);
                for (i = 0; i < LGW_SPECTRAL_SCAN_RESULT_SIZE; i++) {
                    lgw_log(LOG_INFO, "%u ", results[i]);
                }
                lgw_log(LOG_INFO, "\n");

                /* Next frequency to scan */
                freq_hz += 200000; /* 200kHz channels */
                if (freq_hz >= freq_hz_stop) {
                    freq_hz = GW.spectral_scan_params.freq_hz_start;
                }
            } else if (status == LGW_SPECTRAL_SCAN_STATUS_ABORTED) {
                lgw_log(LOG_INFO, "INFO~ [scan] %s: spectral scan has been aborted\n", __FUNCTION__);
            } else {
                lgw_log(LOG_ERROR, "ERROR~ [scan] %s: spectral scan status us unexpected 0x%02X\n", __FUNCTION__, status);
            }
        }
    }
    lgw_log(LOG_INFO, "\nINFO~ [scan] End of Spectral Scan thread\n");
}
#endif

/* --- EOF ------------------------------------------------------------------ */
