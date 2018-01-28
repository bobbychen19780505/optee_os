/*
 * Copyright (C) Microsoft. All rights reserved
 */

#include <arm.h>
#include <kernel/misc.h>
#include <kernel/pseudo_ta.h>
#include <kernel/user_ta.h>
#include <kernel/thread.h>
#include <mm/core_memprot.h>
#include <mm/tee_mmu.h>
#include <optee_msg_supplicant.h>
#include <utee_defines.h>
#include <string.h>
#include <drivers/imx_uart.h>

#include <pta_gt511c3.h>

#define UART_DEBUG 0

/*
 * Baud rate after reset
 */
#define GT511C3_RESET_BAUDRATE 9600

/*
 * Max baud rate
 */
#define GT511C3_MAX_BAUDRATE 115200

/*
 * Max payload that can be read from scanner
 */
#define GT511C3_MAX_PAYLOAD (64 * 1024)


/*
 * GT511C3 error codes
 */

enum gt511C3_status {
    GT511C3_SUCCESS = 0x0000,	                    // Success
    GT511C3_STATUS_TIMEOUT = 0x1001,	            // Obsolete, capture timeout 
    GT511C3_STATUS_INVALID_BAUDRATE = 0x1002,	    // Obsolete, Invalid serial baud rate 
    GT511C3_STATUS_INVALID_POS = 0x1003,	        // The specified ID is not between 0~199 
    GT511C3_STATUS_IS_NOT_USED = 0x1004,	        // The specified ID is not used 
    GT511C3_STATUS_IS_ALREADY_USED = 0x1005,	    // The specified ID is already used 
    GT511C3_STATUS_COMM_ERR = 0x1006,	            // Communication Error 
    GT511C3_STATUS_VERIFY_FAILED = 0x1007,	        // 1:1 Verification Failure 
    GT511C3_STATUS_IDENTIFY_FAILED = 0x1008,	    // 1:N Identification Failure 
    GT511C3_STATUS_DB_IS_FULL = 0x1009,	            // The database is full 
    GT511C3_STATUS_DB_IS_EMPTY = 0x100A,	        // The database is empty 
    GT511C3_STATUS_TURN_ERR = 0x100B,	            // Obsolete, Invalid order of the enrollment (The order was not as: EnrollStart -> Enroll1 -> Enroll2 -> Enroll3) 
    GT511C3_STATUS_BAD_FINGER = 0x100C,	            // Too bad fingerprint 
    GT511C3_STATUS_ENROLL_FAILED = 0x100D,	        // Enrollment Failure 
    GT511C3_STATUS_IS_NOT_SUPPORTED = 0x100E,	    // The specified command is not supported 
    GT511C3_STATUS_DEV_ERR = 0x100F,	            // Device Error, especially if Crypto-Chip is trouble 
    GT511C3_STATUS_CAPTURE_CANCELED = 0x1010,	    // Obsolete, The capturing is canceled 
    GT511C3_STATUS_INVALID_PARAM = 0x1011,	        // Invalid parameter 
    GT511C3_STATUS_FINGER_IS_NOT_PRESSED = 0x1012,  // Finger is not pressed 
    GT511C3_STATUS_INVALID = 0XFFFF	                // Used when parsing fails 
};


/*
 * GT511C3 command codes
 */

enum gt511C3_commands {
    GT511C3_CMD_INVALID = 0x00,
    GT511C3_CMD_OPEN = 0x01,                // Open Initialization 
    GT511C3_CMD_CLOSE = 0x02,               // Close Termination 
    GT511C3_CMD_USB_INTERNAL_CHECK = 0x03,  // UsbInternalCheck Check if the connected USB device is valid 
    GT511C3_CMD_CHANGE_BAUD_RATE = 0x04,    // ChangeBaudrate Change UART baud rate 
    GT511C3_CMD_SET_IAP_MODE = 0x05,        // SetIAPMode Enter IAP Mode In this mode, FW Upgrade is available 
    GT511C3_CMD_CMOS_LED = 0x12,            // CmosLed Control CMOS LED 
    GT511C3_CMD_GET_ENROLL_COUNT = 0x20,	// Get enrolled fingerprint count 
    GT511C3_CMD_CHECK_ENROLLED = 0x21,		// Check whether the specified ID is already enrolled 
    GT511C3_CMD_ENROL_START = 0x22,		    // Start an enrollment 
    GT511C3_CMD_ENROLL1 = 0x23,		        // Make 1st template for an enrollment 
    GT511C3_CMD_ENROLL2 = 0x24,		        // Make 2nd template for an enrollment 
    GT511C3_CMD_ENROLL3 = 0x25,		        // Make 3rd template for an enrollment, merge three templates into one template, save merged template to the database 
    GT511C3_CMD_IS_PRESS_FINGER = 0x26,		// Check if a finger is placed on the sensor 
    GT511C3_CMD_DELETE_ID = 0x40,		    // Delete the fingerprint with the specified ID 
    GT511C3_CMD_DELETE_ALL = 0x41,		    // Delete all fingerprints from the database 
    GT511C3_CMD_VERIFY1_1 = 0x50,		    // Verification of the capture fingerprint image with the specified ID 
    GT511C3_CMD_IDENTIFY1_N = 0x51,		    // Identification of the capture fingerprint image with the database 
    GT511C3_CMD_VERIFY_TEMPLATE1_1 = 0x52,  // Verification of a fingerprint template with the specified ID 
    GT511C3_CMD_IDENTIFY_TEMPLATE1_N = 0x53,// Identification of a fingerprint template with the database 
    GT511C3_CMD_CAPTURE_FINGER = 0x60,		// Capture a fingerprint image(256x256) from the sensor 
    GT511C3_CMD_MAKE_TEMPLATE = 0x61,		// Make template for transmission 
    GT511C3_CMD_GET_IMAGE = 0x62,		    // Download the captured fingerprint image(256x256) 
    GT511C3_CMD_GET_RAW_IMAGE = 0x63,		// Capture & Download raw fingerprint image(320x240) 
    GT511C3_CMD_GET_TEMPLATE = 0x70,        // Download the template of the specified ID 
    GT511C3_CMD_SET_TEMPLATE = 0x71,		// Upload the template of the specified ID 
    GT511C3_CMD_GET_DATABASE_START = 0x72,	// Start database download, obsolete 
    GT511C3_CMD_GET_DATABASE_END = 0x73,	// End database download, obsolete 
    GT511C3_CMD_UPGRADE_FIRMWARE = 0x80,    // Not supported 
    GT511C3_CMD_UPGRADE_ISOCD_IMAGE = 0x81,	// Not supported 
    GT511C3_CMD_ACK = 0x30,		            // Acknowledge. 
    GT511C3_CMD_NACK = 0x31		            // Non-acknowledge 
};


/*
 * Communication protocol related definitions
 */

/*
 * Command frame
 */

enum gt511C3_command_frame_codes {
    GT511C3_CMD_START_CODE1 = 0x55,
    GT511C3_CMD_START_CODE2 = 0xAA,

    GT511C3_CMD_DEVICE_ID = 0x0001
};

struct _gt511C3_command {
    uint8_t start_code1;    // #0 : GT511C3_CMD_START_CODE1
    uint8_t start_code2;    // #1 : GT511C3_CMD_START_CODE2
    int16_t device_id;      // #2 : Device ID: always 0x0001
    uint32_t parameter;     // #4 : Input parameter
    int16_t command;        // #8 : Command code: gt511C3_commands
    int16_t checksum;       // #10: Checksum (byte addition)
} __packed;
typedef struct _gt511C3_command gt511C3_command;


/*
 * Response frame
 */

enum GT511C3_response_frame_codes {
    GT511C3_RSP_START_CODE1 = 0x55,
    GT511C3_RSP_START_CODE2 = 0xAA,

    GT511C3_RSP_DEVICE_ID = 0x0001,

    GT511C3_RSP_ACK = 0x30,
    GT511C3_RSP_NACK = 0x31
};

struct _gt511C3_response {
    uint8_t start_code1;    // #0 : GT511C3_RSP_START_CODE1
    uint8_t start_code2;    // #1 : GT511C3_RSP_START_CODE2
    int16_t device_id;      // #2 : Device ID: always 0x0001
    uint32_t parameter;     // #4 : If response is GT511C3_RSP_ACK -> Output parameter
                            //      If response is GT511C3_RSP_NACK -> Error code
    int16_t response;       // #8 : Response: GT511C3_RSP_ACK/GT511C3_RSP_ACK
    int16_t checksum;       // #10: Checksum (byte addition)
} __packed;
typedef struct _gt511C3_response gt511C3_response;


/*
 * Data frame
 */

enum GT511C3_data_frame_codes {
    GT511C3_DATA_START_CODE1 = 0x5A,
    GT511C3_DATA_START_CODE2 = 0xA5,

    GT511C3_DATA_DEVICE_ID = 0x0001,
};

struct _gt511C3_data {
    uint8_t start_code1;    // #0 : GT511C3_DATA_START_CODE1
    uint8_t start_code2;    // #1 : GT511C3_DATA_START_CODE2
    int16_t device_id;      // #2 : Device ID: always 0x0001
    uint8_t data[1];        // #4 : Data bytes N
                            // ...
                            //int16_t checksum;     // #4 + N: Checksum (byte addition)
} __packed;
typedef struct _gt511C3_data gt511C3_data;

union _gt511C3_data_frame {
    gt511C3_data frame;
    uint8_t raw[GT511C3_MAX_PAYLOAD];
};
typedef union _gt511C3_data_frame gt511C3_data_frame;


/*
 * Auxiliary macros
 */

#define GT511C3_INIT_COMMAND(cmd_frame, cmd_code) { \
    memset((cmd_frame),0,sizeof(gt511C3_command)); \
    (cmd_frame)->start_code1 = GT511C3_CMD_START_CODE1; \
    (cmd_frame)->start_code2 = GT511C3_CMD_START_CODE2; \
    (cmd_frame)->device_id = GT511C3_CMD_DEVICE_ID; \
    (cmd_frame)->command = (cmd_code); \
}

#define GT511C3_DATA_FRAME_SIZE(payload_size) \
    (sizeof(gt511C3_data) - sizeof(uint8_t) + (payload_size) + sizeof(int16_t))

#define FIELD_OFFSET(type, field) ((uint32_t)&(((type *)0)->field))
#define FIELD_SIZE(type, field) (sizeof(((type *)0)->field))

/*
 * GT511C3 interface
 */

TEE_Result gt511C3_open(GT511C3_DeviceConfig *device_config,
            GT511C3_DeviceInfo *device_info __maybe_unused);

TEE_Result gt511C3_close(void);


/*
 * GT511C3 driver globals
 */

/*
 * The UART driver
 */
static struct imx_uart_data uart_driver;
static struct serial_chip* serial = NULL;


/*
 * GT511C3 driver private methods
 */

static TEE_Result gt511C3_to_tee(uint32_t status)
{
    switch (status) {
    case GT511C3_SUCCESS:
        return TEE_SUCCESS;

    case GT511C3_STATUS_TIMEOUT:
        return TEE_ERROR_COMMUNICATION;

    case GT511C3_STATUS_INVALID_BAUDRATE:
        return TEE_ERROR_BAD_PARAMETERS;

    case GT511C3_STATUS_INVALID_POS:
        return TEE_ERROR_BAD_STATE;

    case GT511C3_STATUS_IS_NOT_USED:
        return TEE_ERROR_BAD_STATE;

    case GT511C3_STATUS_IS_ALREADY_USED:
        return TEE_ERROR_BUSY;

    case GT511C3_STATUS_COMM_ERR:
        return TEE_ERROR_COMMUNICATION;

    case GT511C3_STATUS_VERIFY_FAILED:
        return TEE_ERROR_ACCESS_DENIED;

    case GT511C3_STATUS_IDENTIFY_FAILED:
        return TEE_ERROR_ACCESS_DENIED;

    case GT511C3_STATUS_DB_IS_FULL:
        return TEE_ERROR_OUT_OF_MEMORY;

    case GT511C3_STATUS_DB_IS_EMPTY:
        return TEE_ERROR_NO_DATA;

    case GT511C3_STATUS_TURN_ERR:
        return TEE_ERROR_BAD_STATE;

    case GT511C3_STATUS_BAD_FINGER:
        return TEE_ERROR_BAD_STATE;

    case GT511C3_STATUS_ENROLL_FAILED:
        return TEE_ERROR_BAD_STATE;

    case GT511C3_STATUS_IS_NOT_SUPPORTED:
        return TEE_ERROR_NOT_SUPPORTED;

    case GT511C3_STATUS_DEV_ERR:
        return TEE_ERROR_BAD_STATE;

    case GT511C3_STATUS_CAPTURE_CANCELED:
        return TEE_ERROR_CANCEL;

    case GT511C3_STATUS_INVALID_PARAM:
        return TEE_ERROR_BAD_PARAMETERS;

    case GT511C3_STATUS_FINGER_IS_NOT_PRESSED:
        return TEE_ERROR_BAD_STATE;

    case GT511C3_STATUS_INVALID:
        return TEE_ERROR_GENERIC;

    default:
        return TEE_ERROR_GENERIC;
    }
}

static int16_t gt511C3_checksum(uint8_t *msg, uint16_t length)
{
    uint16_t i;
    int16_t cs;

    cs = 0;
    for (i = 0; i < length; ++i) cs += msg[i];

    return cs;
}

static TEE_Result gt511C3_init(GT511C3_DeviceConfig *device_config, bool reset)
{
    struct imx_uart_config uart_config = {
        .clock_hz = device_config->uart_clock_hz,
        .baud_rate = reset ? GT511C3_RESET_BAUDRATE : device_config->baud_rate
    };

    if (uart_config.baud_rate > GT511C3_MAX_BAUDRATE) {
        return TEE_ERROR_BAD_PARAMETERS;
    }

    if (reset) {
        if (device_config->uart_base_pa == 0) {
            return TEE_ERROR_BAD_PARAMETERS;
        }
    }

    if (!imx_uart_init_ex(&uart_driver, 
            device_config->uart_base_pa, 
            &uart_config)) {
                
        return TEE_ERROR_BAD_PARAMETERS;
    }

    serial = &uart_driver.chip;
    
    return TEE_SUCCESS;
}

static TEE_Result gt511C3_recv(uint8_t *msg, uint32_t *length)
{
    int16_t msg_cs;
    int16_t calculated_cs;
    uint16_t cs_length;
    uint16_t i;

    for (i = 0; i < *length; ++i) {
        /*
         * TODO implement RX timeout and use
         * serial->ops->have_rx_data(serial)
         */
        msg[i] = (uint8_t)serial->ops->getchar(serial);
    }
    *length = i;
    
    cs_length = i - sizeof(uint16_t);

    msg_cs = (int16_t)((msg[cs_length + 1] << 8) + msg[cs_length]);
    calculated_cs = gt511C3_checksum(msg, cs_length);

    if (msg_cs != calculated_cs) {
        return TEE_ERROR_COMMUNICATION;
    }
    
    return TEE_SUCCESS;
}

static TEE_Result gt511C3_recv_response(gt511C3_response *rsp)
{
    TEE_Result status;
    uint32_t rsp_length;

    rsp_length = sizeof(gt511C3_response);
    status = gt511C3_recv((uint8_t *)rsp, &rsp_length);
    if (status != TEE_SUCCESS) {
        return status;
    }

    if ((rsp->start_code1 != GT511C3_RSP_START_CODE1) ||
        (rsp->start_code2 != GT511C3_RSP_START_CODE2)) {
        return TEE_ERROR_COMMUNICATION;
    }

    if (rsp->response == GT511C3_RSP_NACK) {
        return gt511C3_to_tee(rsp->parameter);
    }

    return TEE_SUCCESS;
}

static TEE_Result gt511C3_recv_data(uint8_t* data, uint32_t length)
{
    static gt511C3_data_frame data_frame;
    size_t recv_bytes;
    TEE_Result status;

    recv_bytes = GT511C3_DATA_FRAME_SIZE(length);
    if (recv_bytes > sizeof(data_frame)) {
        return TEE_ERROR_SHORT_BUFFER;
    }

    status = gt511C3_recv(&data_frame.raw[0], &recv_bytes);
    if (status != TEE_SUCCESS) {
        return status;
    }

    if ((data_frame.frame.start_code1 != GT511C3_DATA_START_CODE1) ||
        (data_frame.frame.start_code2 != GT511C3_DATA_START_CODE2)) {
        return TEE_ERROR_COMMUNICATION;
    }

    memcpy(data, data_frame.frame.data, length);
    return TEE_SUCCESS;
}


static TEE_Result gt511C3_send_cmd(
    gt511C3_command *cmd,
    gt511C3_response *rsp __maybe_unused)
{
    TEE_Result status;
    uint8_t *payload = (uint8_t *)cmd;
    gt511C3_response local_rsp;
    uint32_t i;

    if (rsp == NULL) {
        rsp = &local_rsp;
    }

    cmd->checksum = gt511C3_checksum((uint8_t*)cmd, 
        FIELD_OFFSET(gt511C3_command, checksum));

    for (i = 0; i < sizeof(gt511C3_command); ++i) {
        serial->ops->putc(serial, payload[i]);
    }
 
    status = gt511C3_recv_response(rsp);
    if (status != TEE_SUCCESS) {
        return status;
    }

    return TEE_SUCCESS;
}

#if UART_DEBUG
static bool
uart_test(void)
{
    static uint8_t uart_test_in[10] = "12345678";
    uint8_t uart_in[8];
    uint8_t uart_out[10] = "UART Test";
    uint32_t i;

    for (i = 0; i < sizeof(uart_out); ++i) {
        serial->ops->putc(serial, uart_out[i]);
    }

    for (i = 0; i < sizeof(uart_test_in); ++i) {
        uart_in[i] = (uint8_t)serial->ops->getchar(serial);

        if (uart_in[i] != uart_test_in[i]) {
            return false;
        }
    }

    return true;
}
#endif // UART_DEBUG


/*
 * GT511C3 interface implementation
 */

static TEE_Result gt511c3_open(GT511C3_DeviceConfig *device_config,
                    GT511C3_DeviceInfo *device_info __maybe_unused)
{
    TEE_Result status;
    gt511C3_command cmd;

    status = gt511C3_init(device_config, true);
    if (status != TEE_SUCCESS) {
        EMSG("Initial gt511C3_init failed, status 0x%X \n", status);
        return status;
    }

#if UART_DEBUG
    if (!uart_test()) {
        return TEE_ERROR_BAD_STATE;
    }
#endif
  
    GT511C3_INIT_COMMAND(&cmd, GT511C3_CMD_OPEN);
    if (device_info != NULL) {
        cmd.parameter = 1;
    }

    status = gt511C3_send_cmd(&cmd, NULL);
    if (status != TEE_SUCCESS) {
        return status;
    }

    if (device_info != NULL) {
        status = gt511C3_recv_data((uint8_t*)device_info, sizeof(*device_info));
    }

    /*
    if (device_config->baud_rate != GT511C3_RESET_BAUDRATE) {
        status = gt511C3_init(device_config, false);
        if (status != TEE_SUCCESS) {
            EMSG("gt511C3_init failed, status 0x%X \n", status);
            return status;
        }
    }
    */

    return status;
}

static TEE_Result gt511c3_close(void)
{
    gt511C3_command cmd;
    TEE_Result status;

    GT511C3_INIT_COMMAND(&cmd, GT511C3_CMD_CLOSE);
    status = gt511C3_send_cmd(&cmd, NULL);

    return status;
}

/*
 * Command handlers
 */

static TEE_Result gt511c3_cmd_initialize(uint32_t param_types,
                    TEE_Param params[TEE_NUM_PARAMS])
{
    TEE_Result status;
    uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
        TEE_PARAM_TYPE_MEMREF_INOUT,
        TEE_PARAM_TYPE_NONE,
        TEE_PARAM_TYPE_NONE);

    DMSG("gt511c3_initialize\n");

    if (exp_pt != param_types) {
        return TEE_ERROR_BAD_PARAMETERS;
    }

    status = gt511c3_open(
        (GT511C3_DeviceConfig *)params[0].memref.buffer,
        (GT511C3_DeviceInfo *)params[1].memref.buffer);

    if (status != TEE_SUCCESS) {
        return status;
    }

    return TEE_SUCCESS;
}

static TEE_Result gt511c3_cmd_exec(uint32_t param_types,
                    TEE_Param params[TEE_NUM_PARAMS])
{
    TEE_Result status;
    gt511C3_command cmd;
    uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
        TEE_PARAM_TYPE_MEMREF_INOUT,
        TEE_PARAM_TYPE_NONE,
        TEE_PARAM_TYPE_NONE);

    DMSG("gt511c3_cmd_exec\n");

    if (exp_pt != param_types) {
        return TEE_ERROR_BAD_PARAMETERS;
    }
   
    GT511C3_INIT_COMMAND(&cmd, params[0].value.a);
    cmd.parameter = params[0].value.b;

    status = gt511C3_send_cmd(&cmd, NULL);
    if (status != TEE_SUCCESS) {
        return status;
    }

    return gt511C3_recv_data((uint8_t*)params[1].memref.buffer, 
            params[1].memref.size);
}

/* 
 * Trusted Application Entry Points
 */

static TEE_Result pta_gt511c3_open_session(uint32_t param_types __unused,
                    TEE_Param params[TEE_NUM_PARAMS] __unused,
                    void **sess_ctx __unused)
{
    DMSG("gt511c3 open session succeeded!\n");
    return TEE_SUCCESS;
}

static void pta_gt511c3_close_session(void *sess_ctx __unused)
{
    TEE_Result status;

    status = gt511c3_close();
    if (status != TEE_SUCCESS) {
        EMSG("gt511c3 close failed, status 0x%X!\n", status);
    } else {
        DMSG("gt511c3 open session succeeded!\n");
    }
}

static TEE_Result pta_gt511c3_invoke_command(void *sess_ctx __unused, uint32_t cmd_id,
				 uint32_t param_types,
				 TEE_Param params[TEE_NUM_PARAMS])
{
    TEE_Result res;
    DMSG("gt511c3 invoke command %d\n", cmd_id);

    switch (cmd_id) {
    case PTA_GT511C3_INIT:
        res = gt511c3_cmd_initialize(param_types, params);
        break;

    case PTA_GT511C3_EXEC:
        res = gt511c3_cmd_exec(param_types, params);
        break;
        
    default:
        EMSG("Command not implemented %d\n", cmd_id);
        res = TEE_ERROR_NOT_IMPLEMENTED;
        break;
    }

    return res;
}

pseudo_ta_register(.uuid = PTA_GT511C3_UUID, .name = "pta_gt511c3",
		   .flags = PTA_DEFAULT_FLAGS,
		   .open_session_entry_point = pta_gt511c3_open_session,
           .close_session_entry_point = pta_gt511c3_close_session,
		   .invoke_command_entry_point = pta_gt511c3_invoke_command);
