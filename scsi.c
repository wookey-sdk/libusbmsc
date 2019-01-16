//#include "params.h
//#include "debug.h"
#include "api/malloc.h"
#include "api/print.h"
//#include "manager.h"
#include "api/scsi.h"
//#include "sd.h"
#include "usb.h"
#include "usb_bbb.h"
#include "queue.h"
#include "debug.h"
#include "api/syscall.h"
#include "wookey_ipc.h"

#define assert(val) if (!(val)) { while (1) ; };

#define SCSI_DEBUG 0

uint32_t scsi_block_size  = 0;

#define MAX_SCSI_CMD_QUEUE_SIZE 10
struct queue *scsi_cmd_queue = NULL;

// FIXME
extern uint32_t called;

volatile uint8_t id_data_source = 0;
volatile uint8_t id_data_sink = 0;

typedef struct __attribute__((packed)) {
    uint16_t garbage; /* FIXME 64bytes aligned due to gcc bug in strd usage */
	uint64_t rw_count;
	uint64_t rw_addr;
	uint8_t cmd;
} scsi_cmd;

static volatile scsi_cmd *current_cmd = NULL;

static volatile uint32_t last_error;
static volatile int ready_for_data_send = 1;
static volatile int ready_for_data_receive = 1;

uint8_t *global_buff = 0; // FIXME should be the READ buffer
uint16_t buflen = 0;
//uint8_t global_buff[4096]  = { 0xaa };

/*
 * R/W
 */
int scsi_is_ready_for_data_send(void)
{
	return ready_for_data_send;
}

int scsi_is_ready_for_data_receive(void)
{
        return ready_for_data_receive;
}

static void scsi_release_current_cmd(void){

	/* TODO: detect if we are in main thread or ISR mode: no need to use critical
	 * section when we are in ISR mode!
	 */
	if(current_cmd != NULL){
#if 1
		e_syscall_ret ret = 0;
		/* XXX: test if we are in main thread mode to remove useless syscalls */
		//if(){
			ret = sys_lock (LOCK_ENTER); /* Enter in critical section */
			if(ret != SYS_E_DONE){
				printf("Error: failed entering critical section!\n");
			}
		//}
#endif
		if(wfree((void**)&current_cmd)){
			while(1){};
		}
#if 1
		/* XXX: test if we are in main thread mode to remove useless syscalls */
		//if(){
			ret = sys_lock (LOCK_EXIT);  /* Exit from critical section */
			if(ret != SYS_E_DONE){
				printf("Error: failed exiting critical section!\n");
			}
		//}
#endif
	}
	current_cmd = NULL;
}

static volatile uint32_t current_size_to_send = 0;
void scsi_send_data(void *data, uint32_t size)
{

	if(current_cmd == NULL){
		return;
	}

	while(!scsi_is_ready_for_data_send()){
		continue;
	}

#if 0
debug_log("SENDING %d, %d\n", size, current_cmd->rw_count);
#endif
	ready_for_data_send = 0;
	current_size_to_send = size;
	usb_bbb_send(data, size, 2);
}

static void scsi_data_sent(void)
{
	if(current_cmd == NULL){
		return;
	}
	current_cmd->rw_count -= current_size_to_send;
	current_cmd->rw_addr += current_size_to_send;
	current_size_to_send = 0;
#if 0
debug_log("==> DATA SENT, rw_count = %d\n", current_cmd->rw_count);
#endif
	ready_for_data_send = 1;
	if (!current_cmd->rw_count){
#if 0
debug_log("===> LALA\n");
#endif
		usb_bbb_send_csw(CSW_STATUS_SUCCESS, 0);
		scsi_release_current_cmd();
	}
}

static volatile uint32_t current_size_to_receive = 0;
void scsi_get_data(void *buffer, uint32_t size)
{
	if(current_cmd == NULL){
		return;
	}

	while(!scsi_is_ready_for_data_receive()){
		continue;
	}
	ready_for_data_receive = 0;
	current_size_to_receive = size;

#if 0
debug_log("RECEIVING %d\n", size);
#endif
	assert(buffer);
	usb_bbb_read(buffer, size, 1);
}

static void scsi_write_data(uint32_t size __attribute__((unused)))
{
	if(current_cmd == NULL){
		return;
	}

	//assert(cmd_is_write_operation(current_cmd.cmd));
#if 0
debug_log("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX scsi_write_data %d\n", size);
#endif
	/* [RB] FIXME: Mockup a scsi write command */

	current_cmd->rw_count -= current_size_to_receive;
	current_cmd->rw_addr += current_size_to_receive;
	current_size_to_receive = 0;
	ready_for_data_receive = 1;

	scsi_send_status();
}



void mockup_scsi_write10_data(void){
	if(current_cmd == NULL){
		return;
	}

	//uint32_t addr = current_cmd->rw_addr;
	uint32_t size = current_cmd->rw_count;
	unsigned int i;
	unsigned int sz = (size < buflen) ? size : buflen;
	//unsigned int num = size / sz;
    struct dataplane_command dataplane_command_wr = { 0 };
    struct dataplane_command dataplane_command_ack = { 0 };
    dataplane_command_wr.magic = MAGIC_DATA_WR_DMA_REQ;
    uint64_t tmp = current_cmd->rw_addr / (uint64_t)scsi_block_size;
    if (tmp > 0xffffffff) {
        printf("PANIC! requested sector address generate int overflow !\n");
    }
    dataplane_command_wr.sector_address = (uint32_t)tmp; /* can support up to block size * 2^32 device size */
    dataplane_command_wr.num_sectors = sz / scsi_block_size;
    uint8_t sinker = id_data_sink;
    logsize_t ipcsize = sizeof(struct dataplane_command);

#if SCSI_DEBUG
printf("!!!!!!!!!!!!!!! ==> mockup_scsi_write10_data 0x%x %d\n", current_cmd->rw_addr, size);
//printf("!!!!!!!!!!!!!!! ==> num = %d, sz = %d\n", num, sz);
#endif
	for(i = buflen; i <= size; i+= buflen) {
		scsi_get_data(global_buff, sz);

        while(!scsi_is_ready_for_data_receive()){
            continue;
        }

        // ipc_dma_request to cryp
        sys_ipc(IPC_SEND_SYNC, id_data_sink, sizeof(struct dataplane_command), (const char*)&dataplane_command_wr);
        //do {
           sinker = id_data_sink;
           ipcsize = sizeof(struct dataplane_command);
           sys_ipc(IPC_RECV_SYNC, &sinker, &ipcsize, (char*)&dataplane_command_ack);
        //} while ((sinker != id_data_sink) || (ipcsize != sizeof(struct dataplane_command)));
        if (dataplane_command_ack.magic != MAGIC_DATA_WR_DMA_ACK) {
          printf("dma request to sinker didn't received acknowledge\n");
        }

        dataplane_command_wr.sector_address += sz / scsi_block_size;

	}
    /* Fractional residue */
    if ((i - buflen) < size) {

#if SCSI_DEBUG
        printf("==> Fractional residue = %d\n", size - i + buflen);
#endif
        // TODO: assert that size - (num * sz) *must* be a sector multiple
        scsi_get_data(global_buff, size - i + buflen);

        while(!scsi_is_ready_for_data_receive()){
            continue;
        }

        dataplane_command_wr.num_sectors = (size - i + buflen) / scsi_block_size;
        // ipc_dma_request to cryp (residual content)
        sys_ipc(IPC_SEND_SYNC, id_data_sink, sizeof(struct dataplane_command), (const char*)&dataplane_command_wr);
        //do {
        sinker = id_data_sink;
        ipcsize = sizeof(struct dataplane_command);
        sys_ipc(IPC_RECV_SYNC, &sinker, &ipcsize, (char*)&dataplane_command_ack);
        //} while ((sinker != id_data_sink) || (ipcsize != sizeof(struct dataplane_command)));
        if (dataplane_command_ack.magic != MAGIC_DATA_WR_DMA_ACK) {
            printf("dma request to sinker didn't received acknowledge\n");
        }

    }
#if SCSI_DEBUG
    printf("mockup_scsi_write10_data ended\n");
#endif
}

void mockup_scsi_read10_data(void){
	if(current_cmd == NULL){
		return;
	}
	uint32_t size = current_cmd->rw_count;
	unsigned int i;
	unsigned int sz = (size < buflen) ? size : buflen;
    struct dataplane_command dataplane_command_rd = { 0 };
    struct dataplane_command dataplane_command_ack = { 0 };
    dataplane_command_rd.magic = MAGIC_DATA_RD_DMA_REQ;

    uint64_t tmp = current_cmd->rw_addr / (uint64_t)scsi_block_size;
    if (tmp > 0xffffffff) {
        printf("PANIC! requested sector address generate int overflow !\n");
    }
    dataplane_command_rd.sector_address = (uint32_t)tmp;

    dataplane_command_rd.num_sectors = sz / scsi_block_size;
    uint8_t sinker = id_data_sink;
    logsize_t ipcsize = sizeof(struct dataplane_command);


#if SCSI_DEBUG
printf("==> mockup_scsi_read10_data 0x%x %d\n", dataplane_command_rd.sector_address, size);
#endif
	for(i = buflen; i <= size; i+= buflen) {

        // asking for READ request
        sys_ipc(IPC_SEND_SYNC, id_data_sink, sizeof(struct dataplane_command), (const char*)&dataplane_command_rd);
        //do {
           sinker = id_data_sink;
           ipcsize = sizeof(struct dataplane_command);
           sys_ipc(IPC_RECV_SYNC, &sinker, &ipcsize, (char*)&dataplane_command_ack);
        //} while ((sinker != id_data_sink) || (ipcsize != sizeof(struct dataplane_command)));
        if (dataplane_command_ack.magic != MAGIC_DATA_RD_DMA_ACK) {
          printf("dma request to sinker didn't received acknowledge\n");
        }

        dataplane_command_rd.sector_address += sz / scsi_block_size;
		scsi_send_data(global_buff, sz);
	}
    /* Fractional residue */
    if ((i - buflen) < size) {
#if SCSI_DEBUG
        printf("==> Fractional residue = %d\n", size - i + buflen);
#endif
        dataplane_command_rd.num_sectors = (size - i + buflen) / scsi_block_size;
        // ipc_dma_request to cryp (residual content)
        sys_ipc(IPC_SEND_SYNC, id_data_sink, sizeof(struct dataplane_command), (const char*)&dataplane_command_rd);
        //do {
        sinker = id_data_sink;
        ipcsize = sizeof(struct dataplane_command);
        sys_ipc(IPC_RECV_SYNC, &sinker, &ipcsize, (char*)&dataplane_command_ack);
        //} while ((sinker != id_data_sink) || (ipcsize != sizeof(struct dataplane_command)));
        if (dataplane_command_ack.magic != MAGIC_DATA_RD_DMA_ACK) {
            printf("dma request to sinker didn't received acknowledge\n");
        }

        scsi_send_data(global_buff, size - i + buflen);
    }
#if SCSI_DEBUG
    printf("mockup_scsi_read10_data ended\n");
#endif

}

void scsi_send_status(void)
{
	/* FIXME: status only when data written to sd not as soon as it is send to manager.
	*/
	if (!current_cmd->rw_count){
		usb_bbb_send_csw(CSW_STATUS_SUCCESS, 0);
		scsi_release_current_cmd();
	}
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

static void scsi_cmd_inquiry(void)
{
	struct inquiry_data data;
	/* Most of support bits are set to 0
	 * version is 0 because the device does not claim conformance to any
	 * standard
	 */
	memset((void *)&data, 0, sizeof(data));
	data.rmb = 1;
	data.data_format = 2; /* < 2 obsoletes, > 2 reserved */
	data.additional_len = sizeof(data) - 5;/* (36 - 5) bytes after this one remain */
	strncpy(data.vendor_info, "ANSSI", sizeof(data.vendor_info));
	strncpy(data.product_identification, "WooKey", sizeof(data.product_identification));
	strncpy(data.product_revision, "0.1", sizeof(data.product_revision));
	usb_bbb_send((uint8_t *)&data, sizeof(data), 2);
}


static uint32_t scsi_get_sd_capacity(void){
    uint8_t sinker = id_data_sink;
    logsize_t size = sizeof(struct sync_command_data);
    e_syscall_ret ret;
    uint32_t block_num = 0;
    uint32_t block_size = 0;

    struct sync_command_data ipc_sync_cmd_data = { 0 };
    ipc_sync_cmd_data.magic = MAGIC_STORAGE_SCSI_BLOCK_NUM_CMD;
    sys_ipc(IPC_SEND_SYNC, id_data_sink, sizeof(struct sync_command), (char*)&ipc_sync_cmd_data);

    ret = sys_ipc(IPC_RECV_SYNC, &sinker, &size, (char*)&ipc_sync_cmd_data);
    if (ipc_sync_cmd_data.magic == MAGIC_STORAGE_SCSI_BLOCK_NUM_RESP) {
        block_size = ipc_sync_cmd_data.data.u32[0];
        block_num = ipc_sync_cmd_data.data.u32[1];
    } else {
        printf("[USB SCSI] Error: got no block size from lower layers ...\n");
	while(1){
		sys_yield();
	}
    }
    return block_num;
}

static uint32_t scsi_get_sd_block_size(void){
    uint8_t sinker = id_data_sink;
    logsize_t size = sizeof(struct sync_command_data);
    e_syscall_ret ret;

    struct sync_command_data ipc_sync_cmd_data = { 0 };
    ipc_sync_cmd_data.magic = MAGIC_STORAGE_SCSI_BLOCK_SIZE_CMD;
    sys_ipc(IPC_SEND_SYNC, id_data_sink, sizeof(struct sync_command), (char*)&ipc_sync_cmd_data);

    ret = sys_ipc(IPC_RECV_SYNC, &sinker, &size, (char*)&ipc_sync_cmd_data);
    if (ipc_sync_cmd_data.magic == MAGIC_STORAGE_SCSI_BLOCK_SIZE_RESP) {
        scsi_block_size = ipc_sync_cmd_data.data.u32[0];
        //printf("received block size is %d\n", ipc_sync_cmd_data.data.u32[0]);
        return scsi_block_size;
    }
    else{
        printf("[USB SCSI] Error: got no block size from lower layers ...\n");
	while(1){
		sys_yield();
	}
    }
}

static void scsi_cmd_read_capacity(uint8_t read)
{
	assert(read == 10 || read == 16);
	uint32_t response[2];

	uint32_t sd_card_size = scsi_get_sd_capacity();
	uint32_t sd_card_block_size = scsi_get_sd_block_size();
	assert(sd_card_block_size && sd_card_block_size);

	if (read == 10) {
		response[0] = to_big32(sd_card_size-1);
		response[1] = to_big32(sd_card_block_size);
                //what is expected is the _LAST_ LBA address ....
                // See Working draft SCSI block cmd  5.10.2 READ CAPACITY (10)
                //
		usb_bbb_send((uint8_t *)response, sizeof(response), 2);
	} else if (read == 16) {
		/* TODO */
		while(1){};
	}
}

struct __packed request_sense_data {
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
};

static void scsi_cmd_request_sense(void)
{

	struct request_sense_data data;
	memset((void *)&data, 0, sizeof(data));

	data.error_code = 0x70;
	data.sense_key = SCSI_ERROR_GET_SENSE_KEY(last_error);
	//data.additional_sense_length = 0x0a;
	data.asc = SCSI_ERROR_GET_ASC(last_error);
	data.ascq = SCSI_ERROR_GET_ASCQ(last_error);

	usb_bbb_send((uint8_t *)&data, sizeof(data), 2);
}

static void scsi_cmd_mode_sense(void){
	last_error = SCSI_ERROR_INVALID_COMMAND;
	usb_bbb_send_csw(CSW_STATUS_FAILED, current_cmd->rw_count);
	scsi_release_current_cmd();
}

static void scsi_cmd_prevent_allow_medium_removal(void){
	last_error = SCSI_ERROR_INVALID_COMMAND;
	usb_bbb_send_csw(CSW_STATUS_FAILED, current_cmd->rw_count);
	scsi_release_current_cmd();
}

static void scsi_cmd_test_unit_ready(){
#if 0
	if (sd_is_ready()) {
		usb_bbb_send_csw(CSW_STATUS_SUCCESS, 0);
	} else {
		usb_bbb_send_csw(CSW_STATUS_FAILED, 0);
		last_error = SCSI_ERROR_UNIT_BECOMING_READY;
	}
#else
	usb_bbb_send_csw(CSW_STATUS_SUCCESS, 0);
#endif
	scsi_release_current_cmd();

}



static volatile unsigned int scsi_cmd_queue_empty = 1;
/* NB: this function is executed in a handler context when a
 * command comes from USB.
 */
static void scsi_parse_cmd(uint8_t cmd[], uint8_t cmd_len)
{
	uint32_t rw_lba;
	uint16_t rw_size;
	scsi_cmd *scsi_c;
	int ret;
    // FIXME malloc return to check
    ret = wmalloc((void**)&scsi_c, sizeof(scsi_cmd), ALLOC_NORMAL);
    if(ret){
        while(1){};
    }

	scsi_c->cmd = cmd[0];
	scsi_c->rw_addr = scsi_c->rw_count = 0;

	switch (cmd[0]) {
	case SCSI_CMD_INQUIRY:
#if 0
		printf("[SCSI] inquiry\n");
#endif
		break;
	case SCSI_CMD_MODE_SENSE_6:
		/* TODO */
#if 0
		printf("[SCSI] Mode sense 6 not implemented\n");
#endif
		last_error = SCSI_ERROR_INVALID_COMMAND;
		break;
	case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
		/* TODO */
#if 0
		printf("[SCSI] prevent/allow medium removal not implemented\n");
#endif
		last_error = SCSI_ERROR_INVALID_COMMAND;
		break;
	case SCSI_CMD_READ_CAPACITY_10:
		assert(cmd_len == 10);
#if 0
		printf("[SCSI] read capacity 10\n");
#endif
		break;
	case SCSI_CMD_REQUEST_SENSE:
#if 0
		printf("[SCSI] request sense\n");
#endif
		break;
	case SCSI_CMD_TEST_UNIT_READY:
#if 0
		printf("[SCSI] test unit ready\n");
#endif
		break;
	case SCSI_CMD_READ_10:
		assert(cmd_len == 10);
#if 0
		printf("[SCSI] read 10\n");
#endif
		rw_lba = from_big32(*(uint32_t *)(&cmd[2]));
		rw_size = from_big16(*(uint16_t *)(&cmd[7]));
		scsi_c->rw_addr  = (uint64_t)scsi_block_size * (uint64_t)rw_lba;
		scsi_c->rw_count = scsi_block_size * rw_size;
#if 0
		debug_log("[SCSI] Reading %x bytes (%x * %d) at %x (%x * %d)\n",
		    scsi_c->rw_count, rw_size, scsi_block_size,
		    scsi_c->rw_addr, rw_lba, scsi_block_size);
#endif
		break;
	case SCSI_CMD_WRITE_10:
		assert(cmd_len == 10);
#if 0
		printf("[SCSI] write 10\n");
#endif
		rw_lba = from_big32(*(uint32_t *)(&cmd[2]));
		rw_size = from_big16(*(uint16_t *)(&cmd[7]));
		scsi_c->rw_addr  = (uint64_t)scsi_block_size * (uint64_t)rw_lba;
		scsi_c->rw_count = scsi_block_size * rw_size;
#if 0
		debug_log("[SCSI] Writing %x bytes (%x * %d) at %x (%x * %d)\n",
		    scsi_c->rw_count, rw_size, scsi_block_size,
		    scsi_c->rw_addr, rw_lba, scsi_block_size);
#endif
		break;
	default:
#if 0
		printf("[SCSI] Unsupported command %x\n", cmd[0]);
#endif
		last_error = SCSI_ERROR_INVALID_COMMAND;
		ret = wfree((void**)&scsi_c);
		if(ret){
			while(1){};
		}
		return;
	};

	queue_enqueue(scsi_cmd_queue, scsi_c);
	scsi_cmd_queue_empty = 0;
}

/* TODO: get an additionnal parameter (direction) to add asserts on it */
static void scsi_execute_cmd(void)
{
#if 1
	e_syscall_ret ret = 0;
#endif
	if(scsi_cmd_queue_empty == 1){
		return;
	}

#if 1
	ret = sys_lock (LOCK_ENTER); /* Enter in critical section */
	if(ret != SYS_E_DONE){
		printf("Error: failed entering critical section!\n");
	}
#endif

	current_cmd = queue_dequeue(scsi_cmd_queue);
	if(queue_is_empty(scsi_cmd_queue)){
		scsi_cmd_queue_empty = 1;
	}
#if 1
	ret = sys_lock (LOCK_EXIT);  /* Exit from critical section */
	if(ret != SYS_E_DONE){
		printf("Error: failed exiting critical section!\n");
	}
#endif
	switch (current_cmd->cmd) {
	case SCSI_CMD_INQUIRY:
#if SCSI_DEBUG
		printf("[SCSI] SCSI_CMD_INQUIRY\n");
#endif
		scsi_cmd_inquiry();
		break;

	case SCSI_CMD_MODE_SENSE_6:
#if SCSI_DEBUG
		printf("[SCSI] SCSI_CMD_MODE_SENSE_6\n");
#endif
		scsi_cmd_mode_sense();
		break;
	case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
#if SCSI_DEBUG
		printf("[SCSI] SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL\n");
#endif
		scsi_cmd_prevent_allow_medium_removal();
		break;
	case SCSI_CMD_READ_CAPACITY_10:
#if SCSI_DEBUG
		printf("[SCSI] SCSI_CMD_READ_CAPACITY_10\n");
#endif
		scsi_cmd_read_capacity(10);
		break;
	case SCSI_CMD_REQUEST_SENSE:
#if SCSI_DEBUG
		printf("[SCSI] SCSI_CMD_REQUEST_SENSE\n");
#endif
		scsi_cmd_request_sense();
		break;
	case SCSI_CMD_TEST_UNIT_READY:
#if SCSI_DEBUG
		printf("[SCSI] SCSI_CMD_TEST_UNIT_READY\n");
#endif
		scsi_cmd_test_unit_ready();
		break;
	case SCSI_CMD_READ_10:
#if SCSI_DEBUG
		printf("[SCSI] SCSI_CMD_READ_10\n");
#endif
		mockup_scsi_read10_data();
		break;
	case SCSI_CMD_WRITE_10:
#if SCSI_DEBUG
		printf("[SCSI] SCSI_CMD_WRITE_10\n");
#endif
		mockup_scsi_write10_data();
		break;
	default:
#if SCSI_DEBUG
		printf("[SCSI] Unsupported command %x\n", current_cmd->cmd);
#endif
		last_error = SCSI_ERROR_INVALID_COMMAND;
		usb_bbb_send_csw(CSW_STATUS_FAILED, current_cmd->rw_count);
		scsi_release_current_cmd();
	};
}


void scsi_early_init(uint8_t *buf, uint16_t len)
{
    global_buff = buf;
    buflen = len;
	usb_bbb_early_init(scsi_parse_cmd, scsi_write_data, scsi_data_sent);
}

/*
 * Init
 */
void scsi_init(void)
{
    /* in USB High speed mode, the USB device is mapped (and enabled) just now */
    usb_driver_map();

	unsigned int i;
#if 0
    debug_init_ring_buffer();
	debug_log("[SCSI] Initialization\n");
    debug_flush();
#endif
	scsi_cmd_queue = queue_create(MAX_SCSI_CMD_QUEUE_SIZE);

	for(i = 0; i < buflen; i++){
		global_buff[i] = i;
	}
	/* Register our callbacks on the lower layer */
	usb_bbb_init();
}


/*
 * Main USB SCSI loop
 */
void scsi_state_machine(uint8_t sink, uint8_t source)
{
    id_data_sink = sink;
    id_data_source = source;
	while(1){
		/* Our loop simply waits for commands to execute them */
		scsi_execute_cmd();
	}
}
