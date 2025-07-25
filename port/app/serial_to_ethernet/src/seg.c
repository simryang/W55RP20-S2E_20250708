#include <stdlib.h>
#include <string.h>
#include "common.h"
#include "wizchip_conf.h"
#include "w5x00_spi.h"
#include "WIZnet_board.h"
#include "socket.h"

#include "mqtt_transport_interface.h"
#include "seg.h"
#include "deviceHandler.h"
#include "timerHandler.h"
#include "uartHandler.h"
#include "gpioHandler.h"
#include "ConfigData.h"


//#include <semphr.h>

#include "port_common.h"

// TLS support
#ifdef __USE_S2E_OVER_TLS__
#include "SSLInterface.h"
wiz_tls_context s2e_tlsContext;
#endif

#include "mqtt_transport_interface.h"

#include "netHandler.h"

//Modbus supprot
#include "mb.h"
#include "mbrtu.h"
#include "mbascii.h"
#include "mbserial.h"

/* Private define ------------------------------------------------------------*/
// Ring Buffer
BUFFER_DECLARATION(data0_rx);

/* Private variables ---------------------------------------------------------*/
uint8_t opmode = DEVICE_GW_MODE;
uint8_t sw_modeswitch_at_mode_on = SEG_DISABLE;

// static variables for function: check_modeswitch_trigger()
static uint8_t triggercode_idx;
static uint8_t ch_tmp[3];

// Gateway mode <-> command mode switch gap time
uint8_t enable_modeswitch_timer = SEG_DISABLE;
volatile uint16_t modeswitch_time = 0;
volatile uint16_t modeswitch_gap_time = DEFAULT_MODESWITCH_INTER_GAP;

static uint8_t mixed_state = MIXED_SERVER;
static uint16_t client_any_port = 0;

uint8_t enable_serial_input_timer = SEG_DISABLE;
volatile uint16_t serial_input_time = 0;
uint8_t flag_serial_input_time_elapse = SEG_DISABLE; // for Time delimiter

// flags
uint8_t flag_connect_pw_auth = SEG_DISABLE; // TCP_SERVER_MODE only
uint8_t flag_auth_time = SEG_DISABLE; // TCP_SERVER_MODE only
uint8_t flag_send_keepalive = SEG_DISABLE;
uint8_t flag_first_keepalive = SEG_DISABLE;
uint8_t flag_inactivity = SEG_DISABLE;

// User's buffer / size idx
extern uint8_t g_send_buf[DATA_BUF_SIZE];
extern uint8_t g_recv_buf[DATA_BUF_SIZE];
extern uint8_t g_recv_mqtt_buf[DATA_BUF_SIZE];

/*the flag of modbus*/
extern volatile uint8_t mb_state_rtu_finish;
extern volatile uint8_t mb_state_ascii_finish;

extern xSemaphoreHandle seg_e2u_sem;
extern xSemaphoreHandle seg_u2e_sem;
extern xSemaphoreHandle seg_sem;
extern xSemaphoreHandle net_seg_sem;
extern xSemaphoreHandle seg_timer_sem;
extern xSemaphoreHandle segcp_uart_sem;
extern xSemaphoreHandle seg_socket_sem;

extern TimerHandle_t seg_inactivity_timer;
extern TimerHandle_t seg_keepalive_timer;
extern TimerHandle_t seg_auth_timer;

uint16_t u2e_size = 0;
uint16_t e2u_size = 0;

// UDP: Peer netinfo
uint8_t peerip[4] = {0, };
uint8_t peerip_tmp[4] = {0xff, };
uint16_t peerport = 0;

// XON/XOFF (Software flow control) flag, Serial data can be transmitted to peer when XON enabled.
uint8_t isXON = SEG_ENABLE;

char * str_working[] = {"TCP_CLIENT_MODE", "TCP_SERVER_MODE", "TCP_MIXED_MODE", "UDP_MODE", "SSL_TCP_CLIENT_MODE", "MQTT_CLIENT_MODE", "MQTTS_CLIENT_MODE"};

//Network mqtt_n;
//MQTTClient mqtt_c = DefaultClient;
//MQTTPacket_connectData mqtt_data = MQTTPacket_connectData_initializer;
NetworkContext_t g_network_context;
TransportInterface_t g_transport_interface;
mqtt_config_t g_mqtt_config;

/* Private functions prototypes ----------------------------------------------*/
void proc_SEG_tcp_client(uint8_t sock);
void proc_SEG_tcp_server(uint8_t sock);
void proc_SEG_tcp_mixed(uint8_t sock);
void proc_SEG_udp(uint8_t sock);
void proc_SEG_mqtt_client(uint8_t sock);
void proc_SEG_mqtts_client(uint8_t sock);

#ifdef __USE_S2E_OVER_TLS__
void proc_SEG_tcp_client_over_tls(uint8_t sock);
#endif

void uart_to_ether(uint8_t sock);
void ether_to_uart(uint8_t sock);
uint16_t get_serial_data(void);
void restore_serial_data(uint8_t idx);

uint8_t check_connect_pw_auth(uint8_t * buf, uint16_t len);
uint8_t check_tcp_connect_exception(void);
void reset_SEG_timeflags(void);

uint16_t get_tcp_any_port(void);

/* Public & Private functions ------------------------------------------------*/

void do_seg(uint8_t sock)
{
    struct __network_connection *network_connection = (struct __network_connection *)&(get_DevConfig_pointer()->network_connection);
    struct __serial_option *serial_option = (struct __serial_option *)&(get_DevConfig_pointer()->serial_option);
    
    if(opmode == DEVICE_GW_MODE)
    {
        switch(network_connection->working_mode)
        {
            case TCP_CLIENT_MODE:
                proc_SEG_tcp_client(sock);
                break;
                
            case TCP_SERVER_MODE:
                proc_SEG_tcp_server(sock);
                break;
            
            case TCP_MIXED_MODE:
                proc_SEG_tcp_mixed(sock);
                break;
            
            case UDP_MODE:
                proc_SEG_udp(sock);
                break;
            
#ifdef __USE_S2E_OVER_TLS__
            case SSL_TCP_CLIENT_MODE:
                proc_SEG_tcp_client_over_tls(sock);
                break;
#endif

            case MQTT_CLIENT_MODE:
                proc_SEG_mqtt_client(sock);
                break;

#ifdef __USE_S2E_OVER_TLS__                
            case MQTTS_CLIENT_MODE:
                proc_SEG_mqtts_client(sock);
                break;
#endif

            default:
                break;
        }
        
        // XON/XOFF Software flow control: Check the Buffer usage and Send the start/stop commands
        // [WIZnet Device] -> [Peer]
        if((serial_option->flow_control == flow_xon_xoff) || (serial_option->flow_control == flow_rts_cts))
        {
            check_uart_flow_control(serial_option->flow_control);
        }
    }
}

void set_device_status(teDEVSTATUS status)
{
    struct __network_connection *network_connection = (struct __network_connection *)&(get_DevConfig_pointer()->network_connection);
    struct __device_option *device_option = (struct __device_option *)&(get_DevConfig_pointer()->device_option);
    uint8_t prev_working_state = network_connection->working_state;

    switch(status)
    {
        case ST_BOOT:       // Boot Mode
            network_connection->working_state = ST_BOOT;
            break;

        case ST_OPEN:       // TCP connection state: disconnected (or UDP mode)
            network_connection->working_state = ST_OPEN;
            break;
        
        case ST_CONNECT:    // TCP connection state: connected
            network_connection->working_state = ST_CONNECT;
            break;
        
        case ST_UPGRADE:    // TCP connection state: disconnected
            network_connection->working_state = ST_UPGRADE;
            break;
        
        case ST_ATMODE:     // TCP connection state: disconnected
            network_connection->working_state = ST_ATMODE;
            break;
        
        case ST_UDP:        // UDP mode
            network_connection->working_state = ST_UDP;
        default:
            break;
    }
    
    if(network_connection->working_state == ST_CONNECT) {
        if (device_option->device_eth_connect_data[0] != 0) {
            struct __mqtt_option *mqtt_option = (struct __mqtt_option *)&(get_DevConfig_pointer()->mqtt_option);
            struct __tcp_option *tcp_option = (struct __tcp_option *)&(get_DevConfig_pointer()->tcp_option);

            if (network_connection->working_mode == MQTT_CLIENT_MODE || network_connection->working_mode == MQTTS_CLIENT_MODE)
                wizchip_mqtt_publish(&g_mqtt_config, mqtt_option->pub_topic, mqtt_option->qos, device_option->device_eth_connect_data, strlen((char *)device_option->device_eth_connect_data));
#ifdef __USE_S2E_OVER_TLS__
            else if (network_connection->working_mode == SSL_TCP_CLIENT_MODE)
                wiz_tls_write(&s2e_tlsContext, device_option->device_eth_connect_data, strlen((char *)device_option->device_eth_connect_data));
#endif
            else
                (int16_t)send(SEG_DATA0_SOCK, device_option->device_eth_connect_data, strlen((char *)device_option->device_eth_connect_data));

            if(tcp_option->keepalive_en == ENABLE && flag_first_keepalive == DISABLE)
            {
                flag_first_keepalive = ENABLE;
                xTimerStart(seg_keepalive_timer, 0);
            }
        }
        
        if (device_option->device_serial_connect_data[0] != 0)
            platform_uart_puts((const char *)device_option->device_serial_connect_data, strlen((const char *)device_option->device_serial_connect_data));

        // Status indicator pins
        set_connection_status_io(STATUS_TCPCONNECT_PIN, ON); // Status I/O pin to low
    }

    else if (prev_working_state == ST_CONNECT && network_connection->working_state == ST_OPEN) {
        if (device_option->device_serial_disconnect_data[0] != 0)
            platform_uart_puts((const char *)device_option->device_serial_disconnect_data, strlen((const char *)device_option->device_serial_disconnect_data));
        // Status indicator pins
        set_connection_status_io(STATUS_TCPCONNECT_PIN, OFF); // Status I/O pin to low
    }
    else
        set_connection_status_io(STATUS_TCPCONNECT_PIN, OFF); // Status I/O pin to high
}

uint8_t get_device_status(void)
{
    struct __network_connection *network_connection = (struct __network_connection *)&(get_DevConfig_pointer()->network_connection);
    return network_connection->working_state;
}


void proc_SEG_udp(uint8_t sock)
{
    struct __network_connection *network_connection = (struct __network_connection *)&(get_DevConfig_pointer()->network_connection);
    struct __serial_common *serial_common = (struct __serial_common *)&(get_DevConfig_pointer()->serial_common);
    struct __serial_data_packing *serial_data_packing = (struct __serial_data_packing *)&(get_DevConfig_pointer()->serial_data_packing);

    // Serial communication mode
    uint8_t serial_mode = get_serial_communation_protocol();

    // Socket state
    uint8_t state = getSn_SR(sock);

    switch(state)
    {
        case SOCK_UDP:
            break;
            
        case SOCK_CLOSED:
            if(serial_mode == SEG_SERIAL_PROTOCOL_NONE)
            {
                // UART Ring buffer clear
                uart_rx_flush();
            }
            
            u2e_size = 0;
            e2u_size = 0;

            xSemaphoreTake(seg_socket_sem, portMAX_DELAY);
            int8_t s = socket(sock, Sn_MR_UDP, network_connection->local_port, 0);
            xSemaphoreGive(seg_socket_sem);

            if(s == sock)
            {
                set_device_status(ST_UDP);
                
                if(serial_data_packing->packing_time)
                {
                    modeswitch_gap_time = serial_data_packing->packing_time; // replace the GAP time (default: 500ms)
                }
                
                if(serial_common->serial_debug_en)
                {
                    PRT_SEG(" > SEG:UDP_MODE:SOCKOPEN\r\n");
                }
            }
            break;
        default:
            break;
    }
}

void proc_SEG_tcp_client(uint8_t sock)
{
    struct __tcp_option *tcp_option = (struct __tcp_option *)&(get_DevConfig_pointer()->tcp_option);
    struct __network_connection *network_connection = (struct __network_connection *)&(get_DevConfig_pointer()->network_connection);
    struct __serial_data_packing *serial_data_packing = (struct __serial_data_packing *)&(get_DevConfig_pointer()->serial_data_packing);
    struct __serial_common *serial_common = (struct __serial_common *)&(get_DevConfig_pointer()->serial_common);
    struct __serial_command *serial_command = (struct __serial_command *)&(get_DevConfig_pointer()->serial_command);

    uint16_t source_port;
    uint8_t destip[4] = {0, };
    uint16_t destport = 0;
    
    // Serial communication mode
    uint8_t serial_mode = get_serial_communation_protocol();

    // Socket state
    uint8_t state = getSn_SR(sock);
    uint16_t reg_val;

    switch(state)
    {
        case SOCK_INIT:
            if (tcp_option->reconnection) vTaskDelay(tcp_option->reconnection);
            // TCP connect exception checker; e.g., dns failed / zero srcip ... and etc.
            if(check_tcp_connect_exception() == ON) return;
            // TCP connect
            connect(sock, network_connection->remote_ip, network_connection->remote_port);
#ifdef _SEG_DEBUG_
            PRT_SEG(" > SEG:TCP_CLIENT_MODE:CLIENT_CONNECTION\r\n");
#endif
            break;
        
        case SOCK_ESTABLISHED:
            if(getSn_IR(sock) & Sn_IR_CON)
            {
                ///////////////////////////////////////////////////////////////////////////////////////////////////
                // S2E: TCP client mode initialize after connection established (only once)
                ///////////////////////////////////////////////////////////////////////////////////////////////////                
                // Interrupt clear
                reg_val = SIK_CONNECTED & 0x00FF; // except SIK_SENT(send OK) interrupt
                ctlsocket(sock, CS_CLR_INTERRUPT, (void *)&reg_val);
                
                // Serial debug message printout
                if(serial_common->serial_debug_en)
                {
                    getsockopt(sock, SO_DESTIP, &destip);
                    getsockopt(sock, SO_DESTPORT, &destport);
                    PRT_SEG(" > SEG:CONNECTED TO - %d.%d.%d.%d : %d\r\n", destip[0], destip[1], destip[2], destip[3], destport);
                }
                
                if(serial_mode == SEG_SERIAL_PROTOCOL_NONE)                    
                    uart_rx_flush(); // UART Ring buffer clear
                
                if(tcp_option->inactivity) {
                    flag_inactivity = SEG_DISABLE;
                    if (seg_inactivity_timer == NULL) 
                        seg_inactivity_timer = xTimerCreate("seg_inactivity_timer", pdMS_TO_TICKS(tcp_option->inactivity * 1000), pdFALSE, 0, inactivity_timer_callback);
                    xTimerStart(seg_inactivity_timer, 0);
                }
                
                if(tcp_option->keepalive_en) {
                    if (seg_keepalive_timer == NULL)
                        seg_keepalive_timer = xTimerCreate("seg_keepalive_timer", pdMS_TO_TICKS(tcp_option->keepalive_wait_time), pdFALSE, 0, keepalive_timer_callback);
                    else {
                        if (xTimerIsTimerActive(seg_keepalive_timer) == pdTRUE)
                            xTimerStop(seg_keepalive_timer, 0);
                        xTimerChangePeriod(seg_keepalive_timer, pdMS_TO_TICKS(tcp_option->keepalive_wait_time), 0);
                    }
                }
                set_device_status(ST_CONNECT);
            }
            break;
        
        case SOCK_CLOSE_WAIT:
            if(serial_mode == SEG_SERIAL_PROTOCOL_NONE)
            {
                while(getSn_RX_RSR(sock) || e2u_size)
                {
                    ether_to_uart(sock); // receive remaining packets
                }
            }
            //process_socket_termination(sock, SOCK_TERMINATION_DELAY);
            disconnect(sock);
            break;
        
        case SOCK_FIN_WAIT:
        case SOCK_CLOSED:
            set_device_status(ST_OPEN);
            process_socket_termination(sock, SOCK_TERMINATION_DELAY);

            u2e_size = 0;
            e2u_size = 0;
            
            if(network_connection->fixed_local_port)
            {
                source_port = network_connection->local_port;
            }
            else
            {
                source_port = get_tcp_any_port();
            }


#ifdef _SEG_DEBUG_
            PRT_SEG(" > TCP CLIENT: client_any_port = %d\r\n", client_any_port);
#endif
            xSemaphoreTake(seg_socket_sem, portMAX_DELAY);
            int8_t s = socket(sock, Sn_MR_TCP, source_port, (SF_TCP_NODELAY | SF_IO_NONBLOCK));
            xSemaphoreGive(seg_socket_sem);
            
            if(s == sock)
            {
                if((serial_command->serial_command == SEG_ENABLE) && serial_data_packing->packing_time)
                    modeswitch_gap_time = serial_data_packing->packing_time;
                
                if(serial_common->serial_debug_en)
                    PRT_SEG(" > SEG:TCP_CLIENT_MODE:SOCKOPEN\r\n");

            }
            break;
            
        default:
            break;
    }
}

#ifdef __USE_S2E_OVER_TLS__
void proc_SEG_tcp_client_over_tls(uint8_t sock)
{
    struct __tcp_option *tcp_option = (struct __tcp_option *)&(get_DevConfig_pointer()->tcp_option);
    struct __network_connection *network_connection = (struct __network_connection *)&(get_DevConfig_pointer()->network_connection);
    struct __serial_data_packing *serial_data_packing = (struct __serial_data_packing *)&(get_DevConfig_pointer()->serial_data_packing);
    struct __serial_common *serial_common = (struct __serial_common *)&get_DevConfig_pointer()->serial_common;
    struct __serial_command *serial_command = (struct __serial_command *)&get_DevConfig_pointer()->serial_command;

    uint16_t source_port;
    uint8_t destip[4] = {0, };
    uint16_t destport = 0;

    // Serial communication mode
    uint8_t serial_mode = get_serial_communation_protocol();

    // Socket state
    uint8_t state = getSn_SR(sock);

    int ret = 0;
    uint16_t reg_val;
    static uint8_t first_established;

    switch(state)
    {
        case SOCK_INIT:
            if (tcp_option->reconnection) vTaskDelay(tcp_option->reconnection);
            // TCP connect exception checker; e.g., dns failed / zero srcip ... and etc.
            if(check_tcp_connect_exception() == ON) return;
            
            reg_val = 0;
            ctlsocket(sock, CS_SET_INTMASK, (void *)&reg_val);
            
            ret = connect(sock, network_connection->remote_ip, network_connection->remote_port);
            if (ret < 0)
            {
                PRT_SEG(" > SEG:TCP_CLIENT_OVER_TLS_MODE:ConnectNetwork Err %d\r\n", ret);
                process_socket_termination(sock, SOCK_TERMINATION_DELAY);
                break;
            }

            reg_val = SIK_CONNECTED & 0x00FF;
            ctlsocket(sock, CS_CLR_INTERRUPT, (void *)&reg_val);
            
            ret = wiz_tls_connect(&s2e_tlsContext,
                                  (char *)network_connection->remote_ip,
                                  (unsigned int)network_connection->remote_port);

#if 1
            reg_val = SIK_RECEIVED & 0x00FF;
            ctlsocket(sock, CS_CLR_INTERRUPT, (void *)&reg_val);

            reg_val =  SIK_RECEIVED & 0x00FF; // except SIK_SENT(send OK) interrupt
            ctlsocket(sock, CS_SET_INTMASK, (void *)&reg_val);

            ctlwizchip(CW_GET_INTRMASK, (void *)&reg_val);
#if (_WIZCHIP_ == W5100S)
            reg_val = (1 << sock);
#elif (_WIZCHIP_ == W5500)
            reg_val = ((1 << sock) << 8) | reg_val;
#endif
            ctlwizchip(CW_SET_INTRMASK, (void *)&reg_val);
#endif

            if(ret != 0) // TLS connection failed
            {
                process_socket_termination(sock, SOCK_TERMINATION_DELAY);
                if(serial_common->serial_debug_en)
                    PRT_SEG(" > SEG:TCP_CLIENT_OVER_TLS_MODE: CONNECTION FAILED\r\n");
                break;
            }
            first_established = 1;
            
            PRT_SEG(" > SEG:TCP_CLIENT_OVER_TLS_MODE: TCP CLIENT CONNECTED\r\n");
            break;

        case SOCK_ESTABLISHED:
            //if(getSn_IR(sock) & Sn_IR_CON)
            if(first_established)
            {
                ///////////////////////////////////////////////////////////////////////////////////////////////////
                // S2E: TCP client mode initialize after connection established (only once)
                ///////////////////////////////////////////////////////////////////////////////////////////////////

                // Serial debug message printout
                if(serial_common->serial_debug_en)
                {
                    getsockopt(sock, SO_DESTIP, &destip);
                    getsockopt(sock, SO_DESTPORT, &destport);
                    PRT_SEG(" > SEG:CONNECTED TO - %d.%d.%d.%d : %d\r\n", destip[0], destip[1], destip[2], destip[3], destport);
                }

                if(serial_mode == SEG_SERIAL_PROTOCOL_NONE)
                    uart_rx_flush(); // UART Ring buffer clear

                if(tcp_option->inactivity) {
                  flag_inactivity = SEG_DISABLE;
                  if (seg_inactivity_timer == NULL) 
                      seg_inactivity_timer = xTimerCreate("seg_inactivity_timer", pdMS_TO_TICKS(tcp_option->inactivity * 1000), pdFALSE, 0, inactivity_timer_callback);
                  xTimerStart(seg_inactivity_timer, 0);
                }

                if(tcp_option->keepalive_en) {
                    if (seg_keepalive_timer == NULL)
                        seg_keepalive_timer = xTimerCreate("seg_keepalive_timer", pdMS_TO_TICKS(tcp_option->keepalive_wait_time), pdFALSE, 0, keepalive_timer_callback);
                    else {
                        if (xTimerIsTimerActive(seg_keepalive_timer) == pdTRUE)
                            xTimerStop(seg_keepalive_timer, 0);
                        xTimerChangePeriod(seg_keepalive_timer, pdMS_TO_TICKS(tcp_option->keepalive_wait_time), 0);
                    }
                }
                first_established = 0;
                set_device_status(ST_CONNECT);
            }
            break;

        case SOCK_CLOSE_WAIT:
            if(serial_mode == SEG_SERIAL_PROTOCOL_NONE)
            {
                while(getSn_RX_RSR(sock) || e2u_size)
                    ether_to_uart(sock); // receive remaining packets
            }
            //process_socket_termination(sock, SOCK_TERMINATION_DELAY); // including disconnect(sock) function
            disconnect(sock);
            break;

        case SOCK_FIN_WAIT:
        case SOCK_CLOSED:
            process_socket_termination(sock, SOCK_TERMINATION_DELAY);
            if(get_wiz_tls_init_state() == ENABLE) {
                wiz_tls_deinit(&s2e_tlsContext);
                set_wiz_tls_init_state(DISABLE);
            }

            if(wiz_tls_init(&s2e_tlsContext, (int *)sock) > 0)
            {
                set_device_status(ST_OPEN);

                u2e_size = 0;
                e2u_size = 0;

                if(network_connection->fixed_local_port)
                    source_port = network_connection->local_port;
                else
                    source_port = get_tcp_any_port();

                PRT_SEG(" > TCP CLIENT over TLS: client_any_port = %d\r\n", client_any_port);
                if(wiz_tls_socket(&s2e_tlsContext, sock, source_port) == sock)
                {
                    // Replace the command mode switch code GAP time (default: 500ms)
                    if((serial_command->serial_command == SEG_ENABLE) && serial_data_packing->packing_time)
                        modeswitch_gap_time = serial_data_packing->packing_time;

                    if(serial_common->serial_debug_en)
                        PRT_SEG(" > SEG:TCP_CLIENT_OVER_TLS_MODE:SOCKOPEN\r\n");
                    set_wiz_tls_init_state(ENABLE);
                }
                else
                {
                    PRT_SEG("wiz_tls_socket() failed\r\n");
                    if(get_wiz_tls_init_state() == ENABLE)
                    {
                        wiz_tls_deinit(&s2e_tlsContext);
                        set_wiz_tls_init_state(DISABLE);
                    }
                }
            }
            else
            {
                PRT_SEG("wiz_tls_init() failed\r\n");
                wiz_tls_deinit(&s2e_tlsContext);
                set_wiz_tls_init_state(DISABLE);
            }
            break;

        default:
            break;
    }
}

#endif


void proc_SEG_mqtt_client(uint8_t sock)
{
    struct __tcp_option *tcp_option = (struct __tcp_option *)&(get_DevConfig_pointer()->tcp_option);
    struct __network_connection *network_connection = (struct __network_connection *)&(get_DevConfig_pointer()->network_connection);
    struct __serial_data_packing *serial_data_packing = (struct __serial_data_packing *)&(get_DevConfig_pointer()->serial_data_packing);
    struct __serial_common *serial_common = (struct __serial_common *)&get_DevConfig_pointer()->serial_common;
    struct __serial_command *serial_command = (struct __serial_command *)&get_DevConfig_pointer()->serial_command;
    struct __mqtt_option *mqtt_option = (struct __mqtt_option *)&(get_DevConfig_pointer()->mqtt_option);

    uint16_t source_port;
    uint8_t destip[4] = {0, };
    uint16_t destport = 0;
    
    uint8_t serial_mode = get_serial_communation_protocol();
    uint8_t state = getSn_SR(sock);
    int ret;
    uint16_t reg_val;
    static uint8_t first_established;

    switch(state)
    {
        case SOCK_INIT:
            if (tcp_option->reconnection) vTaskDelay(tcp_option->reconnection);

            // MQTT connect exception checker; e.g., dns failed / zero srcip ... and etc.
            if(check_tcp_connect_exception() == ON) return;

            reg_val = 0;
            ctlsocket(sock, CS_SET_INTMASK, (void *)&reg_val);

            // MQTT connect
            ret = connect(sock, network_connection->remote_ip, network_connection->remote_port);
            if (ret < 0)
            {
                PRT_SEG(" > SEG:MQTT_CLIENT_MODE:ConnectNetwork Err %d\r\n", ret);
                process_socket_termination(sock, SOCK_TERMINATION_DELAY);
                break;
            }
            PRT_SEG(" > SEG:MQTT_CLIENT_MODE:TCP_CONNECTION\r\n");

            reg_val = SIK_ALL & 0x00FF;
            ctlsocket(sock, CS_CLR_INTERRUPT, (void *)&reg_val);

            ret = mqtt_transport_connect(&g_mqtt_config, tcp_option->reconnection);
            if (ret < 0)
            {
                PRT_SEG(" > SEG:MQTT_CLIENT_MODE:MQTTConnect Err %d\r\n", ret);
                process_socket_termination(sock, SOCK_TERMINATION_DELAY);
                break;
            }
            PRT_SEG(" > SEG:MQTT_CLIENT_MODE:MQTT_CONNECTION\r\n");
          
            if (mqtt_option->sub_topic_0[0] != 0 && mqtt_option->sub_topic_0[0] != 0xFF)
            {
                ret = mqtt_transport_subscribe(&g_mqtt_config, mqtt_option->qos, (char *)mqtt_option->sub_topic_0);
                if (ret < 0)
                {
                  PRT_SEG(" > SEG:MQTT_CLIENT_MODE:MQTTSubscribe Err %d\r\n", ret);
                  process_socket_termination(sock, SOCK_TERMINATION_DELAY);
                  break;
                }
            }
            if (mqtt_option->sub_topic_1[0] != 0 && mqtt_option->sub_topic_1[0] != 0xFF)
            {
                ret = mqtt_transport_subscribe(&g_mqtt_config, mqtt_option->qos, (char *)mqtt_option->sub_topic_1);
                if (ret < 0)
                {
                  PRT_SEG(" > SEG:MQTT_CLIENT_MODE:MQTTSubscribe Err %d\r\n", ret);
                  process_socket_termination(sock, SOCK_TERMINATION_DELAY);
                  break;
                }
            }
            if (mqtt_option->sub_topic_2[0] != 0 && mqtt_option->sub_topic_2[0] != 0xFF)
            {
                ret = mqtt_transport_subscribe(&g_mqtt_config, mqtt_option->qos, (char *)mqtt_option->sub_topic_2);
                if (ret < 0)
                {
                  PRT_SEG(" > SEG:MQTT_CLIENT_MODE:MQTTSubscribe Err %d\r\n", ret);
                  process_socket_termination(sock, SOCK_TERMINATION_DELAY);
                  break;
                }
            }
            PRT_SEG(" > SEG:MQTT_CLIENT_MODE:MQTTSubscribed\r\n");
            first_established = 1;
            break;
        
        case SOCK_ESTABLISHED:
            if(first_established)
            {
                // Serial debug message printout
                if(serial_common->serial_debug_en)
                {
                    getsockopt(sock, SO_DESTIP, &destip);
                    getsockopt(sock, SO_DESTPORT, &destport);
                    PRT_SEG(" > SEG:CONNECTED TO - %d.%d.%d.%d : %d\r\n", destip[0], destip[1], destip[2], destip[3], destport);
                }
                
                if(serial_mode == SEG_SERIAL_PROTOCOL_NONE)
                    uart_rx_flush(); // UART Ring buffer clear
                    
                if(tcp_option->inactivity) {
                  flag_inactivity = SEG_DISABLE;
                  if (seg_inactivity_timer == NULL) 
                      seg_inactivity_timer = xTimerCreate("seg_inactivity_timer", pdMS_TO_TICKS(tcp_option->inactivity * 1000), pdFALSE, 0, inactivity_timer_callback);
                  xTimerStart(seg_inactivity_timer, 0);
                }

                if(tcp_option->keepalive_en) {
                    if (seg_keepalive_timer == NULL)
                        seg_keepalive_timer = xTimerCreate("seg_keepalive_timer", pdMS_TO_TICKS(tcp_option->keepalive_wait_time), pdFALSE, 0, keepalive_timer_callback);
                    else {
                        if (xTimerIsTimerActive(seg_keepalive_timer) == pdTRUE)
                            xTimerStop(seg_keepalive_timer, 0);
                        xTimerChangePeriod(seg_keepalive_timer, pdMS_TO_TICKS(tcp_option->keepalive_wait_time), 0);
                    }
                }
                first_established = 0;
                set_device_status(ST_CONNECT);
            }
            mqtt_transport_yield(&g_mqtt_config);
            break;
        
        case SOCK_CLOSE_WAIT:
            disconnect(sock);
            mqtt_transport_close(sock, &g_mqtt_config);
            break;
        
        case SOCK_FIN_WAIT:
        case SOCK_CLOSED:
            process_socket_termination(sock, SOCK_TERMINATION_DELAY);
            set_device_status(ST_OPEN);
        
            u2e_size = 0;
            e2u_size = 0;
            
            if(network_connection->fixed_local_port)
                source_port = network_connection->local_port;
            else
                source_port = get_tcp_any_port();

            PRT_SEG(" > MQTT CLIENT: client_any_port = %d\r\n", client_any_port);
            xSemaphoreTake(seg_socket_sem, portMAX_DELAY);
            int8_t s = socket(sock, Sn_MR_TCP, source_port, (SF_TCP_NODELAY | SF_IO_NONBLOCK));
            xSemaphoreGive(seg_socket_sem);

            if(s == sock)
            {
                // Replace the command mode switch code GAP time (default: 500ms)
                if((serial_command->serial_command == SEG_ENABLE) && serial_data_packing->packing_time)
                    modeswitch_gap_time = serial_data_packing->packing_time;
                
                if(serial_common->serial_debug_en)
                    PRT_SEG(" > SEG:MQTT_CLIENT_MODE:SOCKOPEN\r\n");
            }

            ret = mqtt_transport_init(sock, &g_mqtt_config, true, 0, g_recv_mqtt_buf, 
                                      DATA_BUF_SIZE, &g_transport_interface, &g_network_context, 
                                      mqtt_option->client_id, mqtt_option->user_name, mqtt_option->password, mqtt_option->keepalive, mqtt_subscribeMessageHandler);
            if (ret < 0) {
              PRT_SEG(" > SEG:MQTT_CLIENT_MODE:INITIALIZE FAILED\r\n");
              process_socket_termination(sock, SOCK_TERMINATION_DELAY);
            }
            break;
            
        default:
            break;
    }
}

#ifdef __USE_S2E_OVER_TLS__
void proc_SEG_mqtts_client(uint8_t sock)
{
    struct __tcp_option *tcp_option = (struct __tcp_option *)&(get_DevConfig_pointer()->tcp_option);
    struct __network_connection *network_connection = (struct __network_connection *)&(get_DevConfig_pointer()->network_connection);
    struct __serial_data_packing *serial_data_packing = (struct __serial_data_packing *)&(get_DevConfig_pointer()->serial_data_packing);
    struct __serial_common *serial_common = (struct __serial_common *)&get_DevConfig_pointer()->serial_common;
    struct __serial_command *serial_command = (struct __serial_command *)&get_DevConfig_pointer()->serial_command;
    struct __mqtt_option *mqtt_option = (struct __mqtt_option *)&(get_DevConfig_pointer()->mqtt_option);

    uint16_t source_port;
    uint8_t destip[4] = {0, };
    uint16_t destport = 0;
    
    uint8_t serial_mode = get_serial_communation_protocol();
    uint8_t state = getSn_SR(sock);
    int ret;
    uint16_t reg_val;
    static uint8_t first_established;

    switch(state)
    {
        case SOCK_INIT:
            if (tcp_option->reconnection) vTaskDelay(tcp_option->reconnection);
            // MQTT connect exception checker; e.g., dns failed / zero srcip ... and etc.
            if(check_tcp_connect_exception() == ON) return;

            reg_val = 0;
            ctlsocket(sock, CS_SET_INTMASK, (void *)&reg_val);

            ret = connect(sock, network_connection->remote_ip, network_connection->remote_port);
            if (ret < 0)
            {
                PRT_SEG(" > SEG:TCP_CLIENT_OVER_TLS_MODE:ConnectNetwork Err %d\r\n", ret);
                process_socket_termination(sock, SOCK_TERMINATION_DELAY);
                break;
            }
            reg_val = SIK_ALL & 0x00FF;
            ctlsocket(sock, CS_CLR_INTERRUPT, (void *)&reg_val);
            
            PRT_SEG(" > SEG:TCP_CLIENT_OVER_TLS_MODE: TCP CLIENT CONNECTED\r\n");
            
            ret = wiz_tls_connect(&s2e_tlsContext,
                                  (char *)network_connection->remote_ip,
                                  (unsigned int)network_connection->remote_port);
            
            if(ret != 0) // TLS connection failed
            {
                process_socket_termination(sock, SOCK_TERMINATION_DELAY);
                if(serial_common->serial_debug_en)
                    PRT_SEG(" > SEG:MQTTS_CLIENT_MODE: CONNECTION FAILED\r\n");
                break;
            }
            PRT_SEG(" > SEG:MQTTS_CLIENT_MODE: SSL CLIENT CONNECTED\r\n");
            
            // MQTTS connect
            ret = mqtt_transport_connect(&g_mqtt_config, tcp_option->reconnection);
            if (ret < 0)
            {
                PRT_SEG(" > SEG:MQTTS_CLIENT_MODE:ConnectNetwork Err %d\r\n", ret);
                mqtt_transport_close(sock, &g_mqtt_config);
                break;
            }
            
            PRT_SEG(" > SEG:MQTTS_CLIENT_MODE:MQTT_CONNECTION\r\n");

            if (mqtt_option->sub_topic_0[0] != 0 && mqtt_option->sub_topic_0[0] != 0xFF)
            {
                ret = mqtt_transport_subscribe(&g_mqtt_config, mqtt_option->qos, (char *)mqtt_option->sub_topic_0);
                if (ret < 0)
                {
                  PRT_SEG(" > SEG:MQTTS_CLIENT_MODE:MQTTSubscribe Err %d\r\n", ret);
                  mqtt_transport_close(sock, &g_mqtt_config);
                  break;
                }
            }
            if (mqtt_option->sub_topic_1[0] != 0 && mqtt_option->sub_topic_1[0] != 0xFF)
            {
                ret = mqtt_transport_subscribe(&g_mqtt_config, mqtt_option->qos, (char *)mqtt_option->sub_topic_1);
                if (ret < 0)
                {
                  PRT_SEG(" > SEG:MQTTS_CLIENT_MODE:MQTTSubscribe Err %d\r\n", ret);
                  mqtt_transport_close(sock, &g_mqtt_config);
                  break;
                }
            }
            if (mqtt_option->sub_topic_2[0] != 0 && mqtt_option->sub_topic_2[0] != 0xFF)
            {
                ret = mqtt_transport_subscribe(&g_mqtt_config, mqtt_option->qos, (char *)mqtt_option->sub_topic_2);
                if (ret < 0)
                {
                  PRT_SEG(" > SEG:MQTTS_CLIENT_MODE:MQTTSubscribe Err %d\r\n", ret);
                  mqtt_transport_close(sock, &g_mqtt_config);
                  break;
                }
            }
            first_established = 1;
            PRT_SEG(" > SEG:MQTTS_CLIENT_MODE:MQTTSubscribed\r\n");
            break;
        
        case SOCK_ESTABLISHED:
            //if(getSn_IR(sock) & Sn_IR_CON)
            if(first_established)
            {
                //reg_val = SIK_ALL & 0x00FF;
                //ctlsocket(sock, CS_CLR_INTERRUPT, (void *)&reg_val);
                
                ///////////////////////////////////////////////////////////////////////////////////////////////////
                // S2E: TCP client mode initialize after connection established (only once)
                ///////////////////////////////////////////////////////////////////////////////////////////////////

                // Serial debug message printout
                if(serial_common->serial_debug_en)
                {
                    getsockopt(sock, SO_DESTIP, &destip);
                    getsockopt(sock, SO_DESTPORT, &destport);
                    PRT_SEG(" > SEG:CONNECTED TO - %d.%d.%d.%d : %d\r\n", destip[0], destip[1], destip[2], destip[3], destport);
                }
                
                if(serial_mode == SEG_SERIAL_PROTOCOL_NONE)
                    uart_rx_flush();
                                
                if(tcp_option->inactivity) {
                  flag_inactivity = SEG_DISABLE;
                  if (seg_inactivity_timer == NULL) 
                      seg_inactivity_timer = xTimerCreate("seg_inactivity_timer", pdMS_TO_TICKS(tcp_option->inactivity * 1000), pdFALSE, 0, inactivity_timer_callback);
                  xTimerStart(seg_inactivity_timer, 0);
                }

                if(tcp_option->keepalive_en) {
                    if (seg_keepalive_timer == NULL)
                        seg_keepalive_timer = xTimerCreate("seg_keepalive_timer", pdMS_TO_TICKS(tcp_option->keepalive_wait_time), pdFALSE, 0, keepalive_timer_callback);
                    else {
                        if (xTimerIsTimerActive(seg_keepalive_timer) == pdTRUE)
                            xTimerStop(seg_keepalive_timer, 0);
                        xTimerChangePeriod(seg_keepalive_timer, pdMS_TO_TICKS(tcp_option->keepalive_wait_time), 0);
                    }
                }
                first_established = 0;
                set_device_status(ST_CONNECT);
            }
            mqtt_transport_yield(&g_mqtt_config);
            break;
        
        case SOCK_CLOSE_WAIT:
            disconnect(sock);
            mqtt_transport_close(sock, &g_mqtt_config);
            break;
        
        case SOCK_FIN_WAIT:
        case SOCK_CLOSED:
            process_socket_termination(sock, SOCK_TERMINATION_DELAY);
            if(get_wiz_tls_init_state() == ENABLE) {
                wiz_tls_deinit(&s2e_tlsContext);
                set_wiz_tls_init_state(DISABLE);
            }

            if(wiz_tls_init(&s2e_tlsContext, (int *)sock) > 0)
            {
                set_device_status(ST_OPEN);

                u2e_size = 0;
                e2u_size = 0;

                if(network_connection->fixed_local_port)
                    source_port = network_connection->local_port;
                else
                    source_port = get_tcp_any_port();

                PRT_SEG(" > MQTTS_CLIENT_MODE:client_any_port = %d\r\n", client_any_port);
                if(wiz_tls_socket(&s2e_tlsContext, sock, source_port) == sock)
                {
                    // Replace the command mode switch code GAP time (default: 500ms)
                    if((serial_command->serial_command == SEG_ENABLE) && serial_data_packing->packing_time)
                        modeswitch_gap_time = serial_data_packing->packing_time;

                    if(serial_common->serial_debug_en)
                        PRT_SEG(" > SEG:MQTTS_CLIENT_MODE:SOCKOPEN\r\n");
                    set_wiz_tls_init_state(ENABLE);
                }
                else
                {
                    PRT_SEG("wiz_tls_socket() failed\r\n");
                    if(get_wiz_tls_init_state() == ENABLE)
                    {
                        wiz_tls_deinit(&s2e_tlsContext);
                        set_wiz_tls_init_state(DISABLE);
                    }
                }
                ret = mqtt_transport_init(sock, &g_mqtt_config, true, 1, g_recv_mqtt_buf, 
                          DATA_BUF_SIZE, &g_transport_interface, &g_network_context, 
                          mqtt_option->client_id, mqtt_option->user_name, mqtt_option->password, mqtt_option->keepalive, mqtt_subscribeMessageHandler);
                if (ret < 0){
                    mqtt_transport_close(sock, &g_mqtt_config);
                    PRT_SEG(" > SEG:MQTTS_CLIENT_MODE:INITIALIZE FAILED\r\n");
                }
            }
            else
            {
                PRT_SEGCP("wiz_tls_init() failed\r\n");

                if(get_wiz_tls_init_state() == ENABLE)
                {
                    wiz_tls_deinit(&s2e_tlsContext);
                    set_wiz_tls_init_state(DISABLE);
                }
                network_connection->working_mode = TCP_SERVER_MODE; // Change to TCP_SERVER_MODE
                PRT_SEG("Operation mode changed to TCP_SERVER_MODE\r\n");
            }
            break;

            
        default:
            break;
    }
}
#endif // __USE_S2E_OVER_TLS__

void proc_SEG_tcp_server(uint8_t sock)
{
    struct __tcp_option *tcp_option = (struct __tcp_option *)&(get_DevConfig_pointer()->tcp_option);
    struct __serial_common *serial_common = (struct __serial_common *)&(get_DevConfig_pointer()->serial_common);
    struct __network_connection *network_connection = (struct __network_connection *)&(get_DevConfig_pointer()->network_connection);
    struct __serial_command *serial_command = (struct __serial_command *)&(get_DevConfig_pointer()->serial_command);
    struct __serial_data_packing *serial_data_packing = (struct __serial_data_packing *)&(get_DevConfig_pointer()->serial_data_packing);
    
    uint8_t destip[4] = {0, };
    uint16_t destport = 0;
    
    // Serial communication mode
    uint8_t serial_mode = get_serial_communation_protocol();

    // Socket state
    uint8_t state = getSn_SR(sock);
    uint16_t reg_val;

    switch(state)
    {
        case SOCK_INIT:
            break;
        
        case SOCK_LISTEN:
            break;
        
        case SOCK_ESTABLISHED:
            if(getSn_IR(sock) & Sn_IR_CON)
            {
                ///////////////////////////////////////////////////////////////////////////////////////////////////
                // S2E: TCP server mode initialize after connection established (only once)
                ///////////////////////////////////////////////////////////////////////////////////////////////////

                // Interrupt clear
                // setSn_IR(sock, Sn_IR_CON);
                // reg_val = (SIK_CONNECTED | SIK_DISCONNECTED | SIK_RECEIVED | SIK_TIMEOUT) & 0x00FF; // except SIK_SENT(send OK) interrupt
                reg_val = SIK_CONNECTED & 0x00FF; // except SIK_SENT(send OK) interrupt
                ctlsocket(sock, CS_CLR_INTERRUPT, (void *)&reg_val);

                // Serial debug message printout
                if(serial_common->serial_debug_en)
                {
                    getsockopt(sock, SO_DESTIP, &destip);
                    getsockopt(sock, SO_DESTPORT, &destport);
                    PRT_SEG(" > SEG:CONNECTED FROM - %d.%d.%d.%d : %d\r\n",destip[0], destip[1], destip[2], destip[3], destport);
                }
                
                if(serial_mode == SEG_SERIAL_PROTOCOL_NONE)
                    uart_rx_flush(); // UART Ring buffer clear
                    
                if(tcp_option->inactivity) {
                  flag_inactivity = SEG_DISABLE;
                  if (seg_inactivity_timer == NULL) 
                      seg_inactivity_timer = xTimerCreate("seg_inactivity_timer", pdMS_TO_TICKS(tcp_option->inactivity * 1000), pdFALSE, 0, inactivity_timer_callback);
                  xTimerStart(seg_inactivity_timer, 0);
                }

                if(tcp_option->keepalive_en) {
                    if (seg_keepalive_timer == NULL)
                        seg_keepalive_timer = xTimerCreate("seg_keepalive_timer", pdMS_TO_TICKS(tcp_option->keepalive_wait_time), pdFALSE, 0, keepalive_timer_callback);
                    else {
                        if (xTimerIsTimerActive(seg_keepalive_timer) == pdTRUE)
                            xTimerStop(seg_keepalive_timer, 0);
                        xTimerChangePeriod(seg_keepalive_timer, pdMS_TO_TICKS(tcp_option->keepalive_wait_time), 0);
                    }
                }

                if(tcp_option->pw_connect_en) // TCP server mode only (+ mixed_server)
                {
                    flag_auth_time = SEG_DISABLE; 
                    if (seg_auth_timer == NULL) 
                      seg_auth_timer = xTimerCreate("seg_auth_timer", pdMS_TO_TICKS(MAX_CONNECTION_AUTH_TIME), pdFALSE, 0, auth_timer_callback);
                    xTimerStart(seg_auth_timer, 0);
                }
                set_device_status(ST_CONNECT);
            }
            break;

        case SOCK_CLOSE_WAIT:
            if(serial_mode == SEG_SERIAL_PROTOCOL_NONE)
            {
                while(getSn_RX_RSR(sock) || e2u_size)
                    ether_to_uart(sock); // receive remaining packets
            }
            disconnect(sock);
            //process_socket_termination(sock, SOCK_TERMINATION_DELAY);
            break;
        
        case SOCK_FIN_WAIT:
        case SOCK_CLOSED:
            process_socket_termination(sock, SOCK_TERMINATION_DELAY);
            set_device_status(ST_OPEN);
            
            u2e_size = 0;
            e2u_size = 0;

            xSemaphoreTake(seg_socket_sem, portMAX_DELAY);
            int8_t s = socket(sock, Sn_MR_TCP, network_connection->local_port, (SF_TCP_NODELAY | SF_IO_NONBLOCK));
            xSemaphoreGive(seg_socket_sem);

            if(s == sock)
            {
                // Replace the command mode switch code GAP time (default: 500ms)
                if((serial_command->serial_command == SEG_ENABLE) && serial_data_packing->packing_time)
                    modeswitch_gap_time = serial_data_packing->packing_time;
                
                // TCP Server listen
                listen(sock);
                
                if(serial_common->serial_debug_en)
                    PRT_SEG(" > SEG:TCP_SERVER_MODE:SOCKOPEN\r\n");
            }
            break;
            
        default:
            break;
    }
}

void proc_SEG_tcp_mixed(uint8_t sock)
{
    struct __tcp_option *tcp_option = (struct __tcp_option *)&(get_DevConfig_pointer()->tcp_option);
    struct __network_connection *network_connection = (struct __network_connection *)&(get_DevConfig_pointer()->network_connection);
    struct __serial_common *serial_common = (struct __serial_common *)&get_DevConfig_pointer()->serial_common;
    struct __serial_command *serial_command = (struct __serial_command *)&get_DevConfig_pointer()->serial_command;
    struct __serial_data_packing *serial_data_packing = (struct __serial_data_packing *)&(get_DevConfig_pointer()->serial_data_packing);

    uint16_t source_port = 0;
    uint8_t destip[4] = {0, };
    uint16_t destport = 0;
    
    // Serial communication mode
    uint8_t serial_mode = get_serial_communation_protocol();

    // Socket state
    uint8_t state = getSn_SR(sock);
    uint16_t reg_val;

#ifdef MIXED_CLIENT_LIMITED_CONNECT
    static uint8_t reconnection_count = 0;
#endif
    switch(state)
    {
        case SOCK_INIT:
            if(mixed_state == MIXED_CLIENT)
            {
                if(reconnection_count && tcp_option->reconnection)
                  vTaskDelay(tcp_option->reconnection);
                
                // TCP connect exception checker; e.g., dns failed / zero srcip ... and etc.
                if(check_tcp_connect_exception() == ON)
                {
#ifdef MIXED_CLIENT_LIMITED_CONNECT
                    process_socket_termination(sock, SOCK_TERMINATION_DELAY);
                    reconnection_count = 0;
                    uart_rx_flush();
                    mixed_state = MIXED_SERVER;
#endif
                    return;
                }
                
                // TCP connect
                connect(sock, network_connection->remote_ip, network_connection->remote_port);
                
#ifdef MIXED_CLIENT_LIMITED_CONNECT
                reconnection_count++;

                if(reconnection_count >= MAX_RECONNECTION_COUNT)
                {
                    PRT_SEG("reconnection_count >= MAX_RECONNECTION_COUNT\r\n");
                    process_socket_termination(sock, SOCK_TERMINATION_DELAY);
                    reconnection_count = 0;
                    uart_rx_flush();
                    mixed_state = MIXED_SERVER;
                }
#ifdef _SEG_DEBUG_
                if(reconnection_count != 0)
                    PRT_SEG(" > SEG:TCP_MIXED_MODE:CLIENT_CONNECTION [%d]\r\n", reconnection_count);
                else
                    PRT_SEG(" > SEG:TCP_MIXED_MODE:CLIENT_CONNECTION_RETRY FAILED\r\n");
#endif
#endif
            }
            break;
        
        case SOCK_LISTEN:
            break;
        
        case SOCK_ESTABLISHED:
            if(getSn_IR(sock) & Sn_IR_CON)
            {
                ///////////////////////////////////////////////////////////////////////////////////////////////////
                // S2E: TCP mixed (server or client) mode initialize after connection established (only once)
                ///////////////////////////////////////////////////////////////////////////////////////////////////
                reg_val = SIK_CONNECTED & 0x00FF;
                ctlsocket(sock, CS_CLR_INTERRUPT, (void *)&reg_val);
                
                // Serial debug message printout
                if(serial_common->serial_debug_en)
                {
                    getsockopt(sock, SO_DESTIP, &destip);
                    getsockopt(sock, SO_DESTPORT, &destport);
                    
                    if(mixed_state == MIXED_SERVER)
                        PRT_SEG(" > SEG:CONNECTED FROM - %d.%d.%d.%d : %d\r\n", destip[0], destip[1], destip[2], destip[3], destport);
                    else
                        PRT_SEG(" > SEG:CONNECTED TO - %d.%d.%d.%d : %d\r\n", destip[0], destip[1], destip[2], destip[3], destport);
                }

                if(tcp_option->inactivity) {
                  flag_inactivity = SEG_DISABLE;
                  if (seg_inactivity_timer == NULL) 
                      seg_inactivity_timer = xTimerCreate("seg_inactivity_timer", pdMS_TO_TICKS(tcp_option->inactivity * 1000), pdFALSE, 0, inactivity_timer_callback);
                  xTimerStart(seg_inactivity_timer, 0);
                }
            
                if(tcp_option->keepalive_en) {
                    if (seg_keepalive_timer == NULL)
                        seg_keepalive_timer = xTimerCreate("seg_keepalive_timer", pdMS_TO_TICKS(tcp_option->keepalive_wait_time), pdFALSE, 0, keepalive_timer_callback);
                    else {
                        if (xTimerIsTimerActive(seg_keepalive_timer) == pdTRUE)
                            xTimerStop(seg_keepalive_timer, 0);
                        xTimerChangePeriod(seg_keepalive_timer, pdMS_TO_TICKS(tcp_option->keepalive_wait_time), 0);
                    }
                }

                set_device_status(ST_CONNECT);
                // Check the connection password auth timer
                if(mixed_state == MIXED_SERVER)
                {
                    // Connection Password option: TCP server mode only (+ mixed_server)
                    uart_rx_flush();
                    flag_auth_time = SEG_DISABLE;
                    if(tcp_option->pw_connect_en == SEG_ENABLE)
                    {
                        if (seg_auth_timer == NULL) 
                          seg_auth_timer = xTimerCreate("seg_auth_timer", pdMS_TO_TICKS(MAX_CONNECTION_AUTH_TIME), pdTRUE, 0, auth_timer_callback);
                        xTimerStart(seg_auth_timer, 0);
                    }
                }
                else
                {
                    if(get_uart_buffer_usedsize() || u2e_size)
                        xSemaphoreGive(seg_u2e_sem);
                        //uart_to_ether(sock);
                        
                    // Mixed-mode flag switching in advance
					mixed_state = MIXED_SERVER;
                }
                
#ifdef MIXED_CLIENT_LIMITED_CONNECT
                reconnection_count = 0;
#endif
            }
            break;
        
        case SOCK_CLOSE_WAIT:
            PRT_SEG("case SOCK_CLOSE_WAIT\r\n");
            if(serial_mode == SEG_SERIAL_PROTOCOL_NONE)
            {
                while(getSn_RX_RSR(sock) || e2u_size)
                    ether_to_uart(sock); // receive remaining packets
            }
            //process_socket_termination(sock, SOCK_TERMINATION_DELAY);
            disconnect(sock);
            break;
        
        case SOCK_FIN_WAIT:
        case SOCK_CLOSED:
            PRT_SEG("case SOCK_FIN_WAIT or SOCK_CLOSED\r\n");
            set_device_status(ST_OPEN);
            process_socket_termination(sock, SOCK_TERMINATION_DELAY);

            if(mixed_state == MIXED_SERVER) // MIXED_SERVER
            {
                u2e_size = 0;
                e2u_size = 0;
                
                xSemaphoreTake(seg_socket_sem, portMAX_DELAY);
                int8_t s = socket(sock, Sn_MR_TCP, network_connection->local_port, (SF_TCP_NODELAY | SF_IO_NONBLOCK));
                xSemaphoreGive(seg_socket_sem);

                if(s == sock)
                {
                    // Replace the command mode switch code GAP time (default: 500ms)
                    if((serial_command->serial_command == SEG_ENABLE) && serial_data_packing->packing_time)
                        modeswitch_gap_time = serial_data_packing->packing_time;
                    
                    // TCP Server listen
                    listen(sock);
                    
                    if(serial_common->serial_debug_en)
                        PRT_SEG(" > SEG:TCP_MIXED_MODE:SERVER_SOCKOPEN\r\n");
                }
            }
            else  // MIXED_CLIENT
            {
                PRT_INFO(" > SEG:TCP_MIXED_MODE:CLIENT_SOCKCLOSED\r\n");
                e2u_size = 0;
                if(network_connection->fixed_local_port)
                    source_port = network_connection->local_port;
                else
                    source_port = get_tcp_any_port();

#ifdef _SEG_DEBUG_
                PRT_SEG(" > TCP CLIENT: any_port = %d\r\n", source_port);
#endif
                xSemaphoreTake(seg_socket_sem, portMAX_DELAY);
                int8_t s = socket(sock, Sn_MR_TCP, source_port, (SF_TCP_NODELAY | SF_IO_NONBLOCK));
                xSemaphoreGive(seg_socket_sem);
                if(s == sock)
                {
                    // Replace the command mode switch code GAP time (default: 500ms)
                    if((serial_command->serial_command == SEG_ENABLE) && serial_data_packing->packing_time)
                        modeswitch_gap_time = serial_data_packing->packing_time;
                    
                    // Enable the reconnection Timer
                    //if(tcp_option->reconnection)
                        //reconnection_start_time = millis();
                    
                    if(serial_common->serial_debug_en)
                        PRT_SEG(" > SEG:TCP_MIXED_MODE:CLIENT_SOCKOPEN\r\n");
                }
            }
            break;
            
        default:
            break;
    }
}

void uart_to_ether(uint8_t sock)
{
    struct __network_connection *network_connection = (struct __network_connection *)&(get_DevConfig_pointer()->network_connection);
    struct __serial_common *serial_common = (struct __serial_common *)&get_DevConfig_pointer()->serial_common;
    struct __tcp_option *tcp_option = (struct __tcp_option *)&(get_DevConfig_pointer()->tcp_option);
    struct __mqtt_option *mqtt_option = (struct __mqtt_option *)&(get_DevConfig_pointer()->mqtt_option);

    uint16_t len;
    int16_t sent_len = 0;

    // UART ring buffer -> user's buffer

    len = get_serial_data();
    
    if(len > 0)
    {
        if (seg_inactivity_timer != NULL)
            xTimerReset(seg_inactivity_timer, 0);

        if((serial_common->serial_debug_en == SEG_DEBUG_S2E) || (serial_common->serial_debug_en == SEG_DEBUG_ALL))
            debugSerial_dataTransfer(g_send_buf, len, SEG_DEBUG_S2E);
        
        if (network_connection->working_mode == UDP_MODE)
        {
            if((network_connection->remote_ip[0] == 0x00) &&
               (network_connection->remote_ip[1] == 0x00) &&
               (network_connection->remote_ip[2] == 0x00) &&
               (network_connection->remote_ip[3] == 0x00))
            {
                if((peerip[0] == 0x00) && (peerip[1] == 0x00) && (peerip[2] == 0x00) && (peerip[3] == 0x00))
                {
                    if(serial_common->serial_debug_en)
                        PRT_SEG(" > SEG:UDP_MODE:DATA SEND FAILED - UDP Peer IP/Port required (0.0.0.0)\r\n");
                }
                else
                    sent_len = (int16_t)sendto(sock, g_send_buf, len, peerip, peerport); // UDP 1:N mode
            }
            else
                sent_len = (int16_t)sendto(sock, g_send_buf, len, network_connection->remote_ip, network_connection->remote_port); // UDP 1:1 mode
        }
        else if (network_connection->working_state == ST_CONNECT)
        {
            if (network_connection->working_mode == MQTT_CLIENT_MODE || network_connection->working_mode == MQTTS_CLIENT_MODE)
                sent_len = wizchip_mqtt_publish(&g_mqtt_config, mqtt_option->pub_topic, mqtt_option->qos, g_send_buf, len);
#ifdef __USE_S2E_OVER_TLS__
            else if (network_connection->working_mode == SSL_TCP_CLIENT_MODE)
                sent_len = wiz_tls_write(&s2e_tlsContext, g_send_buf, len);
#endif
            else
                sent_len = (int16_t)send(sock, g_send_buf, len);

            if(tcp_option->keepalive_en == ENABLE && flag_first_keepalive == DISABLE)
            {
                flag_first_keepalive = ENABLE;
                xTimerStart(seg_keepalive_timer, 0);
            }
        }
        if(sent_len > 0) u2e_size-=sent_len;
    }
}

uint16_t get_serial_data(void)
{
    struct __serial_data_packing *serial_data_packing = (struct __serial_data_packing *)&(get_DevConfig_pointer()->serial_data_packing);

    uint16_t i;
    uint16_t len;

    len = get_uart_buffer_usedsize();
    
    if((len + u2e_size) >= DATA_BUF_SIZE) // Avoiding u2e buffer (g_send_buf) overflow
    {
        /* Checking Data packing option: character delimiter */
        if((serial_data_packing->packing_delimiter[0] != 0x00) && (len == 1))
        {
            g_send_buf[u2e_size] = (uint8_t)platform_uart_getc();
            if(serial_data_packing->packing_delimiter[0] == g_send_buf[u2e_size])
                return u2e_size;
        }
        
        // serial data length value update for avoiding u2e buffer overflow
        len = DATA_BUF_SIZE - u2e_size;
    }
    
    if((!serial_data_packing->packing_time) &&
       (!serial_data_packing->packing_size) &&
       (!serial_data_packing->packing_delimiter[0])) // No Data Packing tiem / size / delimiters.
    {
        // ## 20150427 bugfix: Incorrect serial data storing (UART ring buffer to g_send_buf)
        for(i = 0; i < len; i++)
        {
            g_send_buf[u2e_size++] = (uint8_t)platform_uart_getc();
        }
        
        return u2e_size;
    }
    else
    {
        /* Checking Data packing options */
        for(i = 0; i < len; i++)
        {
            g_send_buf[u2e_size++] = (uint8_t)platform_uart_getc();
            
            // Packing delimiter: character option
            if((serial_data_packing->packing_delimiter[0] != 0x00) &&
               (serial_data_packing->packing_delimiter[0] == g_send_buf[u2e_size - 1]))
                return u2e_size;
            
            // Packing delimiter: size option
            if((serial_data_packing->packing_size != 0) && (serial_data_packing->packing_size == u2e_size))
                return u2e_size;
        }
    }
    
    // Packing delimiter: time option
    if((serial_data_packing->packing_time != 0) && (u2e_size != 0) && (flag_serial_input_time_elapse))
    {
        if(get_uart_buffer_usedsize() == 0)
            flag_serial_input_time_elapse = SEG_DISABLE; // ##
        
        return u2e_size;
    }
    
    return 0;
}

void ether_to_uart(uint8_t sock)
{
    struct __serial_option *serial_option = (struct __serial_option *)&(get_DevConfig_pointer()->serial_option);
    struct __serial_common *serial_common = (struct __serial_common *)&(get_DevConfig_pointer()->serial_common);
    struct __network_connection *network_connection = (struct __network_connection *)&(get_DevConfig_pointer()->network_connection);
    struct __tcp_option *tcp_option = (struct __tcp_option *)&(get_DevConfig_pointer()->tcp_option);

    uint16_t len;
    uint16_t i;
    uint16_t reg_val;

    if(serial_option->flow_control == flow_rts_cts)
    {
#ifdef __USE_GPIO_HARDWARE_FLOWCONTROL__
        if(get_uart_cts_pin() != UART_CTS_LOW) return; // DATA0 (RS-232) only
#else
        ; // check the CTS reg
#endif
    }

    do {
        // H/W Socket buffer -> User's buffer
        if (!(network_connection->working_mode == MQTT_CLIENT_MODE || network_connection->working_mode == MQTTS_CLIENT_MODE))
        {
            len = getSn_RX_RSR(sock);
            if(len > DATA_BUF_SIZE) len = DATA_BUF_SIZE; // avoiding buffer overflow
            
            if(len > 0)
            {
                if (seg_inactivity_timer != NULL)
                  xTimerReset(seg_inactivity_timer, 0);

                if (network_connection->working_mode == UDP_MODE) {
                    e2u_size = recvfrom(sock, g_recv_buf, len, peerip, &peerport);
                    
                    if(memcmp(peerip_tmp, peerip, 4) !=  0)
                    {
                        memcpy(peerip_tmp, peerip, 4);
                        if(serial_common->serial_debug_en)
                            PRT_SEG(" > UDP Peer IP/Port: %d.%d.%d.%d : %d\r\n", peerip[0], peerip[1], peerip[2], peerip[3], peerport);
                    }
                }
                else if (network_connection->working_state == ST_CONNECT) {
#ifdef __USE_S2E_OVER_TLS__
                    if (network_connection->working_mode == SSL_TCP_CLIENT_MODE)
                        e2u_size = wiz_tls_read(&s2e_tlsContext, g_recv_buf, len);
                    else
#endif
                        e2u_size = recv(sock, g_recv_buf, len);
                }
                reg_val = SIK_RECEIVED & 0x00FF;
                ctlsocket(sock, CS_CLR_INTERRUPT, (void *)&reg_val);

            }
            else
              break;
            
            if((network_connection->working_mode == TCP_SERVER_MODE) ||  \
              ((network_connection->working_mode == TCP_MIXED_MODE) && (mixed_state == MIXED_SERVER)))
            {
                // Connection password authentication
                if((tcp_option->pw_connect_en == SEG_ENABLE) && (flag_connect_pw_auth == SEG_DISABLE))
                {
                    if(check_connect_pw_auth(g_recv_buf, len) == SEG_ENABLE)
                        flag_connect_pw_auth = SEG_ENABLE;
                    else
                        flag_connect_pw_auth = SEG_DISABLE;
                    e2u_size = 0;

                    xTimerStop(seg_auth_timer, 0);
                    if(flag_connect_pw_auth == SEG_DISABLE)
                    {
                        disconnect(sock);
                        return;
                    }
                }
            }
        }
        // Ethernet data transfer to DATA UART
        if(e2u_size != 0)
        {
            if(serial_option->dsr_en == SEG_ENABLE) // DTR / DSR handshake (flow control)
                if(get_flowcontrol_dsr_pin() == IO_HIGH) return;
    //////////////////////////////////////////////////////////////////////
#ifdef __USE_UART_485_422__
            if((serial_option->uart_interface == UART_IF_RS422) ||
               (serial_option->uart_interface == UART_IF_RS485))
            {
                if((serial_common->serial_debug_en == SEG_DEBUG_E2S) || (serial_common->serial_debug_en == SEG_DEBUG_ALL))
                    debugSerial_dataTransfer(g_recv_buf, e2u_size, SEG_DEBUG_E2S);
                
                uart_rs485_enable();
                for(i = 0; i < e2u_size; i++) platform_uart_putc(g_recv_buf[i]);
                uart_rs485_disable();
                
                e2u_size = 0;
            }
    //////////////////////////////////////////////////////////////////////
            else if(serial_option->flow_control == flow_xon_xoff)
#else
            if(serial_option->flow_control == flow_xon_xoff)
#endif
            {
                if(isXON == SEG_ENABLE)
                {
                    if((serial_common->serial_debug_en == SEG_DEBUG_E2S) || (serial_common->serial_debug_en == SEG_DEBUG_ALL))
                        debugSerial_dataTransfer(g_recv_buf, e2u_size, SEG_DEBUG_E2S);
                    
                    for(i = 0; i < e2u_size; i++) platform_uart_putc(g_recv_buf[i]);
                    e2u_size = 0;
                }
            }
            else
            {
                if((serial_common->serial_debug_en == SEG_DEBUG_E2S) || (serial_common->serial_debug_en == SEG_DEBUG_ALL))
                    debugSerial_dataTransfer(g_recv_buf, e2u_size, SEG_DEBUG_E2S);

                for(i = 0; i < e2u_size; i++) platform_uart_putc(g_recv_buf[i]);
                e2u_size = 0;
            }
        }
    }while(e2u_size);
}


uint16_t get_tcp_any_port(void)
{
    if(client_any_port)
    {
        if(client_any_port < 0xffff)
            client_any_port++;
        else
            client_any_port = 0;
    }
    
    if(client_any_port == 0)
    {
        // todo: gen random seed (srand + random value)
        client_any_port = (rand() % 10000) + 35000; // 35000 ~ 44999
    }
    
    return client_any_port;
}


uint8_t get_serial_communation_protocol(void)
{
    struct __serial_option *serial_option = (struct __serial_option *)&(get_DevConfig_pointer()->serial_option);

    // SEG_SERIAL_PROTOCOL_NONE
    // SEG_SERIAL_MODBUS_RTU
    // SEG_SERIAL_MODBUS_ASCII

    return serial_option->protocol;
}


void send_keepalive_packet_manual(uint8_t sock)
{
    setsockopt(sock, SO_KEEPALIVESEND, 0);
}


uint8_t process_socket_termination(uint8_t sock, uint32_t timeout)
{
    struct __network_connection *network_connection = (struct __network_connection *)&(get_DevConfig_pointer()->network_connection);

    int8_t ret;
    uint8_t sock_status = getSn_SR(sock);
    uint32_t tickStart = millis();
    
    timers_stop();
    if (!(network_connection->working_mode == TCP_MIXED_MODE && mixed_state == MIXED_CLIENT)) {
        reset_SEG_timeflags();
    }
    
#ifdef __USE_S2E_OVER_TLS__
    if(get_wiz_tls_init_state() == ENABLE) {
        wiz_tls_close_notify(&s2e_tlsContext);
        PRT_SEG("wiz_tls_deinit\r\n");
        wiz_tls_deinit(&s2e_tlsContext);
        set_wiz_tls_init_state(DISABLE);
    }
#endif

    if(sock_status == SOCK_CLOSED) return sock;
    xSemaphoreTake(seg_socket_sem, portMAX_DELAY);
    if(network_connection->working_mode != UDP_MODE) // TCP_SERVER_MODE / TCP_CLIENT_MODE / TCP_MIXED_MODE
    {
        //if (network_connection->working_mode == TCP_MIXED_MODE)
        //    mixed_state = MIXED_SERVER;

        if((sock_status == SOCK_ESTABLISHED) || (sock_status == SOCK_CLOSE_WAIT)) {
            do {
              ret = disconnect(sock);
              if((ret == SOCK_OK) || (ret == SOCKERR_TIMEOUT)) break;
            } while ((millis() - tickStart) < timeout);
        }
    }

    close(sock);
    xSemaphoreGive(seg_socket_sem);
	xSemaphoreGive(seg_sem);
    return sock;
}

uint8_t check_connect_pw_auth(uint8_t * buf, uint16_t len)
{
    struct __tcp_option *tcp_option = (struct __tcp_option *)&(get_DevConfig_pointer()->tcp_option);

    uint8_t ret = SEG_DISABLE;
    uint8_t pwbuf[11] = {0,};
    
    if(len >= sizeof(pwbuf))
    {
        len = sizeof(pwbuf) - 1;
    }
    
    memcpy(pwbuf, buf, len);
    if((len == strlen(tcp_option->pw_connect)) && (memcmp(tcp_option->pw_connect, pwbuf, len) == 0))
    {
        ret = SEG_ENABLE; // Connection password auth success
    }
    
#ifdef _SEG_DEBUG_
    PRT_SEG(" > Connection password: %s, len: %d\r\n", tcp_option->pw_connect, strlen(tcp_option->pw_connect));
    PRT_SEG(" > Entered password: %s, len: %d\r\n", pwbuf, len);
    PRT_SEG(" >> Auth %s\r\n", ret ? "success":"failed");
#endif
    
    return ret;
}


void init_trigger_modeswitch(uint8_t mode)
{
    struct __serial_common *serial_common = (struct __serial_common *)&(get_DevConfig_pointer()->serial_common);
    struct __network_connection *network_connection = (struct __network_connection *)&(get_DevConfig_pointer()->network_connection);
    
    if(mode == DEVICE_AT_MODE)
    {
        opmode = DEVICE_AT_MODE;
        set_device_status(ST_ATMODE);
        
        if(serial_common->serial_debug_en)
        {
            PRT_SEG(" > SEG:AT Mode\r\n");
            platform_uart_puts((uint8_t *)"SEG:AT Mode\r\n", strlen("SEG:AT Mode\r\n"));
        }
    }
    else // DEVICE_GW_MODE
    {
        opmode = DEVICE_GW_MODE;
        set_device_status(ST_OPEN);

        if(network_connection->working_mode == TCP_MIXED_MODE)
        {
            mixed_state = MIXED_SERVER;
        }
                
        if(serial_common->serial_debug_en)
        {
            PRT_SEG(" > SEG:GW Mode\r\n");
            platform_uart_puts((uint8_t *)"SEG:GW Mode\r\n", strlen("SEG:GW Mode\r\n"));
        }
    }
    
    u2e_size = 0;
    uart_rx_flush();
    reset_SEG_timeflags();
    //xTimerReset(seg_inactivity_timer, 0);
}

uint8_t check_modeswitch_trigger(uint8_t ch)
{
    struct __serial_command *serial_command = (struct __serial_command *)&(get_DevConfig_pointer()->serial_command);
    
    uint8_t modeswitch_failed = SEG_DISABLE;
    uint8_t ret = 0;
    
    if(opmode != DEVICE_GW_MODE)                return 0;
    if(serial_command->serial_command == SEG_DISABLE)   return 0;
    
    switch(triggercode_idx)
    {
        case 0:
            if((ch == serial_command->serial_trigger[triggercode_idx]) && (modeswitch_time >= modeswitch_gap_time)) // comparison succeed
            {
                ch_tmp[triggercode_idx] = ch;
                triggercode_idx++;
                enable_modeswitch_timer = SEG_ENABLE;
            }
            break;
            
        case 1:
        case 2:
            if((ch == serial_command->serial_trigger[triggercode_idx]) && (modeswitch_time <= modeswitch_gap_time)) // comparison succeed
            {
                ch_tmp[triggercode_idx] = ch;
                triggercode_idx++;
            }
            else // comparison failed: invalid trigger code
                modeswitch_failed = SEG_ENABLE; 
            break;
        case 3:
            if(modeswitch_time < modeswitch_gap_time) // comparison failed: end gap
                modeswitch_failed = SEG_ENABLE;
            break;
    }
    
    if(modeswitch_failed == SEG_ENABLE)
        restore_serial_data(triggercode_idx);
    
    modeswitch_time = 0; // reset the inter-gap time count for each trigger code recognition (Allowable interval)
    ret = triggercode_idx;
    
    return ret;
}

// when serial command mode trigger code comparison failed
void restore_serial_data(uint8_t idx)
{
    uint8_t i;
    
    for(i = 0; i < idx; i++)
    {
        put_byte_to_uart_buffer(ch_tmp[i]);
        ch_tmp[i] = 0x00;
    }
    
    enable_modeswitch_timer = SEG_DISABLE;
    triggercode_idx = 0;
}

uint8_t check_serial_store_permitted(uint8_t ch)
{
    struct __network_connection *network_connection = (struct __network_connection *)&(get_DevConfig_pointer()->network_connection);
    struct __serial_option *serial_option = (struct __serial_option *)&(get_DevConfig_pointer()->serial_option);
    
    uint8_t ret = SEG_DISABLE; // SEG_DISABLE: Doesn't put the serial data in a ring buffer

    switch(network_connection->working_state)
    {
        case ST_OPEN:
            if(network_connection->working_mode != TCP_MIXED_MODE) return ret;
        case ST_CONNECT:
        case ST_UDP:
        case ST_ATMODE:
            ret = SEG_ENABLE;
            break;
        default:
            break;
    }

    // Software flow control: Check the XON/XOFF start/stop commands
    // [Peer] -> [WIZnet Device]
    if((ret == SEG_ENABLE) && (serial_option->flow_control == flow_xon_xoff))
    {
        if(ch == UART_XON)
        {
            isXON = SEG_ENABLE;
            ret = SEG_DISABLE; 
        }
        else if(ch == UART_XOFF)
        {
            isXON = SEG_DISABLE;
            ret = SEG_DISABLE;
        }
    }
    return ret;
}

void reset_SEG_timeflags(void)
{
    // Timer disable
    enable_serial_input_timer = SEG_DISABLE;
    
    // Flag clear
    flag_serial_input_time_elapse = SEG_DISABLE;
    flag_send_keepalive = SEG_DISABLE;
    flag_first_keepalive = SEG_DISABLE;
    flag_auth_time = SEG_DISABLE;
    flag_connect_pw_auth = SEG_DISABLE; // TCP_SERVER_MODE only (+ MIXED_SERVER)
    flag_inactivity = SEG_DISABLE;
    
    // Timer value clea
    serial_input_time = 0;
}

void init_time_delimiter_timer(void)
{
    struct __serial_data_packing *serial_data_packing = (struct __serial_data_packing *)&(get_DevConfig_pointer()->serial_data_packing);
    
    if(opmode == DEVICE_GW_MODE)
    {
        if(serial_data_packing->packing_time != 0)
        {
            if(enable_serial_input_timer == SEG_DISABLE)
                enable_serial_input_timer = SEG_ENABLE;
            serial_input_time = 0;
        }
    }
}

uint8_t check_tcp_connect_exception(void)
{
    struct __network_option *network_option = (struct __network_option *)&get_DevConfig_pointer()->network_option;
    struct __serial_common *serial_common = (struct __serial_common *)&get_DevConfig_pointer()->serial_common;
    struct __network_connection *network_connection = (struct __network_connection *)&(get_DevConfig_pointer()->network_connection);
    
    uint8_t srcip[4] = {0, };
    uint8_t ret = OFF;
    
    getSIPR(srcip);
    
    // DNS failed
    if((network_connection->dns_use == SEG_ENABLE) && (flag_process_dns_success != ON))
    {
        if(serial_common->serial_debug_en) PRT_SEG(" > SEG:CONNECTION FAILED - DNS Failed flag_process_dns_success = %d\r\n", flag_process_dns_success);
        ret = ON;
    }
    // if dhcp failed (0.0.0.0), this case do not connect to peer
    else if((srcip[0] == 0x00) && (srcip[1] == 0x00) && (srcip[2] == 0x00) && (srcip[3] == 0x00))
    {
        if(serial_common->serial_debug_en) PRT_SEG(" > SEG:CONNECTION FAILED - Invalid IP address: Zero IP\r\n");
        ret = ON;
    }
    // Destination zero IP
    else if((network_connection->remote_ip[0] == 0x00) &&
            (network_connection->remote_ip[1] == 0x00) &&
            (network_connection->remote_ip[2] == 0x00) &&
            (network_connection->remote_ip[3] == 0x00))
    {
        if(serial_common->serial_debug_en) PRT_SEG(" > SEG:CONNECTION FAILED - Invalid Destination IP address: Zero IP\r\n");
        ret = ON;
    }
     // Duplicate IP address
    else if((srcip[0] == network_connection->remote_ip[0]) &&
            (srcip[1] == network_connection->remote_ip[1]) &&
            (srcip[2] == network_connection->remote_ip[2]) &&
            (srcip[3] == network_connection->remote_ip[3]))
    {
        if(serial_common->serial_debug_en) PRT_SEG(" > SEG:CONNECTION FAILED - Duplicate IP address\r\n");
        ret = ON;
    }
    else if((srcip[0] == 192) && (srcip[1] == 168)) // local IP address == Class C private IP
    {
        // Static IP address obtained
        if((network_option->dhcp_use == SEG_DISABLE) && ((network_connection->remote_ip[0] == 192) &&
                                                         (network_connection->remote_ip[1] == 168)))
        {
            if(srcip[2] != network_connection->remote_ip[2]) // Class C Private IP network mismatch
            {
                if(serial_common->serial_debug_en)
                    PRT_SEG(" > SEG:CONNECTION FAILED - Invalid IP address range (%d.%d.[%d].%d)\r\n",
                                                                network_connection->remote_ip[0],
                                                                network_connection->remote_ip[1],
                                                                network_connection->remote_ip[2],
                                                                network_connection->remote_ip[3]);
                ret = ON; 
            }
        }
    }
    
    return ret;
}

int wizchip_mqtt_publish(mqtt_config_t *mqtt_config, uint8_t *pub_topic, uint8_t qos, uint8_t *pub_data, uint32_t pub_data_len)
{
//    PRT_SEG("MQTT PUB Len = %d\r\n", pub_data_len);
//    PRT_SEG("MQTT PUB Data = %.*s\r\n", pub_data_len, pub_data);
    
    if(mqtt_transport_publish(mqtt_config, pub_topic, pub_data, pub_data_len, qos))
        return -1;
    return pub_data_len;
}

void mqtt_subscribeMessageHandler(uint8_t *data, uint32_t data_len)
{
    e2u_size = data_len;
    memcpy(g_recv_buf, data, data_len);
    ether_to_uart(SOCK_DATA); //socket parameter is not used in mqtt
}

uint16_t debugSerial_dataTransfer(uint8_t * buf, uint16_t size, teDEBUGTYPE type)
{
    uint16_t bytecnt = 0;

    if(getDeviceUptime_day() > 0)
        printf(" [%ldd/%02d:%02d:%02d]", getDeviceUptime_day(), getDeviceUptime_hour(), getDeviceUptime_min(), getDeviceUptime_sec());
    else
        printf(" [%02d:%02d:%02d]", getDeviceUptime_hour(), getDeviceUptime_min(), getDeviceUptime_sec());
    
    if((type == SEG_DEBUG_S2E) || (type == SEG_DEBUG_E2S))
    {
        printf("[%s][%04d] ", (type == SEG_DEBUG_S2E)?"S2E":"E2S", size);
        for(bytecnt = 0; bytecnt < size; bytecnt++) printf("%02X ", buf[bytecnt]);
        printf("\r\n");
    }
    
    return bytecnt;
}


void send_sid(uint8_t sock, uint8_t link_message)
{
    DevConfig *dev_config = get_DevConfig_pointer();

    uint8_t buf[45] = {0, };
    uint8_t len = 0;

    switch(link_message)
    {
        case SEG_LINK_MSG_NONE:
            break;

        case SEG_LINK_MSG_DEVNAME:
            len = snprintf((char *)buf, sizeof(buf), "%s", dev_config->device_common.device_name);
            break;

        case SEG_LINK_MSG_MAC:
            len = snprintf((char *)buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                                                     dev_config->network_common.mac[0],
                                                     dev_config->network_common.mac[1],
                                                     dev_config->network_common.mac[2],
                                                     dev_config->network_common.mac[3],
                                                     dev_config->network_common.mac[4],
                                                     dev_config->network_common.mac[5]);
            break;

        case SEG_LINK_MSG_IP:
            len = snprintf((char *)buf, sizeof(buf), "%d.%d.%d.%d",
                                                     dev_config->network_common.local_ip[0],
                                                     dev_config->network_common.local_ip[1],
                                                     dev_config->network_common.local_ip[2],
                                                     dev_config->network_common.local_ip[3]);
            break;

        case SEG_LINK_MSG_DEVID:
            len = snprintf((char *)buf, sizeof(buf), "%s-%02X%02X%02X%02X%02X%02X",
                                                     dev_config->device_common.device_name,
                                                     dev_config->network_common.mac[0],
                                                     dev_config->network_common.mac[1],
                                                     dev_config->network_common.mac[2],
                                                     dev_config->network_common.mac[3],
                                                     dev_config->network_common.mac[4],
                                                     dev_config->network_common.mac[5]);
            break;


        case SEG_LINK_MSG_DEVALIAS:
            len = snprintf((char *)buf, sizeof(buf), "%s", dev_config->device_option.device_alias);
            break;

        case SEG_LINK_MSG_DEVGROUP:
            len = snprintf((char *)buf, sizeof(buf), "%s", dev_config->device_option.device_group);
            break;

        default:
            break;
    }

    if(len > 0)
    {
        send(sock, buf, len);
    }
}

// This function have to call every 1 millisecond by Timer IRQ handler routine.
void seg_timer_msec(void)
{
    struct __serial_data_packing *serial_data_packing = (struct __serial_data_packing *)&(get_DevConfig_pointer()->serial_data_packing);
    struct __network_connection *network_connection = (struct __network_connection *)&(get_DevConfig_pointer()->network_connection);
    
    signed portBASE_TYPE xHigherPriorityTaskWoken = pdFALSE;

    // Serial data packing time delimiter timer
    if(enable_serial_input_timer)
    {
        if(serial_input_time < serial_data_packing->packing_time)
        {
            serial_input_time++;
        }
        else
        {
            serial_input_time = 0;
            enable_serial_input_timer = 0;
            flag_serial_input_time_elapse = SEG_ENABLE;

            switch (network_connection->working_mode)
            {
                case TCP_CLIENT_MODE:
                case TCP_SERVER_MODE:
                case TCP_MIXED_MODE:
                case SSL_TCP_CLIENT_MODE:
                case UDP_MODE:
                case MQTT_CLIENT_MODE:
                case MQTTS_CLIENT_MODE:
                    xSemaphoreGiveFromISR(seg_u2e_sem, &xHigherPriorityTaskWoken);
                    portEND_SWITCHING_ISR(xHigherPriorityTaskWoken);
                    break;
            }
            
        }
    }

    // Mode switch timer: Time count routine (msec) (GW mode <-> Serial command mode, for s/w mode switch trigger code)
    if(modeswitch_time < modeswitch_gap_time) modeswitch_time++;

    if((enable_modeswitch_timer) && (modeswitch_time >= modeswitch_gap_time))
    {
        // result of command mode trigger code comparison
        if(triggercode_idx == 3) {
            sw_modeswitch_at_mode_on = SEG_ENABLE;  // success}
            xSemaphoreGiveFromISR(segcp_uart_sem, &xHigherPriorityTaskWoken);
            portEND_SWITCHING_ISR(xHigherPriorityTaskWoken);
        }
        else
            restore_serial_data(triggercode_idx);   // failed

        triggercode_idx = 0;
        enable_modeswitch_timer = SEG_DISABLE;
    }
}

void seg_task (void *argument)  {
    
    while(1) {
        if (get_net_status() == NET_LINK_DISCONNECTED)
        {
            PRT_SEGCP("get_net_status() != NET_LINK_DISCONNECTED\r\n");
            xSemaphoreTake(net_seg_sem, portMAX_DELAY);
        }
        do_seg(SEG_DATA0_SOCK);
        xSemaphoreTake(seg_sem, pdMS_TO_TICKS(10));
        //vTaskDelay(pdMS_TO_TICKS(10)); // wait for 10ms
    }
}

void seg_u2e_task (void *argument)  {
    uint8_t serial_mode = get_serial_communation_protocol();
    struct __network_connection *network_connection = (struct __network_connection *)&(get_DevConfig_pointer()->network_connection);

    PRT_SEG("Running Task\r\n");
    while(1) {
        xSemaphoreTake(seg_u2e_sem, portMAX_DELAY);
        //PRT_SEG("xSemaphoreTake(seg_u2e_sem, portMAX_DELAY)\r\n");

        switch (serial_mode)
        {
            case SEG_SERIAL_PROTOCOL_NONE :
                if(get_uart_buffer_usedsize() || u2e_size)
                {
                    if ((network_connection->working_mode == TCP_MIXED_MODE) && (mixed_state == MIXED_SERVER) && (ST_OPEN == get_device_status())) {
                        process_socket_termination(SEG_DATA0_SOCK, SOCK_TERMINATION_DELAY);
                        mixed_state = MIXED_CLIENT;
                        xSemaphoreGive(seg_sem);
                    }
                    else if((ST_CONNECT == get_device_status()) || network_connection->working_mode == UDP_MODE)
                        uart_to_ether(SEG_DATA0_SOCK);
                }
                break;

            case SEG_SERIAL_MODBUS_RTU :
                RTU_Uart_RX();
                if(mb_state_rtu_finish == TRUE) {
                    mb_state_rtu_finish = FALSE;
                    mbRTUtoTCP(SEG_DATA0_SOCK);
                }
                break;
            
            case SEG_SERIAL_MODBUS_ASCII :
                ASCII_Uart_RX();
                if(mb_state_ascii_finish == TRUE) {
                    mb_state_ascii_finish = FALSE;
                    mbASCIItoTCP(SEG_DATA0_SOCK);
                }
                break;
        }
#ifdef __USE_WATCHDOG__
        device_wdt_reset();
#endif
    }
}

void seg_recv_task (void *argument)  {
    uint8_t serial_mode = get_serial_communation_protocol();

    while(1) {
        xSemaphoreTake(seg_e2u_sem, portMAX_DELAY);
        switch (serial_mode)
        {
            case SEG_SERIAL_PROTOCOL_NONE :
                ether_to_uart(SEG_DATA0_SOCK);
                break;

            case SEG_SERIAL_MODBUS_RTU :
                mbTCPtoRTU(SEG_DATA0_SOCK);
                break;
            
            case SEG_SERIAL_MODBUS_ASCII :
                mbTCPtoASCII(SEG_DATA0_SOCK);
                break;
        }
#ifdef __USE_WATCHDOG__
        device_wdt_reset();
#endif
    }
}

void timers_stop(void)
{
    if (seg_inactivity_timer != NULL)
        xTimerStop(seg_inactivity_timer, 0);
    
    if (seg_keepalive_timer != NULL) 
        xTimerStop(seg_keepalive_timer, 0);
        
    if (seg_auth_timer != NULL) 
        xTimerStop(seg_auth_timer, 0);  
}

void keepalive_timer_callback( TimerHandle_t xTimer )
{
    flag_send_keepalive = SEG_ENABLE;
    xSemaphoreGive(seg_timer_sem);
}

void inactivity_timer_callback( TimerHandle_t xTimer )
{
    flag_inactivity = SEG_ENABLE;
    xSemaphoreGive(seg_timer_sem);
}

void auth_timer_callback( TimerHandle_t xTimer )
{
    flag_auth_time = SEG_ENABLE;
    xSemaphoreGive(seg_timer_sem);
}

void seg_timer_task (void *argument)  {
    struct __tcp_option *tcp_option = (struct __tcp_option *)&(get_DevConfig_pointer()->tcp_option);

    while(1) {
        xSemaphoreTake(seg_timer_sem, portMAX_DELAY);
        if((flag_inactivity == SEG_ENABLE))
        {
#ifdef _SEG_DEBUG_
            PRT_SEG(" > INACTIVITY TIMER: TIMEOUT\r\n");
            flag_inactivity = SEG_DISABLE;
#endif
            process_socket_termination(SEG_DATA0_SOCK, SOCK_TERMINATION_DELAY);
        }

        if(flag_send_keepalive == SEG_ENABLE)
        {
//#ifdef _SEG_DEBUG_
//            PRT_SEG(" >> send_keepalive_packet\r\n");
//#endif
            flag_send_keepalive = SEG_DISABLE;
            send_keepalive_packet_manual(SEG_DATA0_SOCK); // <-> send_keepalive_packet_auto()
            if (xTimerGetPeriod(seg_keepalive_timer) != pdMS_TO_TICKS(tcp_option->keepalive_retry_time))
                xTimerChangePeriod(seg_keepalive_timer, pdMS_TO_TICKS(tcp_option->keepalive_retry_time), 0);
            xTimerStart(seg_keepalive_timer, 0);
        }
        
        // Check the connection password auth timer
        if(tcp_option->pw_connect_en == SEG_ENABLE)
        {
            if((flag_auth_time == SEG_ENABLE) && (flag_connect_pw_auth == SEG_DISABLE))
            {
                flag_auth_time = SEG_DISABLE;

#ifdef _SEG_DEBUG_
                PRT_SEG(" > CONNECTION PW: AUTH TIMEOUT\r\n");
#endif
                process_socket_termination(SEG_DATA0_SOCK, SOCK_TERMINATION_DELAY);
            }
        }
    }
}
