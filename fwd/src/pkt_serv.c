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
 * \brief 
 *  Description:
*/

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include <semaphore.h>
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>

#include "db.h"
#include "fwd.h"
#include "jitqueue.h"
#include "loragw_hal.h"
#include "loragw_aux.h"
#include "timersync.h"

#include "service.h"
#include "pkt_service.h"
#include "loramac-crypto.h"
#include "mac-header-decode.h"

DECLARE_GW;

static uint8_t rx2bw;
static uint8_t rx2dr;
static uint32_t rx2freq;

static char db_family[32] = {'\0'};
static char db_key[32] = {'\0'};
static char tmpstr[16] = {'\0'};

LGW_LIST_HEAD_STATIC(dn_list, _dn_pkt); // downlink for customer

static void pkt_prepare_downlink(void* arg);
static void pkt_deal_up(void* arg);
static void prepare_frame(uint8_t, devinfo_s*, uint32_t, const uint8_t*, int, uint8_t*, int*);
static int strcpypt(char* dest, const char* src, int* start, int size, int len);

static enum jit_error_e custom_rx2dn(dn_pkt_s* dnelem, devinfo_s *devinfo, uint32_t us, uint8_t txmode) {
    int i, fsize = 0;

    uint32_t dwfcnt = 0;

    uint8_t payload_en[DEFAULT_PAYLOAD_SIZE] = {'\0'};  /* data which have decrypted */
    struct lgw_pkt_tx_s txpkt;

    uint32_t current_concentrator_time;
    enum jit_error_e jit_result = JIT_ERROR_OK;
    enum jit_pkt_type_e downlink_type;

    memset(&txpkt, 0, sizeof(txpkt));

    if (dnelem->mode > 0)
        txpkt.modulation = dnelem->mode;
    else
        txpkt.modulation = MOD_LORA;

    if (dnelem->rxwindow > 1)
        txpkt.count_us = us + 2000000UL; /* rx2 window plus 2s */
    else
        txpkt.count_us = us + 1000000UL; /* rx1 window plus 1s */

    txpkt.no_crc = true;

    if (dnelem->txfreq > 0)
        txpkt.freq_hz = dnelem->txfreq; 
    else
        txpkt.freq_hz = rx2freq; 

    txpkt.rf_chain = 0;

    if (dnelem->txpw > 0)
        txpkt.rf_power = dnelem->txpw;
    else
        txpkt.rf_power = 20;

    if (dnelem->txdr > 0)
        txpkt.datarate = dnelem->txdr;
    else
        txpkt.datarate = rx2dr;

    if (dnelem->txbw > 0)
        txpkt.bandwidth = dnelem->txbw;
    else
        txpkt.bandwidth = rx2bw;

    txpkt.coderate = CR_LORA_4_5;

    txpkt.invert_pol = true;

    txpkt.preamble = STD_LORA_PREAMB;

    txpkt.tx_mode = txmode;

    if (txmode)
        downlink_type = JIT_PKT_TYPE_DOWNLINK_CLASS_A;
    else
        downlink_type = JIT_PKT_TYPE_DOWNLINK_CLASS_C;

    /* 这个key重启将会删除, 下发的计数器 */
    sprintf(db_family, "/downlink/%08X", devinfo->devaddr);
    if (lgw_db_get(db_family, "fcnt", tmpstr, sizeof(tmpstr)) == -1) {
        lgw_db_put(db_family, "fcnt", "0");
    } else { 
        dwfcnt = atol(tmpstr);
        sprintf(tmpstr, "%u", dwfcnt + 1);
        lgw_db_put(db_family, "fcnt", tmpstr);
    }

    /* prepare MAC message */
    lgw_memset(payload_en, '\0', sizeof(payload_en));

    prepare_frame(FRAME_TYPE_DATA_UNCONFIRMED_DOWN, devinfo, dwfcnt++, (uint8_t *)dnelem->payload, dnelem->psize, payload_en, &fsize);

    lgw_memcpy(txpkt.payload, payload_en, fsize);

    txpkt.size = fsize;

    lgw_log(LOG_DEBUG, "DEBUG~ [DNLK] TX(%d):", fsize);
    for (i = 0; i < fsize; ++i) {
        lgw_log(LOG_DEBUG, "%02X", payload_en[i]);
    }
    lgw_log(LOG_DEBUG, "\n");

#ifdef SX1302MOD
    pthread_mutex_lock(&GW.hal.mx_concent);
    lgw_get_instcnt(&current_concentrator_time);
    pthread_mutex_unlock(&GW.hal.mx_concent);
#else
    get_concentrator_time(&current_concentrator_time);
#endif
    jit_result = jit_enqueue(&GW.tx.jit_queue[txpkt.rf_chain], current_concentrator_time, &txpkt, downlink_type);
    lgw_log(LOG_DEBUG, "DEBUG~ [DNLK] DNRX2-> tmst:%u, freq:%u, size:%u, SF%uBW%uHZ\n", txpkt.count_us, txpkt.freq_hz, txpkt.size, txpkt.datarate, txpkt.bandwidth);
    return jit_result;
}

static void prepare_frame(uint8_t type, devinfo_s *devinfo, uint32_t downcnt, const uint8_t* payload, int payload_size, uint8_t* frame, int* frame_size) {
	uint32_t mic;
	uint8_t index = 0;
	uint8_t* encrypt_payload;

	LoRaMacHeader_t hdr;
	LoRaMacFrameCtrl_t fctrl;

	/*MHDR*/
	hdr.Value = 0;
	hdr.Bits.MType = type;
	frame[index] = hdr.Value;

	/*DevAddr*/
	frame[++index] = devinfo->devaddr&0xFF;
	frame[++index] = (devinfo->devaddr>>8)&0xFF;
	frame[++index] = (devinfo->devaddr>>16)&0xFF;
	frame[++index] = (devinfo->devaddr>>24)&0xFF;

	/*FCtrl*/
	fctrl.Value = 0;
	if(type == FRAME_TYPE_DATA_UNCONFIRMED_DOWN){
		fctrl.Bits.Ack = 1;
	}
	fctrl.Bits.Adr = 1;
	frame[++index] = fctrl.Value;

	/*FCnt*/
	frame[++index] = (downcnt)&0xFF;
	frame[++index] = (downcnt>>8)&0xFF;

	/*FOpts*/
	/*Fport*/
	frame[++index] = (DEFAULT_DOWN_FPORT)&0xFF;

	/*encrypt the payload*/
	encrypt_payload = lgw_malloc(sizeof(uint8_t) * payload_size);
	LoRaMacPayloadEncrypt(payload, payload_size, (DEFAULT_DOWN_FPORT == 0) ? devinfo->nwkskey : devinfo->appskey, devinfo->devaddr, DOWN, downcnt, encrypt_payload);
	++index;
	memcpy(frame + index, encrypt_payload, payload_size);
	lgw_free(encrypt_payload);
	index += payload_size;

	/*calculate the mic*/
	LoRaMacComputeMic(frame, index, devinfo->nwkskey, devinfo->devaddr, DOWN, downcnt, &mic);
    //printf("INFO~ [MIC] %08X\n", mic);
	frame[index] = mic&0xFF;
	frame[++index] = (mic>>8)&0xFF;
	frame[++index] = (mic>>16)&0xFF;
	frame[++index] = (mic>>24)&0xFF;
	*frame_size = index + 1;
}

static inline dn_pkt_s* search_dn_list(const char* addr) {
    dn_pkt_s* entry = NULL;

    LGW_LIST_LOCK(&dn_list);
    LGW_LIST_TRAVERSE(&dn_list, entry, list) {
        if (!strcasecmp(entry->devaddr, addr))
            break;
    }
    LGW_LIST_UNLOCK(&dn_list);

    return entry;
}


int pkt_start(serv_s* serv) {
    if (lgw_pthread_create_background(&serv->thread.t_up, NULL, (void *(*)(void *))pkt_deal_up, serv)) {
        lgw_log(LOG_WARNING, "WARNING~ [%s] Can't create packages deal pthread.\n", serv->info.name);
        return -1;
    }

    if (GW.cfg.custom_downlink) {
        switch (GW.cfg.region) {
            case EU: 
                rx2dr = DR_LORA_SF12;
                rx2bw = BW_125KHZ;
                rx2freq = 869525000UL;
                break;
            case US:
                rx2dr = DR_LORA_SF12;
                rx2bw = BW_500KHZ;
                rx2freq = 923300000UL;
                break;
            case CN:
                rx2dr = DR_LORA_SF12;
                rx2bw = BW_125KHZ;
                rx2freq = 505300000UL;
                break;
            case CN780:
                rx2dr = DR_LORA_SF12;
                rx2bw = BW_125KHZ;
                rx2freq = 786000000UL;
                break;
            case AU:
                rx2dr = DR_LORA_SF12;
                rx2bw = BW_500KHZ;
                rx2freq = 923300000UL;
                break;
            case AS1:
            case AS2:
                rx2dr = DR_LORA_SF10;
                rx2bw = BW_125KHZ;
                rx2freq = 923200000UL;
                break;
            case KR:
                rx2dr = DR_LORA_SF12;
                rx2bw = BW_125KHZ;
                rx2freq = 921900000UL;
                break;
            case IN:
                rx2dr = DR_LORA_SF12;
                rx2bw = BW_125KHZ;
                rx2freq = 866550000UL;
                break;
            case RU:
                rx2dr = DR_LORA_SF12;
                rx2bw = BW_125KHZ;
                rx2freq = 869100000UL;
                break;
            default:
                rx2dr = DR_LORA_SF12;
                rx2bw = BW_125KHZ;
                rx2freq = 869525000UL;
                break;
        }

        if (lgw_pthread_create_background(&serv->thread.t_down, NULL, (void *(*)(void *))pkt_prepare_downlink, (void*)serv)) {
            lgw_log(LOG_WARNING, "WARNING~ [%s] Can't create pthread for custom downlonk.\n", serv->info.name);
            return -1;
        }
    }

    lgw_db_put("service/pkt", serv->info.name, "runing");
    lgw_db_put("thread", serv->info.name, "runing");

    return 0;
}

void pkt_stop(serv_s* serv) {
    serv->thread.stop_sig = true;
	sem_post(&serv->thread.sema);
	pthread_join(serv->thread.t_up, NULL);
    if (GW.cfg.custom_downlink) {
	    pthread_join(serv->thread.t_down, NULL);
    }
    lgw_db_del("service/pkt", serv->info.name);
    lgw_db_del("thread", serv->info.name);
}

static void pkt_deal_up(void* arg) {
    serv_s* serv = (serv_s*) arg;
    lgw_log(LOG_INFO, "INFO~ [%s] Staring pkt_deal_up thread\n", serv->info.name);

	int i;					/* loop variables */
    int fsize = 0;
    int index = 0;
    int nb_pkt = 0;

	struct lgw_pkt_rx_s *p;	/* pointer on a RX packet */

    rxpkts_s* rxpkt_entry = NULL;

	int buff_index;

    enum jit_error_e jit_result = JIT_ERROR_OK;
    
    uint8_t payload_encrypt[DEFAULT_PAYLOAD_SIZE] = {'\0'};
    uint8_t payload_txt[DEFAULT_PAYLOAD_SIZE] = {'\0'};

    LoRaMacMessageData_t macmsg;

	while (!serv->thread.stop_sig) {
		// wait for data to arrive
		sem_wait(&serv->thread.sema);

        nb_pkt = get_rxpkt(serv);     //only get the first rxpkt of list

        if (nb_pkt == 0)
            continue;

        lgw_log(LOG_DEBUG, "DEBUG~ [%s] pkt_push_up fetch %d pachages.\n", serv->info.name, nb_pkt);

        /* serialize one Lora packet metadata and payload */
        for (i = 0; i < nb_pkt; i++) {
            p = &serv->rxpkt[i];

            if (p->size < 13) /* offset of frmpayload */
                continue;

            memset(&macmsg, 0, sizeof(macmsg));
            macmsg.Buffer = p->payload;
            macmsg.BufSize = p->size;

            if (LoRaMacParserData(&macmsg) != LORAMAC_PARSER_SUCCESS)
                continue;

            decode_mac_pkt_up(&macmsg, p);

            if (GW.cfg.mac_decoded || GW.cfg.custom_downlink) {
                devinfo_s devinfo = { .devaddr = macmsg.FHDR.DevAddr, 
                                      .devaddr_str = {0},
                                      .appskey_str = {0},
                                      .nwkskey_str = {0}
                                    };
                sprintf(db_family, "devinfo/%08X", devinfo.devaddr);
                if ((lgw_db_get(db_family, "appskey", devinfo.appskey_str, sizeof(devinfo.appskey_str)) == -1) || 
                    (lgw_db_get(db_family, "nwkskey", devinfo.nwkskey_str, sizeof(devinfo.nwkskey_str)) == -1)) {
                    continue;
                }

                str2hex(devinfo.appskey, devinfo.appskey_str, sizeof(devinfo.appskey));
                str2hex(devinfo.nwkskey, devinfo.nwkskey_str, sizeof(devinfo.nwkskey));

                /* Debug message of appskey */
                /*
                lgw_log(LOG_DEBUG, "\nDEBUG~ [MAC-Decode]appskey:");
                for (j = 0; j < (int)sizeof(devinfo.appskey); ++j) {
                    lgw_log(LOG_DEBUG, "%02X", devinfo.appskey[j]);
                }
                lgw_log(LOG_DEBUG, "\n");
                */

                if (GW.cfg.mac_decoded) {
                    int l;
                    uint32_t fcnt, mic;
					bool fcnt_valid = false;
                    fsize = p->size - 13 - macmsg.FHDR.FCtrl.Bits.FOptsLen; 
                    memcpy(payload_encrypt, p->payload + 9 + macmsg.FHDR.FCtrl.Bits.FOptsLen, fsize);
                    for (l = 0; l < FCNT_GAP; l++) {   // loop 8 times
                        fcnt = macmsg.FHDR.FCnt | (l * 0x10000);
                        LoRaMacComputeMic(p->payload, p->size - 4, devinfo.nwkskey, devinfo.devaddr, UP, fcnt, &mic);
                        lgw_log(LOG_DEBUG, "DEBUG~ [MIC] mic=%08X, MIC=%08X, fcnt=%u, FCNT=%u\n", mic, macmsg.MIC, fcnt, macmsg.FHDR.FCnt);
                        if (mic == macmsg.MIC) {
                            fcnt_valid = true;
                            lgw_log(LOG_DEBUG, "DEBUG~ [MIC] Found a match fcnt(=%u)\n", fcnt);
                            break;
                        }
                    }

                    if (fcnt_valid) {
                    
                        if (macmsg.FPort == 0)
                            LoRaMacPayloadDecrypt(payload_encrypt, fsize, devinfo.nwkskey, devinfo.devaddr, UP, fcnt, payload_txt);
                        else
                            LoRaMacPayloadDecrypt(payload_encrypt, fsize, devinfo.appskey, devinfo.devaddr, UP, fcnt, payload_txt);

                        /* Debug message of decoded payload */
                        lgw_log(LOG_DEBUG, "\nDEBUG~ [MAC-Decode] RX(%d):", fsize);
                        for (i = 0; i < fsize; ++i) {
                            lgw_log(LOG_DEBUG, "%02X", payload_txt[i]);
                        }
                        lgw_log(LOG_DEBUG, "\n");

                        if (GW.cfg.mac2file) {
                            FILE *fp;
                            char pushpath[128];
                            char rssi_snr[18] = {'\0'};
                            snprintf(pushpath, sizeof(pushpath), "/var/iot/channels/%08X", devinfo.devaddr);
                            fp = fopen(pushpath, "w+");
                            if (NULL == fp)
                                lgw_log(LOG_WARNING, "WARNING~ [Decrypto] Fail to open path: %s\n", pushpath);
                            else { 
                                sprintf(rssi_snr, "%08X%08X", (short)p->rssic, (short)(p->snr*10));
                                fwrite(rssi_snr, sizeof(char), 16, fp);
                                fwrite(payload_txt, sizeof(uint8_t), fsize, fp);
                                fflush(fp); 
                                fclose(fp);
                            }
                        
                        }

                        if (GW.cfg.mac2db) { /* 每个devaddr最多保存10个payload */
                            sprintf(db_family, "/payload/%08X", devinfo.devaddr);
                            if (lgw_db_get(db_family, "index", tmpstr, sizeof(tmpstr)) == -1) {
                                lgw_db_put(db_family, "index", "0");
                            } else 
                                index = atoi(tmpstr) % 9;
                            sprintf(db_family, "/payload/%08X/%d", devinfo.devaddr, index);
                            sprintf(db_key, "%u", p->count_us);
                            lgw_db_put(db_family, db_key, (char*)payload_txt);
                        }
                    }
                }

                if (GW.cfg.custom_downlink) {
                    /* Customer downlink process */
                    dn_pkt_s* dnelem = NULL;
                    sprintf(tmpstr, "%08X", devinfo.devaddr);
                    dnelem = search_dn_list(tmpstr);
                    if (dnelem != NULL) {
                        lgw_log(LOG_DEBUG, "DEBUG~ [DNLK]Found a match devaddr: %s, prepare a downlink!\n", tmpstr);
                        jit_result = custom_rx2dn(dnelem, &devinfo, p->count_us, TIMESTAMPED);
                        if (jit_result == JIT_ERROR_OK) { /* Next upmsg willbe indicate if received by note */
                            LGW_LIST_LOCK(&dn_list);
                            LGW_LIST_REMOVE(&dn_list, dnelem, list);
                            lgw_free(dnelem);
                            LGW_LIST_UNLOCK(&dn_list);
                        } else {
                            lgw_log(LOG_ERROR, "ERROR~ [DNLK]Packet REJECTED (jit error=%d)\n", jit_result);
                        }

                    } else
                        lgw_log(LOG_DEBUG, "DEBUG~ [DNLK] Can't find SessionKeys for Dev %08X\n", devinfo.devaddr);
                }
            }
        }
	}
    lgw_log(LOG_INFO, "INFO~ [%s] END of pkt_deal_up thread\n", serv->info.name);
}

static void pkt_prepare_downlink(void* arg) {
    serv_s* serv = (serv_s*) arg;
    lgw_log(LOG_INFO, "INFO~ [%s] Staring pkt_prepare_downlink thread...\n", serv->info.name);
    
    int i, j, start; /* loop variables, start of cpychar */
    uint8_t psize = 0, size = 0; /* sizeof payload, file char lenth */

    uint32_t uaddr;

    DIR *dir;
    FILE *fp;
    struct dirent *ptr;
    struct stat statbuf;
    char dn_file[128]; 

    /* data buffers */
    uint8_t buff_down[512]; /* buffer to receive downstream packets */
    uint8_t dnpld[256];
    uint8_t hexpld[256];
    
    dn_pkt_s* entry = NULL;

    dn_pkt_s* element = NULL;

    enum jit_error_e jit_result = JIT_ERROR_OK;

    while (!serv->thread.stop_sig) {
        
        /* lookup file */
        if ((dir = opendir(DNPATH)) == NULL) {
            //lgw_log(LOG_ERROR, "ERROR~ [push]open sending path error\n");
            wait_ms(100); 
            continue;
        }

	    while ((ptr = readdir(dir)) != NULL) {
            if (strcmp(ptr->d_name, ".") == 0 || strcmp(ptr->d_name, "..") == 0) /* current dir OR parrent dir */
                continue;

            lgw_log(LOG_INFO, "INFO~ [DNLK]Looking file : %s\n", ptr->d_name);

            snprintf(dn_file, sizeof(dn_file), "%s/%s", DNPATH, ptr->d_name);

            if (stat(dn_file, &statbuf) < 0) {
                lgw_log(LOG_ERROR, "ERROR~ [DNLK]Canot stat %s!\n", ptr->d_name);
                continue;
            }

            if ((statbuf.st_mode & S_IFMT) == S_IFREG) {
                if ((fp = fopen(dn_file, "r")) == NULL) {
                    lgw_log(LOG_ERROR, "ERROR~ [DNLK]Cannot open %s\n", ptr->d_name);
                    continue;
                }

                lgw_memset(buff_down, '\0', sizeof(buff_down));

                size = fread(buff_down, sizeof(char), sizeof(buff_down), fp); /* the size less than buff_down return EOF */
                fclose(fp);

                unlink(dn_file); /* delete the file */

                for (i = 0, j = 0; i < size; i++) {
                    if (buff_down[i] == ',')
                        j++;
                }

                if (j < 3) { /* Error Format, ',' must be greater than or equal to 3*/
                    lgw_log(LOG_INFO, "INFO~ [DNLK]Format error: %s\n", buff_down);
                    continue;
                }

                start = 0;

                entry = (dn_pkt_s*) lgw_malloc(sizeof(dn_pkt_s));

                if (strcpypt(tmpstr, (char*)buff_down, &start, size, sizeof(tmpstr)) < 1) { 
                    lgw_free(entry);
                    continue;
                } else
                    strcpy(entry->devaddr, tmpstr);

                if (strcpypt(tmpstr, (char*)buff_down, &start, size, sizeof(tmpstr)) < 1)
                    strcpy(entry->txmode, "time");
                else
                    strcpy(entry->txmode, tmpstr); 

                if (strcpypt(tmpstr, (char*)buff_down, &start, size, sizeof(tmpstr)) < 1)
                    strcpy(entry->pdformat, "txt"); 
                else
                    strcpy(entry->pdformat, tmpstr); 

                psize = strcpypt((char*)dnpld, (char*)buff_down, &start, size, sizeof(dnpld)); 
                if (psize < 1) {
                    lgw_free(entry);
                    continue;
                }

                if (strcpypt(tmpstr, (char*)buff_down, &start, size, sizeof(tmpstr)) > 0) {
                    entry->txpw = atoi(tmpstr);
                } else
                    entry->txpw = 0;

                if (strcpypt(tmpstr, (char*)buff_down, &start, size, sizeof(tmpstr)) > 0) {
                    entry->txbw = atoi(tmpstr);
                } else
                    entry->txbw = 0; 

                if (strcpypt(tmpstr, (char*)buff_down, &start, size, sizeof(tmpstr)) > 0) {
                    if (!strncmp(tmpstr, "SF7", 3))
                        entry->txdr = DR_LORA_SF7; 
                    else if (!strncmp(tmpstr, "SF8", 3))
                        entry->txdr = DR_LORA_SF8; 
                    else if (!strncmp(tmpstr, "SF9", 3))
                        entry->txdr = DR_LORA_SF9; 
                    else if (!strncmp(tmpstr, "SF10", 4))
                        entry->txdr = DR_LORA_SF10; 
                    else if (!strncmp(tmpstr, "SF11", 4))
                        entry->txdr = DR_LORA_SF11; 
                    else if (!strncmp(tmpstr, "SF12", 4))
                        entry->txdr = DR_LORA_SF12; 
                    else if (!strncmp(tmpstr, "SF5", 3))
                        entry->txdr = DR_LORA_SF5; 
                    else if (!strncmp(tmpstr, "SF6", 3))
                        entry->txdr = DR_LORA_SF6; 
                    else if (!strncmp(tmpstr, "SF7", 3))
                        entry->txdr = DR_LORA_SF7; 
                    else 
                        entry->txdr = 0; 
                } else 
                    entry->txdr = 0; 

                if (strcpypt(tmpstr, (char*)buff_down, &start, size, sizeof(tmpstr)) > 0) {
                    i = sscanf(tmpstr, "%u", &entry->txfreq);
                    if (i != 1)
                        entry->txfreq = 0;
                } else 
                    entry->txfreq = 0; 

                if (strcpypt(tmpstr, (char*)buff_down, &start, size, sizeof(tmpstr)) > 0) {
                    entry->rxwindow = atoi(tmpstr);
                    if (entry->rxwindow > 2 || entry->rxwindow < 1)
                        entry->rxwindow = 0;
                } else 
                    entry->rxwindow = 2; 

                if (strstr(entry->pdformat, "hex") != NULL) { 
                    if (psize % 2) {
                        lgw_log(LOG_INFO, "INFO~ [DNLK] Size of hex payload invalid.\n");
                        lgw_free(entry);
                        continue;
                    }
                    hex2str(dnpld, hexpld, psize);
                    psize = psize/2;
                    lgw_memcpy(entry->payload, hexpld, psize + 1);
                } else
                    lgw_memcpy(entry->payload, dnpld, psize + 1);

                entry->psize = psize;
				
                lgw_log(LOG_DEBUG, "DEBUG~ [DNLK]devaddr:%s, txmode:%s, pdfm:%s, size:%d, freq:%u, bw:%u, dr:%u\n", entry->devaddr, entry->txmode, entry->pdformat, entry->psize, entry->txfreq, entry->txbw, entry->txdr);
                lgw_log(LOG_DEBUG, "DEBUG~ [DNLK]payload:\"%s\"\n", entry->payload); 

                if (strstr(entry->txmode, "imme") != NULL) {
                    lgw_log(LOG_INFO, "INFO~ [DNLK] Pending IMMEDIATE downlink for %s\n", entry->devaddr);

                    uaddr = strtoul(entry->devaddr, NULL, 16);

                    devinfo_s devinfo = { .devaddr = uaddr };

                    sprintf(db_family, "devinfo/%08X", devinfo.devaddr);

                    if ((lgw_db_get(db_family, "appskey", devinfo.appskey_str, sizeof(devinfo.appskey_str)) == -1) || 
                        (lgw_db_get(db_family, "nwkskey", devinfo.devaddr_str, sizeof(devinfo.nwkskey_str)) == -1)) {
                        lgw_free(entry);
                        continue;
                    }

                    str2hex(devinfo.appskey, devinfo.appskey_str, sizeof(devinfo.appskey));
                    str2hex(devinfo.nwkskey, devinfo.nwkskey_str, sizeof(devinfo.nwkskey));

                    jit_result = custom_rx2dn(entry, &devinfo, 0, IMMEDIATE);

                    if (jit_result != JIT_ERROR_OK)  
                        lgw_log(LOG_ERROR, "ERROR~ [DNLK]Packet REJECTED (jit error=%d)\n", jit_result);
                    else
                        lgw_log(LOG_INFO, "INFO~ [DNLK]No devaddr match, Drop the link of %s\n", entry->devaddr);

                    lgw_free(entry);
                    continue;
                }

                LGW_LIST_LOCK(&dn_list);

                if (dn_list.size > 16) {
                    element = LGW_LIST_REMOVE_HEAD(&dn_list, list);
                    if (element != NULL) {
                        dn_list.size--;
                        lgw_free(element);
                    }
                }
  
                LGW_LIST_TRAVERSE_SAFE_BEGIN(&dn_list, element, list) {
                    if (!strcmp(element->devaddr, entry->devaddr)) {
                        LGW_LIST_REMOVE_CURRENT(list);
                        dn_list.size--;
                        lgw_free(element);
                    }
                }
                LGW_LIST_TRAVERSE_SAFE_END;

                LGW_LIST_INSERT_TAIL(&dn_list, entry, list);
                LGW_LIST_UNLOCK(&dn_list);

            }
            wait_ms(20); /* wait for HAT send or other process */
        }
        if (closedir(dir) < 0)
            lgw_log(LOG_INFO, "INFO~ [DNLK]Cannot close DIR: %s\n", DNPATH);
        wait_ms(100);
    }
    lgw_log(LOG_INFO, "INFO~ [%s] END of pkt_prepare_downlink thread\n", serv->info.name);
}

/*! 
 * \brief copies the string pointed to by src, 
 * \param start the point of src 
 * \param size of the src length 
 * \param len of dest length
 * \retval return the number of characters copyed
 *
 */

static int strcpypt(char* dest, const char* src, int* start, int size, int len)
{
    int i, j;

    i = *start;
    
    while (src[i] == ' ' && i < size) {
        i++;
    }

    if ( i >= size ) return 0;

    for (j = 0; j < len; j++) {
        dest[j] = '\0';
    }

    for (j = 0; i < size; i++) {
        if (src[i] == ',') {
            i++; // skip ','
            break;
        }

        if (j == len - 1) 
            continue;

		if(src[i] != 0 && src[i] != 10 )
            dest[j++] = src[i];
    }

    *start = i;

    return j;
}
/* --- EOF ------------------------------------------------------------------ */

