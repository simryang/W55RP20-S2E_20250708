#ifndef __SEGCP_H
#define __SEGCP_H

#include <stdint.h>

//#define _SEGCP_DEBUG_

#ifndef SEG_DATA0_SOCK
    #define SEG_DATA0_SOCK      0
#endif

#ifndef SEG_DATA1_SOCK
    #define SEG_DATA1_SOCK      1
#endif

#define DEVCONF_VALID				0xA55A
#define DEVCONF_DOMAIN_MAX			32
#define DEVCONF_IDPASS_MAX			16
#define DEVCONF_NAME_MAX			16
#define DEVCONF_CONNPASS_MAX		8
#define DEVCONF_SEARCHPASS_MAX		8

#define SEGCP_CMD_MAX				2
#define SEGCP_PARAM_MAX				256
#define SEGCP_DELIMETER				"\r\n"

// Command [K1] : Hidden command, This command erase the configuration data in flash / or EEPROM
#if (DEVICE_UART_CNT > 1)
typedef enum {SEGCP_MC, SEGCP_VR, SEGCP_MN, SEGCP_IM, SEGCP_OP, SEGCP_DD, SEGCP_CP, SEGCP_PO, SEGCP_DG, SEGCP_KA,
              SEGCP_KI, SEGCP_KE, SEGCP_RI, SEGCP_LI, SEGCP_SM, SEGCP_GW, SEGCP_DS, SEGCP_PI, SEGCP_PP, SEGCP_DX,
              SEGCP_DP, SEGCP_DI, SEGCP_DW, SEGCP_DH, SEGCP_LP, SEGCP_RP, SEGCP_RH, SEGCP_BR, SEGCP_DB, SEGCP_PR,
              SEGCP_SB, SEGCP_FL, SEGCP_IT, SEGCP_PT, SEGCP_PS, SEGCP_PD, SEGCP_TE, SEGCP_SS, SEGCP_NP, SEGCP_SP,
              SEGCP_LG, SEGCP_ER, SEGCP_FW, SEGCP_MA, SEGCP_PW, SEGCP_SV, SEGCP_EX, SEGCP_RT, SEGCP_UN, SEGCP_ST,
              SEGCP_FR, SEGCP_EC, SEGCP_K1, SEGCP_UE, SEGCP_GA, SEGCP_GB, SEGCP_GC, SEGCP_GD, SEGCP_CA, SEGCP_CB,
              SEGCP_CC, SEGCP_CD, SEGCP_SC, SEGCP_S0, SEGCP_S1, SEGCP_RX, SEGCP_FS, SEGCP_FC, SEGCP_FP, SEGCP_FD,
              SEGCP_FH, SEGCP_UI, SEGCP_QS, SEGCP_QO, SEGCP_QH, SEGCP_QP, SEGCP_QL, SEGCP_RV, SEGCP_RA, SEGCP_RS,
              SEGCP_RE, SEGCP_RR, SEGCP_EN, SEGCP_EI, SEGCP_EB, SEGCP_ED, SEGCP_EP, SEGCP_ES, SEGCP_EF, SEGCP_E0,
              SEGCP_E1, SEGCP_NT, SEGCP_NS, SEGCP_ND, SEGCP_CR, SEGCP_NR, SEGCP_AB, SEGCP_TR, SEGCP_BU, SEGCP_LF,
              SEGCP_AE, SEGCP_AP, SEGCP_MB, SEGCP_SE, SEGCP_CE, SEGCP_CT, SEGCP_N0, SEGCP_N1, SEGCP_N2, SEGCP_AL,
              SEGCP_GR, SEGCP_AM, SEGCP_QF, SEGCP_MM, SEGCP_CS, SEGCP_CM, SEGCP_C0, SEGCP_C1, SEGCP_C2, SEGCP_C3,
              SEGCP_UNKNOWN=255
} teSEGCPCMDNUM;
#else
// Command [K1] : Hidden command, This command erase the configutation data in flash / or EEPROM
// S2E SSL from SEGCP_RC
typedef enum {SEGCP_MC, SEGCP_VR, SEGCP_MN, SEGCP_IM, SEGCP_OP, SEGCP_CP, SEGCP_DG, SEGCP_KA, SEGCP_KI, SEGCP_KE,
              SEGCP_RI, SEGCP_LI, SEGCP_SM, SEGCP_GW, SEGCP_DS, SEGCP_DH, SEGCP_LP, SEGCP_RP, SEGCP_RH, SEGCP_BR,
              SEGCP_DB, SEGCP_PR, SEGCP_SB, SEGCP_FL, SEGCP_PO, SEGCP_IT, SEGCP_PT, SEGCP_PS, SEGCP_PD, SEGCP_TE,
              SEGCP_SS, SEGCP_NP, SEGCP_SP, SEGCP_MA, SEGCP_PW, SEGCP_SV, SEGCP_EX, SEGCP_RT, SEGCP_UN, SEGCP_ST,
              SEGCP_FR, SEGCP_EC, SEGCP_GA, SEGCP_GB, SEGCP_GC, SEGCP_GD, SEGCP_CA, SEGCP_CB, SEGCP_CC, SEGCP_CD,
              SEGCP_SC, SEGCP_S0, SEGCP_S1, SEGCP_RX, SEGCP_UI, SEGCP_TR, SEGCP_QU, SEGCP_QP, SEGCP_QC, SEGCP_QK,
              SEGCP_PU, SEGCP_U0, SEGCP_U1, SEGCP_U2, SEGCP_QO, SEGCP_RC, SEGCP_CE, SEGCP_OC, SEGCP_LC, SEGCP_PK,
              SEGCP_UF, SEGCP_FW, SEGCP_SO, SEGCP_SD, SEGCP_DD, SEGCP_SE, SEGCP_UNKNOWN=255

              
} teSEGCPCMDNUM;
   


#if 0
typedef enum {SEGCP_MC, SEGCP_VR, SEGCP_MN, SEGCP_IM, SEGCP_OP, SEGCP_DD, SEGCP_CP, SEGCP_PO, SEGCP_DG, SEGCP_KA,
              SEGCP_KI, SEGCP_KE, SEGCP_RI, SEGCP_LI, SEGCP_SM, SEGCP_GW, SEGCP_DS, SEGCP_PI, SEGCP_PP, SEGCP_DX,
              SEGCP_DP, SEGCP_DI, SEGCP_DW, SEGCP_DH, SEGCP_LP, SEGCP_RP, SEGCP_RH, SEGCP_BR, SEGCP_DB, SEGCP_PR, 
              SEGCP_SB, SEGCP_FL, SEGCP_IT, SEGCP_PT, SEGCP_PS, SEGCP_PD, SEGCP_TE, SEGCP_SS, SEGCP_NP, SEGCP_SP, 
              SEGCP_LG, SEGCP_ER, SEGCP_FW, SEGCP_MA, SEGCP_PW, SEGCP_SV, SEGCP_EX, SEGCP_RT, SEGCP_UN, SEGCP_ST, 
              SEGCP_FR, SEGCP_EC, SEGCP_K1, SEGCP_UE, SEGCP_GA, SEGCP_GB, SEGCP_GC, SEGCP_GD, SEGCP_CA, SEGCP_CB,
              SEGCP_CC, SEGCP_CD, SEGCP_SC, SEGCP_S0, SEGCP_S1, SEGCP_RX, SEGCP_FS, SEGCP_FC, SEGCP_FP, SEGCP_FD,
              SEGCP_FH, SEGCP_UI, SEGCP_AB, SEGCP_TR, SEGCP_BU, SEGCP_LF, SEGCP_AE, SEGCP_AP, SEGCP_MB, SEGCP_SE,
              SEGCP_CE, SEGCP_CT, SEGCP_N0, SEGCP_N1, SEGCP_N2, SEGCP_AL, SEGCP_GR, SEGCP_AM, SEGCP_CS, SEGCP_CM,
              SEGCP_C0, SEGCP_C1, SEGCP_C2, SEGCP_C3, SEGCP_UNKNOWN=255
} teSEGCPCMDNUM;
#endif
#endif

#define SEGCP_ER_NULL         0
#define SEGCP_ER_NOTAVAIL     1
#define SEGCP_ER_NOPARAM      2
#define SEGCP_ER_IGNORED      3
#define SEGCP_ER_NOCOMMAND    4
#define SEGCP_ER_INVALIDPARAM 5
#define SEGCP_ER_NOPRIVILEGE  6


#define SEGCP_RET_ERR               0x8000
#define SEGCP_RET_ERR_NOTAVAIL      (SEGCP_RET_ERR | (SEGCP_ER_NOTAVAIL << 8))
#define SEGCP_RET_ERR_NOPARAM       (SEGCP_RET_ERR | (SEGCP_ER_NOPARAM << 8))
#define SEGCP_RET_ERR_IGNORED       (SEGCP_RET_ERR | (SEGCP_ER_IGNORED << 8))
#define SEGCP_RET_ERR_NOCOMMAND     (SEGCP_RET_ERR | (SEGCP_ER_NOCOMMAND << 8))
#define SEGCP_RET_ERR_INVALIDPARAM  (SEGCP_RET_ERR | (SEGCP_ER_INVALIDPARAM << 8))
#define SEGCP_RET_ERR_NOPRIVILEGE   (SEGCP_RET_ERR | (SEGCP_ER_NOPRIVILEGE << 8))
#define SEGCP_RET_REBOOT            0x0080
#define SEGCP_RET_FWUP              0x0040
#define SEGCP_RET_FWUP_SERVER       0x0041
#define SEGCP_RET_FWUP_APPBOOT      0x0042
#define SEGCP_RET_FWUP_BANK         0x0043
#define SEGCP_RET_SWITCH            0x0020
#define SEGCP_RET_SAVE              0x0010
#define SEGCP_RET_FACTORY           0x0008

#define SEGCP_RET_ERASE_EEPROM      0x1000

#define SEGCP_NULL      ' '

#define SEGCP_DISABLE   0
#define SEGCP_ENABLE    1

#define SEGCP_STATIC    0
#define SEGCP_DHCP      1

#define SEGCP_RAW       0
#define SEGCP_TELNET    1

#define SEGCP_300       baud_300
#define SEGCP_600       baud_600
#define SEGCP_1200      baud_1200
#define SEGCP_1800      baud_1800
#define SEGCP_2400      baud_2400
#define SEGCP_4800      baud_4800
#define SEGCP_9600      baud_9600
#define SEGCP_14400     baud_14400
#define SEGCP_19200     baud_19200
#define SEGCP_28800     baud_28800
#define SEGCP_38400     baud_38400
#define SEGCP_57600     baud_57600
#define SEGCP_115200    baud_115200
#define SEGCP_230400    baud_230400
#define SEGCP_460800    baud_460800
#define SEGCP_921600    baud_921600

#define SEGCP_DTBIT7    word_len7
#define SEGCP_DTBIT8    word_len8

#define SEGCP_NONE      parity_none
#define SEGCP_ODD       parity_odd
#define SEGCP_EVEN      parity_even

#define SEGCP_STBIT1    stop_bit1
#define SEGCP_STBIT2    stop_bit2

#define SEGCP_XONOFF    flow_xon_xoff
#define SEGCP_RTSCTS    flow_rts_cts

#define SEGCP_PRIVILEGE_SET   0x80
#define SEGCP_PRIVILEGE_CLR   0x00
#define SEGCP_PRIVILEGE_READ  0x00
#define SEGCP_PRIVILEGE_WRITE 0x08

#define CONFIGTOOL_KEEPALIVE_TIME_MS	15000 // unit:ms, used by TCP unicast search function only.

void do_segcp_tcp(void);
void do_segcp_udp(void);
void do_segcp_uart(void);
void segcp_ret_handler(uint16_t segcp_ret);
void set_segcp_uart(uint8_t uartNum);
uint8_t get_segcp_uart(void);
uint8_t parse_SEGCP(uint8_t * pmsg, uint8_t * param);
uint16_t proc_SEGCP(uint8_t* segcp_req, uint8_t* segcp_rep, uint8_t segcp_privilege);
uint16_t proc_SEGCP_tcp(uint8_t * segcp_req, uint8_t * segcp_rep);
uint16_t proc_SEGCP_udp(uint8_t * segcp_req, uint8_t * segcp_rep);
uint16_t proc_SEGCP_uart(uint8_t * segcp_req, uint8_t * segcp_rep);
void segcp_timer_msec(void); // for timer
void segcp_tcp_task(void *argument);
void segcp_udp_task(void *argument);
void segcp_uart_task(void *argument);
#endif
