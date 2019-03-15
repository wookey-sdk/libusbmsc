#include "api/malloc.h"
#include "api/stdio.h"
#include "api/nostd.h"
#include "api/string.h"
#include "api/scsi.h"

#include "usb.h"
#include "usb_bbb.h"
#include "queue.h"
#include "debug.h"
#include "autoconf.h"
#include "api/syscall.h"
#include "wookey_ipc.h"

#define assert(val) if (!(val)) { while (1) ; };

#define SCSI_DEBUG 1

#define MAX_SCSI_CMD_QUEUE_SIZE 10

static scsi_calbacks_t scsi_cb = {
   .read = NULL,
   .write = NULL,
   .get_storage_capacity = NULL,
   .get_storage_block_size = NULL
};



typedef enum scsi_state {
    SCSI_IDLE = 0x00,
    SCSI_READ,
    SCSI_WRITE,
    SCSI_ERROR,
} scsi_state_t;

/*
 * The SCSI stack context. This is a global variable, which means
 * that the SCSI stack is not reentrant (not for scsi_context write access).
 * As most micro-controlers are not multicore based, this should not be
 * a problem.
 */
typedef enum {
   SCSI_TRANSMIT_LINE_READY = 0,
   SCSI_TRANSMIT_LINE_BUSY,
   SCSI_TRANSMIT_LINE_ERROR,
} transmition_line_state_t;

typedef enum {
    SCSI_DIRECTION_IDLE = 0,
    SCSI_DIRECTION_SEND,
    SCSI_DIRECTION_RECV,
} transmission_direction_t;

typedef enum {
    TOTO,
    SCSI_ERROR_INVALID_COMMAND = 0x52000,

} scsi_error_t;


typedef  struct {
    transmission_direction_t direction;
    transmition_line_state_t line_state;
    uint32_t size_to_process;
    uint32_t addr;
    scsi_error_t error;
    struct queue *queue;
    bool queue_empty;
    uint8_t *global_buf;
    uint16_t global_buf_len;
    scsi_state_t state;
    uint32_t scsi_block_size;
} scsi_context_t;


scsi_context_t scsi_ctx = {
    .direction = SCSI_DIRECTION_IDLE,
    .line_state = SCSI_TRANSMIT_LINE_READY,
    .size_to_process = 0,
    .addr = 0,
    .error= 0,
    .queue=NULL,
    .queue_empty = true,
    .global_buf = NULL,
    .state = SCSI_IDLE,
    .global_buf_len = 0,
    .scsi_block_size = 0,
};






/****************************************************************
 * SCSI state automaton formal definition and associate utility
 * functions
 ***************************************************************/

/*
 * all allowed transitions and target states for each current state
 * empty fields are set as 0xff/0xff for request/state couple, which is
 * an inexistent state and request
 *
 * This table associate each state of the DFU automaton with up to
 * 5 potential allowed transitions/next_state couples. This permit to
 * easily detect:
 *    1) authorized transitions, based on the current state
 *    2) next state, based on the current state and current transition
 *
 * If the next_state for the current transision is keeped to 0xff, this
 * means that the current transition for the current state may lead to
 * multiple next state depending on other informations. In this case,
 * the transition handler has to handle this manually.
 */

# define MAX_TRANSITION_STATE 12

/*
 * Association between a request and a transition to a next state. This couple
 * depend on the current state and is use in the following structure
 */
typedef struct scsi_operation_code_transition {
    uint8_t    request;
    uint8_t    target_state;
} scsi_operation_code_transition_t;


static const struct {
    scsi_state_t          state;
    scsi_operation_code_transition_t  req_trans[MAX_TRANSITION_STATE];
} scsi_automaton[] = {
    { SCSI_IDLE,               {
                                {SCSI_CMD_INQUIRY,SCSI_IDLE},
                                {SCSI_CMD_MODE_SELECT_10,SCSI_IDLE},
                                {SCSI_CMD_MODE_SENSE_10,SCSI_IDLE},
                                {SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL,SCSI_IDLE},
                                {SCSI_CMD_READ_10,SCSI_IDLE},
                                {SCSI_CMD_READ_CAPACITY_10,SCSI_IDLE},
                                {SCSI_CMD_READ_FORMAT_CAPACITIES,SCSI_IDLE},
                                {SCSI_CMD_REPORT_LUNS,SCSI_IDLE},
                                {SCSI_CMD_REQUEST_SENSE,SCSI_IDLE},
                                {SCSI_CMD_SEND_DIAGNOSTIC,SCSI_IDLE},
                                {SCSI_CMD_TEST_UNIT_READY,SCSI_IDLE},
                                {SCSI_CMD_WRITE_10,SCSI_IDLE},
                             }
    },
    { SCSI_READ,     {
                                 {SCSI_CMD_READ_10,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},


                             }
    },
    { SCSI_WRITE,     {
                                 {SCSI_CMD_WRITE_10,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                             }
    },
    { SCSI_ERROR,     {
                                 {SCSI_CMD_MODE_SENSE_10, SCSI_IDLE},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                                 {0xff,0xff},
                             },
    },

};

/**********************************************
 * SCSI getters and setters
 *********************************************/


static inline scsi_state_t scsi_get_state() {
    return scsi_ctx.state;
}


static inline void scsi_set_state(const scsi_state_t new_state)
{
    if (new_state == 0xff) {
        printf("%s: PANIC! this should never arise !", __func__);
        while (1) {}; //FIXME
        return;
    }
#if SCSI_DEBUG
    printf("state: %x => %x\n", scsi_ctx.state, new_state);
#endif
    scsi_ctx.state = new_state;
}

void scsi_error(scsi_error_t reason){
#if SCSI_DEBUG
    printf("%s: %s: status=%d\n", __func__, __func__, reason);
    printf("%s: state -> Error\n",__func__);
#endif
    scsi_ctx.error = reason;
    usb_bbb_send_csw(CSW_STATUS_FAILED, 0);
    scsi_set_state(SCSI_IDLE);
}

/******************************************************
 * SCSI automaton management function (transition and
 * state check)
 *****************************************************/

/*!
 * \brief return the next automaton state
 *
 * The next state is returned depending on the current state
 * and the current request. In some case, it can be 0xff if multiple
 * next states are possible.
 *
 * \param current_state the current automaton state
 * \param request       the current transition request
 *
 * \return the next state, or 0xff
 */
static uint8_t scsi_next_state(scsi_state_t  current_state, scsi_operation_code_t    request)
{
    for (uint8_t i = 0; i < MAX_TRANSITION_STATE; ++i) {
        if (scsi_automaton[current_state].req_trans[i].request == request) {
            return (scsi_automaton[current_state].req_trans[i].target_state);
        }
    }
    /* fallback, no corresponding request found for  this state */
    return 0xff;
}

/*!
 * \brief Specify if the current request is valid for the current state
 *
 * \param current_state the current automaton state
 * \param request       the current transition request
 *
 * \return true if the transition request is allowed for this state, or false
 */
static bool scsi_is_valid_transition(scsi_state_t current_state,
        scsi_operation_code_t    request)
{
    for (uint8_t i = 0; i < MAX_TRANSITION_STATE; ++i) {
        if (scsi_automaton[current_state].req_trans[i].request == request) {
            return true;
        }
    }
    /*
     * Didn't find any request associated to current state. This is not a
     * valid transition. We should stall the request.
     */
    printf("%s: invalid transition from state %d, request %d\n", __func__, current_state, request);
    scsi_set_state(SCSI_ERROR);
    return false;
}

/*********************************************************************
 * Mutexes, protection against race conditions...
 ********************************************************************/

/*
 * \brief entering a critical section
 *
 * During this critical section, any ISR is postponed to avoid any
 * race condition on variables shared in write-access between ISR and
 * main thread. See sys_lock() syscall API documentation.
 *
 * Critical sections must be as short as possible to avoid border
 * effects such as latency increase and ISR queue overloading.
 */
static inline void enter_critical_section(void)
{
    uint8_t ret;
    ret = sys_lock (LOCK_ENTER); /* Enter in critical section */
    if(ret != SYS_E_DONE){
        printf("%s: Error: failed entering critical section!\n", __func__);
    }
    return;
}

/*
 * \brief leaving a critical section
 *
 * Reallow the execution of the previously postponed task ISR.
 */
static inline void leave_critical_section(void)
{
    uint8_t ret;
    ret = sys_lock (LOCK_EXIT);  /* Exit from critical section */
    if(ret != SYS_E_DONE){
        printf("Error: failed exiting critical section!\n");
    }
    return;
}

/****************** END OF AUTOMATON *********************/



typedef struct  __attribute__((packed)){
     uint8_t misc1:3;
     uint8_t service_action:5;
     uint32_t logical_block;
     uint8_t misc2;
     uint16_t transfer_blocks;
     uint8_t control;
} cdb10_t;


typedef union {
   cdb10_t cdb10;
   uint32_t junk;
} u_cdb_payload;

typedef  struct  __attribute__((packed)){
    uint8_t operation;
    u_cdb_payload payload;
} cdb_t;


static void scsi_release_cdb(cdb_t * current_cdb){

	/* TODO: detect if we are in main thread or ISR mode: no need to use critical
	 * section when we are in ISR mode!
	 */
	if(current_cdb != NULL){
        enter_critical_section();
		if(wfree((void**)&current_cdb)){
			while(1){}; // FIXME
		}
        leave_critical_section();
	}
	current_cdb = NULL;
}

static inline bool scsi_is_ready_for_data_receive(void)
{
    return (   scsi_ctx.direction == SCSI_DIRECTION_RECV
            && scsi_ctx.line_state == SCSI_TRANSMIT_LINE_READY );
}


void scsi_get_data(void *buffer, uint32_t size)
{
#ifdef SCSI_DEBUG
		printf("%s: size: %d \n", __func__, size );
#endif
	while(!scsi_is_ready_for_data_receive()){
		continue;
	}
	scsi_ctx.direction = SCSI_DIRECTION_RECV;
    scsi_ctx.line_state = SCSI_TRANSMIT_LINE_READY;
	scsi_ctx.size_to_process = size;
    scsi_ctx.addr = 0;

	assert(buffer); // FIXME
	usb_bbb_read(buffer, size, 1);
}


void scsi_send_data(void *data, uint32_t size)
{
#ifdef SCSI_DEBUG
		printf("%s: size: %d \n", __func__, size );
#endif
	scsi_ctx.direction = SCSI_DIRECTION_SEND;
    scsi_ctx.line_state = SCSI_TRANSMIT_LINE_READY;
	scsi_ctx.size_to_process = size;
    scsi_ctx.addr = 0;
	usb_bbb_send(data, size, 2); // FIXME HARCODED ENDPOINT
}

void scsi_send_status(void)
{
#ifdef SCSI_DEBUG
		printf("%s:\n", __func__ );
#endif
	usb_bbb_send_csw(CSW_STATUS_SUCCESS, 0);
}


static void scsi_data_available(uint32_t size)
{

#if SCSI_DEBUG
    printf("%s: %d\n", __func__, size);
#endif

    scsi_ctx.size_to_process -= size;
    scsi_ctx.line_state = SCSI_TRANSMIT_LINE_READY;

    if (scsi_ctx.size_to_process == 0){
	    scsi_send_status();
        scsi_ctx.direction = SCSI_DIRECTION_IDLE;
        scsi_set_state(SCSI_IDLE);
    }
}


static void scsi_data_sent()
{

#if SCSI_DEBUG
    printf("%s: %d\n", __func__);
#endif

    scsi_ctx.size_to_process = 0;
    scsi_ctx.line_state = SCSI_TRANSMIT_LINE_READY;

    if (scsi_ctx.size_to_process == 0){
	    scsi_send_status();
        scsi_ctx.direction = SCSI_DIRECTION_IDLE;
        scsi_set_state(SCSI_IDLE);
    }
}




/* NB: this function is executed in a handler context when a
 * command comes from USB.
 */static void scsi_parse_cdb(uint8_t cdb[], uint8_t cdb_len __attribute__((unused)))
{
    cdb_t *current_cdb;
	int ret;

    // Only 10 byte commands are supported, bigger sized commands are trunccated
    ret = wmalloc((void**)&current_cdb, sizeof(cdb_t), ALLOC_NORMAL);
    if(ret){
        while(1){}; // FIXME
    }

    memcpy((void *)current_cdb, (void *)cdb, sizeof(cdb_t));

	queue_enqueue(scsi_ctx.queue, current_cdb);
	scsi_ctx.queue_empty = 0;
}









/*
 * Commands
 */
struct __packed inquiry_data {
	uint8_t device_type:5;
	uint8_t qualifier:3;
	uint8_t reserved1:7;
	uint8_t rmb:1;
	uint8_t version;
	uint8_t data_format:4;
	uint8_t hi_sup:1;
	uint8_t norm_aca:1;
	uint8_t reserved3:2;
	uint8_t additional_len;
	uint8_t prot:1;
	uint8_t reserved5:2;
	uint8_t pc:1;
	uint8_t tpgs:2;
	uint8_t acc:1;
	uint8_t sccs:1;
	uint8_t addr16:1;
	uint8_t reserved6_1:3;
	uint8_t multip:1;
	uint8_t vs6:1;
	uint8_t env_serv:1;
	uint8_t reserved6_7:1;
	uint8_t vs7:1;
	uint8_t cmd_que:1;
	uint8_t reserved7_2:2;
	uint8_t sync:1;
	uint8_t wbus16:1;
	uint8_t reserved7_6:2;
	char vendor_info[8];
	char product_identification[16];
	char product_revision[4];
};



static void scsi_cmd_inquiry(scsi_state_t  current_state, cdb_t * cdb)
{
	struct inquiry_data data;
#ifdef SCSI_DEBUG
		printf("%s:\n", __func__ );
#endif
    /* Sanity check */
    if(cdb == NULL){
        goto invalid_transition;
    }

    /* Sanity check and next state detection */
    uint8_t next_state;
    next_state = scsi_next_state(current_state, cdb->operation);

    if (!scsi_is_valid_transition(current_state, cdb->operation)) {
        goto invalid_transition;
    }
    next_state = scsi_next_state(current_state, cdb->operation);

    /* effective transition execution (if needed) */

	/* Most of support bits are set to 0
	 * version is 0 because the device does not claim conformance to any
	 * standard
	 */
	memset((void *)&data, 0, sizeof(data));
	data.rmb = 1;                           /* Removable media */
	data.data_format = 2;                   /* < 2 obsoletes, > 2 reserved */
	data.additional_len = sizeof(data) - 5; /* (36 - 5) bytes after this one remain */
    data.additional_len = sizeof(data) - 5; /* (36 - 5) bytes after this one remain */
    strncpy(data.vendor_info, CONFIG_USB_DEV_MANUFACTURER, sizeof(data.vendor_info));
    strncpy(data.product_identification, CONFIG_USB_DEV_PRODNAME, sizeof(data.product_identification));
    strncpy(data.product_revision, CONFIG_USB_DEV_REVISION, sizeof(data.product_revision));

    #if SCSI_DEBUG
        printf("%s: %s\n",__func__, data.product_revision);
    #endif

	usb_bbb_send((uint8_t *)&data, sizeof(data), 2);
    return;

invalid_transition:
    printf("%s: invalid_transition\n", __func__);
    scsi_error(SCSI_ERROR_INVALID_COMMAND);
    return;
}


static void scsi_cmd_prevent_allow_medium_removal(scsi_state_t  current_state, cdb_t * current_cdb)
{
    #if SCSI_DEBUG
        printf("%s\n", __func__);
    #endif

    /* Sanity check */
    if(current_cdb == NULL){
        goto invalid_transition;
    }

    /* Sanity check and next state detection */
    uint8_t next_state;
    next_state = scsi_next_state(current_state, current_cdb->operation);

    if (!scsi_is_valid_transition(current_state, current_cdb->operation)) {
        goto invalid_transition;
    }
    next_state = scsi_next_state(current_state, current_cdb->operation);

    usb_bbb_send_csw(CSW_STATUS_FAILED, 0); //FIXME
    return;
    /* effective transition execution (if needed) */

invalid_transition:
    printf("%s: invalid_transition\n", __func__);
    scsi_error(SCSI_ERROR_INVALID_COMMAND);
    return;
}


// SCSI_CMD_READ_10
static void scsi_cmd_read_data10(scsi_state_t  current_state, cdb_t * current_cdb)
{
    unsigned int i;
    unsigned int sz;
    uint32_t num_sectors;

    uint32_t rw_lba;
    uint16_t rw_size;
    uint64_t size;
    uint64_t rw_addr;


    #if SCSI_DEBUG
        printf("%s\n", __func__);
    #endif

    /* Sanity check */
    if(current_cdb == NULL){
        goto invalid_transition;
    }

    /* Sanity check and next state detection */
    uint8_t next_state;
    next_state = scsi_next_state(current_state, current_cdb->operation);

    if (!scsi_is_valid_transition(current_state, current_cdb->operation)) {
        goto invalid_transition;
    }
    next_state = scsi_next_state(current_state, current_cdb->operation);

    /* effective transition execution (if needed) */
    rw_lba = from_big32(current_cdb->payload.cdb10.logical_block);
    rw_size = from_big16(current_cdb->payload.cdb10.transfer_blocks);
    rw_addr  = (uint64_t)scsi_ctx.scsi_block_size * (uint64_t)rw_lba;
    size = scsi_ctx.scsi_block_size * rw_size;

	sz = (size < scsi_ctx.global_buf_len) ? size : scsi_ctx.global_buf_len;

    uint64_t tmp = rw_addr / (uint64_t)scsi_ctx.scsi_block_size;

    if (tmp > 0xffffffff) {
        printf("%s: PANIC! requested sector address generate int overflow !\n", __func__);
    }

    num_sectors = sz / scsi_ctx.scsi_block_size;
    #if SCSI_DEBUG
        printf("%s: sz %u, scsi_ctx.scsi_block_size: %u | sz / scsi_ctx.scsi_block_size = num_sectors to read: %u\n", __func__, sz, scsi_ctx.scsi_block_size, num_sectors);
    #endif
	for(i = scsi_ctx.global_buf_len; i <= size; i+= scsi_ctx.global_buf_len) {
        if (scsi_cb.read) {
            #if SCSI_DEBUG
                printf("%s: asking num_sectors: %u to storage app: / (%u) \n", __func__, num_sectors, (sz / scsi_ctx.scsi_block_size));
            #endif
            scsi_cb.read((uint32_t)tmp, num_sectors);
        }
        tmp += sz / scsi_ctx.scsi_block_size;
        #if SCSI_DEBUG
            printf("%s: sending data to host.\n", __func__);
        #endif

		scsi_send_data(scsi_ctx.global_buf, sz);
	}

    /* Fractional residue */
    if ((i - scsi_ctx.global_buf_len) < size) {
        #if SCSI_DEBUG
            printf("%s: sending data residue to host.\n", __func__);
        #endif
        num_sectors = (size - i + scsi_ctx.global_buf_len) / scsi_ctx.scsi_block_size;
        if (scsi_cb.read) {
            scsi_cb.read((uint32_t)tmp, num_sectors);
        }
        scsi_send_data(scsi_ctx.global_buf, size - i + scsi_ctx.global_buf_len);
        #if SCSI_DEBUG
            printf("%s: sending data residue to host: DONE\n", __func__);
        #endif

    }
    return;

invalid_transition:
    printf("%s: invalid_transition\n", __func__);
    scsi_error(SCSI_ERROR_INVALID_COMMAND);
    return;
}



// FIXME SCSI_CMD_READ_CAPACITY_10
static void scsi_cmd_read_capacity10(scsi_state_t  current_state, cdb_t * current_cdb)
{
    uint32_t storage_block_size = 0;
    uint32_t storage_size = 0;

    #if SCSI_DEBUG
        printf("%s\n", __func__);
    #endif

    /* Sanity check */
    if(current_cdb == NULL){
        goto invalid_transition;
    }

    /* Sanity check and next state detection */
    uint8_t next_state;
    next_state = scsi_next_state(current_state, current_cdb->operation);

    if (!scsi_is_valid_transition(current_state, current_cdb->operation)) {
        goto invalid_transition;
    }
    next_state = scsi_next_state(current_state, current_cdb->operation);

    uint32_t response[2];
    if (scsi_cb.get_storage_capacity) {
	    storage_size = scsi_cb.get_storage_capacity();
    #if SCSI_DEBUG
        printf("%s: storage_size: %d\n", __func__, storage_size);
    #endif

	}

    if (scsi_cb.get_storage_block_size) {
        storage_block_size = scsi_cb.get_storage_block_size();
    #if SCSI_DEBUG
        printf("%s: storage_block_size: %d\n", __func__, storage_block_size);
    #endif

    }

    assert(storage_block_size && storage_block_size); // FIXME

    //what is expected is the _LAST_ LBA address ....
    // See Working draft SCSI block cmd  5.10.2 READ CAPACITY (10)

	response[0] = to_big32(storage_size-1);
	response[1] = to_big32(storage_block_size);

    #if SCSI_DEBUG
        printf("%s: response[0]: %d response[1]: %d\n", __func__, response[0], response[1]);
    #endif

	usb_bbb_send((uint8_t *)response, sizeof(response), 2);
    return;


invalid_transition:
    printf("%s: invalid_transition\n", __func__);
    scsi_error(SCSI_ERROR_INVALID_COMMAND);
    return;
}


// FIXME SCSI_CMD_REPORT_LUNS
static void scsi_cmd_report_luns(scsi_state_t  current_state, cdb_t * current_cdb)
{
    #if SCSI_DEBUG
        printf("%s\n", __func__);
    #endif

    /* Sanity check */
    if(current_cdb == NULL){
        goto invalid_transition;
    }

    /* Sanity check and next state detection */
    uint8_t next_state;
    next_state = scsi_next_state(current_state, current_cdb->operation);

    if (!scsi_is_valid_transition(current_state, current_cdb->operation)) {
        goto invalid_transition;
    }
    next_state = scsi_next_state(current_state, current_cdb->operation);

    if (next_state != 0xff) {
        scsi_set_state(next_state);
    } else {
        goto invalid_transition;
    }
    return;
    /* effective transition execution (if needed) */

invalid_transition:
    printf("%s: invalid_transition\n", __func__);
    scsi_error(SCSI_ERROR_INVALID_COMMAND);
    return;
}


// FIXME SCSI_CMD_REQUEST_SENSE
typedef struct __packed request_sense_data {
   uint8_t error_code:7;
   uint8_t info_valid:1;
   uint8_t reserved1;
   uint8_t sense_key:4;
   uint8_t reserved:1;
   uint8_t ili:1;
   uint8_t eom:1;
   uint8_t filemark:1;
   uint8_t information[4];
   uint8_t additional_sense_length;
   uint32_t reserved8;
   uint8_t asc;
   uint8_t ascq;
   uint8_t field_replaceable_unit_code;
   uint8_t sense_key_specific[3];
} request_sense_data_t;


static void scsi_cmd_request_sense(scsi_state_t  current_state, cdb_t * current_cdb)
{
    request_sense_data_t data;

    #if SCSI_DEBUG
        printf("%s\n", __func__);
    #endif

    /* Sanity check */
    if(current_cdb == NULL){
        goto invalid_transition;
    }

    /* Sanity check and next state detection */
    uint8_t next_state;
    next_state = scsi_next_state(current_state, current_cdb->operation);

    if (!scsi_is_valid_transition(current_state, current_cdb->operation)) {
        goto invalid_transition;
    }
    next_state = scsi_next_state(current_state, current_cdb->operation);

    if (next_state != 0xff) {
        scsi_set_state(next_state);
    } else {
        goto invalid_transition;
    }

    /* effective transition execution (if needed) */
	memset((void *)&data, 0, sizeof(data));
    //FIXME test the desc bit

	data.error_code = 0x70;
	data.sense_key = SCSI_ERROR_GET_SENSE_KEY(scsi_ctx.error);
	data.additional_sense_length = 0x0a;
	data.asc = SCSI_ERROR_GET_ASC(scsi_ctx.error);
	data.ascq = SCSI_ERROR_GET_ASCQ(scsi_ctx.error);
	usb_bbb_send((uint8_t *)&data, sizeof(data), 2);
    return;


invalid_transition:
    printf("%s: invalid_transition\n", __func__);
    scsi_error(SCSI_ERROR_INVALID_COMMAND);
    return;
}

// FIXME SCSI_CMD_SEND_DIAGNOSTIC
static void scsi_cmd_send_diagnostic(scsi_state_t  current_state, cdb_t * current_cdb)
{
    #if SCSI_DEBUG
        printf("%s\n", __func__);
    #endif

    /* Sanity check */
    if(current_cdb == NULL){
        goto invalid_transition;
    }

    /* Sanity check and next state detection */
    uint8_t next_state;
    next_state = scsi_next_state(current_state, current_cdb->operation);

    if (!scsi_is_valid_transition(current_state, current_cdb->operation)) {
        goto invalid_transition;
    }
    next_state = scsi_next_state(current_state, current_cdb->operation);

    if (next_state != 0xff) {
        scsi_set_state(next_state);
    } else {
        goto invalid_transition;
    }
    return;
    /* effective transition execution (if needed) */
invalid_transition:
    printf("%s: invalid_transition\n", __func__);
    scsi_error(SCSI_ERROR_INVALID_COMMAND);
    return;
}


// FIXME SCSI_CMD_TEST_UNIT_READY
static void scsi_cmd_test_unit_ready(scsi_state_t  current_state, cdb_t * current_cdb)
{
    #if SCSI_DEBUG
        printf("%s\n", __func__);
    #endif

    /* Sanity check */
    if(current_cdb == NULL){
        printf("%s: current_cdb == NULL\n", __func__);
        goto invalid_transition;
    }

    /* Sanity check and next state detection */
    uint8_t next_state;
    next_state = scsi_next_state(current_state, current_cdb->operation);

    if (!scsi_is_valid_transition(current_state, current_cdb->operation)) {
        goto invalid_transition;
    }
    next_state = scsi_next_state(current_state, current_cdb->operation);

    /* effective transition execution (if needed) */
    #if 0
	if (sd_is_ready()) {
		usb_bbb_send_csw(CSW_STATUS_SUCCESS, 0);
	} else {
		usb_bbb_send_csw(CSW_STATUS_FAILED, 0);
		scsi_ctx.error = SCSI_ERROR_UNIT_BECOMING_READY;
        scsi_error(ERRUNKNOWN);
	}
    #else
	    usb_bbb_send_csw(CSW_STATUS_SUCCESS, 0);
        return;
    #endif

invalid_transition:
    printf("%s: invalid_transition\n", __func__);
    scsi_error(SCSI_ERROR_INVALID_COMMAND);
    return;
}

// FIXME SCSI_CMD_WRITE_10
static void scsi_write_data10(scsi_state_t  current_state, cdb_t * current_cdb)
{

	if(current_cdb == NULL){
		return;
	}
#if SCSI_DEBUG
    printf("%s:\n",__func__);
#endif
	unsigned int i;
    uint32_t num_sectors;
    unsigned int sz;

    uint32_t rw_lba;
    uint16_t rw_size;
    uint64_t size;
    uint64_t rw_addr;

    /* Sanity check */
    if(current_cdb == NULL){
        goto invalid_transition;
    }

    /* Sanity check and next state detection */
    uint8_t next_state;
    next_state = scsi_next_state(current_state, current_cdb->operation);

    if (!scsi_is_valid_transition(current_state, current_cdb->operation)) {
        goto invalid_transition;
    }
    next_state = scsi_next_state(current_state, current_cdb->operation);

    if (next_state != 0xff) {
        scsi_set_state(next_state);
    } else {
        goto invalid_transition;
    }

    rw_lba = from_big32(current_cdb->payload.cdb10.logical_block);
    rw_size = from_big16(current_cdb->payload.cdb10.transfer_blocks);
    rw_addr  = (uint64_t)scsi_ctx.scsi_block_size * (uint64_t)rw_lba;
    size = scsi_ctx.scsi_block_size * rw_size;

	sz = (size < scsi_ctx.global_buf_len) ? size : scsi_ctx.global_buf_len;

    uint64_t tmp = rw_addr / (uint64_t)scsi_ctx.scsi_block_size;

    if (tmp > 0xffffffff) {
        printf("PANIC! requested sector address generate int overflow !\n");
    }
    num_sectors = sz / scsi_ctx.scsi_block_size;

	for(i = scsi_ctx.global_buf_len; i <= size; i+= scsi_ctx.global_buf_len) {
		scsi_get_data(scsi_ctx.global_buf, sz);

        while(!scsi_is_ready_for_data_receive()){
            continue;
        }
        if (scsi_cb.write) {
            scsi_cb.write((uint32_t)tmp, num_sectors);
        }
        tmp += sz / scsi_ctx.scsi_block_size;
	}
    /* Fractional residue */
    if ((i - scsi_ctx.global_buf_len) < size) {

        // TODO: assert that size - (num * sz) *must* be a sector multiple
        scsi_get_data(scsi_ctx.global_buf, size - i + scsi_ctx.global_buf_len);

        while(!scsi_is_ready_for_data_receive()){
            continue;
        }
        num_sectors = (size - i + scsi_ctx.global_buf_len) / scsi_ctx.scsi_block_size;
        if (scsi_cb.write) {
            scsi_cb.write((uint32_t)tmp, num_sectors);
        }
    }
    return;
        /* effective transition execution (if needed) */

invalid_transition:
    printf("%s: invalid_transition\n", __func__);
    scsi_error(SCSI_ERROR_INVALID_COMMAND);
    return;
}


void scsi_exec_automaton(void)
{
    cdb_t * current_cdb = NULL;


    enter_critical_section();
	if(scsi_ctx.queue_empty == 1){
		return;
	}
	current_cdb = queue_dequeue(scsi_ctx.queue);
	if(queue_is_empty(scsi_ctx.queue)){
		scsi_ctx.queue_empty = 1;
	}
    leave_critical_section();

    scsi_state_t current_state = scsi_get_state();

	switch (current_cdb->operation) {
	case SCSI_CMD_INQUIRY:
		scsi_cmd_inquiry(current_state, current_cdb);
		break;
#if 0
	case SCSI_CMD_MODE_SELECT_10:
		scsi_cmd_mode_select(current_state, current_cdb);
		break;

	case SCSI_CMD_MODE_SENSE_10:
		scsi_cmd_mode_sense(current_state, current_cdb);
		break;
#endif
	case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
		scsi_cmd_prevent_allow_medium_removal(current_state, current_cdb);
		break;

	case SCSI_CMD_READ_10:
		scsi_cmd_read_data10(current_state, current_cdb);
		break;

	case SCSI_CMD_READ_CAPACITY_10:
		scsi_cmd_read_capacity10(current_state, current_cdb);
		break;

	case SCSI_CMD_REPORT_LUNS:
		scsi_cmd_report_luns(current_state, current_cdb);
		break;

	case SCSI_CMD_REQUEST_SENSE:
		scsi_cmd_request_sense(current_state, current_cdb);
		break;

	case SCSI_CMD_SEND_DIAGNOSTIC:
		scsi_cmd_send_diagnostic(current_state, current_cdb);
		break;

	case SCSI_CMD_TEST_UNIT_READY:
		scsi_cmd_test_unit_ready(current_state, current_cdb);
		break;

	case SCSI_CMD_WRITE_10:
		scsi_write_data10(current_state, current_cdb);
		break;

	default:
        goto invalid_command;
	};

    scsi_release_cdb(current_cdb);
    return;

invalid_command:
#if SCSI_DEBUG
    printf("%s: Unsupported command: %x  \n", __func__, current_cdb->operation);
#endif
    scsi_error(SCSI_ERROR_INVALID_COMMAND);
    return;
}


typedef enum scsi_init_error {
    SCSI_INIT_DONE = 0x00,
    SCSI_INIT_CALLBACK_ERROR,
    SCSI_INIT_BUFFER_ERROR,
} scsi_init_error_t;


uint8_t scsi_early_init(uint8_t *buf, uint16_t len, scsi_calbacks_t * init_cb){

    scsi_init_error_t error = -1;
    #if SCSI_DEBUG
        printf("%s\n", __func__);
    #endif
    if (! init_cb ) {
        error = SCSI_INIT_CALLBACK_ERROR;
        goto init_error;
    }


    if (! init_cb->read ) {
        error = SCSI_INIT_CALLBACK_ERROR;
        goto init_error;
    }


    if (! init_cb->write ) {
        error = SCSI_INIT_CALLBACK_ERROR;
        goto init_error;
    }

    if (! init_cb->get_storage_capacity ) {
        error = SCSI_INIT_CALLBACK_ERROR;
        goto init_error;
    }

    if (! init_cb->get_storage_block_size ) {
        error = SCSI_INIT_CALLBACK_ERROR;
        goto init_error;
    }

    if ( !buf ) {
        error = SCSI_INIT_BUFFER_ERROR;
        goto init_error;
    }

    if ( len <= 0 ) {
        error = SCSI_INIT_BUFFER_ERROR;
        goto init_error;
    }

    scsi_cb.read = init_cb->read;
    scsi_cb.write = init_cb->write;
    scsi_cb.get_storage_capacity = init_cb->get_storage_capacity;
    scsi_cb.get_storage_block_size = init_cb->get_storage_block_size;
    scsi_ctx.global_buf = buf;
    scsi_ctx.global_buf_len = len;

    if (scsi_cb.get_storage_block_size) {
        scsi_ctx.scsi_block_size = scsi_cb.get_storage_block_size();
        if (scsi_ctx.scsi_block_size > 0 ) {
            #if SCSI_DEBUG
                printf("%s: Asking storage block size: %u\n", __func__, scsi_ctx.scsi_block_size);
            #endif
        } else {
            printf("%s: ERROR asking storage block size: %u\n", __func__, scsi_ctx.scsi_block_size);
            goto init_error;
        }
    }

	usb_bbb_early_init(scsi_parse_cdb, scsi_data_available, scsi_data_sent);
    return 0;

init_error:
#if SCSI_DEBUG
    printf("%s: ERROR: Unable to initialize scsi stack : %x  \n", __func__, error);
#endif
    return 1;
}

/*
 * Init
 */
#define MAX_SCSI_CMD_QUEUE_SIZE 10

void scsi_init(void)
{
    #if SCSI_DEBUG
        printf("%s\n", __func__);
    #endif

    /* in USB High speed mode, the USB device is mapped (and enabled) just now */
    usb_driver_map();

	unsigned int i;

	scsi_ctx.queue = queue_create(MAX_SCSI_CMD_QUEUE_SIZE);

	for(i = 0; i < scsi_ctx.global_buf_len; i++){
		scsi_ctx.global_buf[i] = i;
	}

	/* Register our callbacks on the lower layer */
	usb_bbb_init();
    scsi_set_state(SCSI_IDLE);
}


