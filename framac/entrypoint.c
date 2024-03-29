#include "autoconf.h"
#include "libc/types.h"
#include "libc/string.h"
#include "generated/devlist.h"
#include "libusbctrl.h"
#include "api/libusbmsc.h"
#include "usbmsc_framac_private.h"
#include "usb_bbb.h"
#include "scsi_log.h"
#include "usbmass_desc.h"
#include "usb_control_mass_storage.h"
#include "framac/entrypoint.h"

/* massstorage specific exports for framac */
#include "scsi_automaton.h"


#define USB_BUF_SIZE 16384

/* NOTE: alignment for DMA */
__attribute__ ((aligned(4)))
         uint8_t usb_buf[USB_BUF_SIZE] = { 0 };

/*
 * Support for Frama-C testing
 */

/*@
  @ assigns \nothing;
  */
void Frama_C_update_entropy_b(void) {
  Frama_C_entropy_source_b = Frama_C_entropy_source_b;
}


/*@
  @ assigns \nothing;
  */
void Frama_C_update_entropy_8(void) {
  Frama_C_entropy_source_8 = Frama_C_entropy_source_8;
}

/*@
  @ assigns \nothing;
  */
void Frama_C_update_entropy_16(void) {
  Frama_C_entropy_source_16 = Frama_C_entropy_source_16;
}

/*@
  @ assigns \nothing;
  */
void Frama_C_update_entropy_32(void) {
  Frama_C_entropy_source_32 = Frama_C_entropy_source_32;
}


/*@
  @ assigns \nothing;
  */
bool Frama_C_interval_b(bool min, bool max)
{
  bool r,aux;
  Frama_C_update_entropy_b();
  aux = Frama_C_entropy_source_b;
  if ((aux>=min) && (aux <=max))
    r = aux;
  else
    r = min;
  return r;
}



/*@
  @ assigns \nothing;
  */
uint8_t Frama_C_interval_8(uint8_t min, uint8_t max)
{
  uint8_t r,aux;
  Frama_C_update_entropy_8();
  aux = Frama_C_entropy_source_8;
  if ((aux>=min) && (aux <=max))
    r = aux;
  else
    r = min;
  return r;
}


/*@
  @ assigns \nothing;
  */
uint16_t Frama_C_interval_16(uint16_t min, uint16_t max)
{
  uint16_t r,aux;
  Frama_C_update_entropy_16();
  aux = Frama_C_entropy_source_16;
  if ((aux>=min) && (aux <=max))
    r = aux;
  else
    r = min;
  return r;
}

/*@
  @ assigns \nothing;
  */
uint32_t Frama_C_interval_32(uint32_t min, uint32_t max)
{
  uint32_t r,aux;
  Frama_C_update_entropy_32();
  aux = Frama_C_entropy_source_32;
  if ((aux>=min) && (aux <=max))
    r = aux;
  else
    r = min;
  return r;
}

/*

 test_fcn_usbctrl : test des fonctons définies dans usbctrl.c avec leurs paramètres
 					correctement définis (pas de débordement de tableaux, pointeurs valides...)

*/

/*********************************************************************
 * Callbacks implementations that are required by libmassstorage API
 */

mbed_error_t variable_errcode;

/*@
  @ assigns \nothing;
  */
void usbctrl_configuration_set(void)
{
}


void usbmsc_reset_stack(void)
{
    usbmsc_reinit();
}

/* TODO: The 2 following functions may fails in case of storage error (read error/write error).
 * This should be tested (i.e. returning non-zero value in case of error) to check
 * resiliency on bad storage cases */


/*@
  @ assigns \nothing;
  */
mbed_error_t usbmsc_storage_backend_read(uint32_t sector_addr __attribute__((unused)),
                                       uint32_t num_sectors __attribute__((unused)))
{
    mbed_error_t errcode = variable_errcode;
    return errcode;
}

/*@
  @ assigns \nothing;
  */
mbed_error_t usbmsc_storage_backend_write(uint32_t sector_addr __attribute__((unused)),
                                        uint32_t num_sectors __attribute__((unused)))
{
    mbed_error_t errcode = variable_errcode;
    return errcode;
}

/*@
  @ requires \valid(numblocks);
  @ requires \valid(blocksize);
  @ requires \separated(numblocks, blocksize);
  @ assigns *numblocks, *blocksize;
*/
mbed_error_t usbmsc_storage_backend_capacity(uint32_t *numblocks, uint32_t *blocksize)
{
    mbed_error_t errcode = variable_errcode;
    /* 4GB backend storage size */
    *numblocks = 1024*1024;
    *blocksize = 4096;
    return errcode;
}

/*********************************************************************
 * Effective tests functions
 */

uint32_t usbxdci_handler = 0;

mbed_error_t prepare_ctrl_ctx(void)
{
    mbed_error_t errcode;
    errcode = usbctrl_declare(USB_OTG_HS_ID, &usbxdci_handler);

    // declare buffer, declare device.
    /* should fail */
    errcode = usbmsc_declare(NULL, USB_BUF_SIZE);
    /* @ \assert errcode != MBED_ERROR_NONE ; */
    errcode = usbmsc_declare(usb_buf, USB_BUF_SIZE);
    /* @ \assert errcode == MBED_ERROR_NONE ; */
    // register interface toward libusbctrl
    errcode = usbmsc_initialize(42);
    /* @ \assert errcode != MBED_ERROR_NONE ; */
    errcode = usbmsc_initialize(usbxdci_handler);
    /* @ \assert errcode == MBED_ERROR_NONE ; */
    usbctrl_start_device(usbxdci_handler);

    usbmsc_initialize_automaton();

err:
    return errcode;
}



void test_fcn_massstorage(){

    // Here usbmsc_exec_automaton should be a endless loop, waiting for
    // content pushed by ISR.
    // This content is the result of received data (SCSI messages)
    // read and push into the incomming queue.
    // the SCSI automaton execute the corresponding SCSI cmd, while the
    // ISR only parse and push the received SCSI cmd.
    usbmsc_exec_automaton();

}

void test_fcn_massstorage_errorcases(){
    scsi_error(SCSI_SENSE_ILLEGAL_REQUEST, ASC_NO_ADDITIONAL_SENSE,
               ASCQ_NO_ADDITIONAL_SENSE);

    /* testing invalid transitions */
    scsi_is_valid_transition(SCSI_ERROR, SCSI_CMD_INQUIRY);
    scsi_is_valid_transition(SCSI_IDLE,  0xff);
}

/*requests, triggers... This is specially required for the SCSI stack as all
 * treatments are the consequences of data pushed by the host (this SCSI stack is
 * the *slave* stack implementation).
 * Most of the coverage should be handled here.
 */

static inline void launch_data_recv_and_exec(struct scsi_cbw *cbw)
{
    // garbage on cdb
    for (uint8_t i = 1; i < 16; ++i) {
        cbw->cdb[i] = Frama_C_interval_8(0,255);
    }
    // triggering reception
    usb_bbb_data_received(7, sizeof(struct scsi_cbw), 2);
    scsi_context_t *ctx = scsi_get_context();
    ctx->queue_empty = false;
    /* @ assert scsi_ctx.queue_empty == \false ; */
    // parsing content
    usbmsc_exec_automaton();

    // all cmd exec send back something, assuming this "something" is correctly sent now.
    // This is required to set the SCSI line state to the correct state for next cmd.
    usb_bbb_data_sent(7, 0, 1);

    return;
}


typedef struct {
    uint8_t cdb_len;
    uint8_t cmd;
} cmd_data_t;

static const cmd_data_t cmd_sequence[] = {
  { sizeof(cdb6_inquiry_t),                 SCSI_CMD_INQUIRY },
  { sizeof(cdb16_read_capacity_16_t),       SCSI_CMD_READ_CAPACITY_16 },
  { sizeof(cdb16_read_capacity_16_t),       SCSI_CMD_READ_CAPACITY_16 },
  { sizeof(cdb10_t),                        SCSI_CMD_READ_CAPACITY_10 },
  { sizeof(cdb6_mode_select_t),             SCSI_CMD_MODE_SELECT_6 },
  { sizeof(cdb10_mode_select_t),            SCSI_CMD_MODE_SELECT_10 },
  { sizeof(cdb6_mode_select_t),             SCSI_CMD_MODE_SELECT_6 },
  { sizeof(cdb10_mode_select_t),            SCSI_CMD_MODE_SELECT_10 },
  { sizeof(cdb6_mode_sense_t),              SCSI_CMD_MODE_SENSE_6 },
  { sizeof(cdb10_mode_sense_t),             SCSI_CMD_MODE_SENSE_10 },
  { sizeof(cdb6_mode_sense_t),              SCSI_CMD_MODE_SENSE_6 },
  { sizeof(cdb10_mode_sense_t),             SCSI_CMD_MODE_SENSE_10 },
  { sizeof(cdb12_read_format_capacities_t), SCSI_CMD_READ_FORMAT_CAPACITIES },
  { sizeof(cdb12_read_format_capacities_t), SCSI_CMD_READ_FORMAT_CAPACITIES },
  { sizeof(cdb12_report_luns_t),            SCSI_CMD_REPORT_LUNS },
  { sizeof(cdb10_request_sense_t),          SCSI_CMD_REQUEST_SENSE },
  { sizeof(cdb10_prevent_allow_removal_t),  SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL },
  { sizeof(cdb10_t),                        SCSI_CMD_READ_10 },
  { sizeof(cdb6_t),                         SCSI_CMD_READ_6 },
  { sizeof(cdb6_t),                         SCSI_CMD_TEST_UNIT_READY },
  { sizeof(cdb10_t),                        SCSI_CMD_WRITE_10 },
  { sizeof(cdb6_t),                         SCSI_CMD_WRITE_6 },
  { sizeof(cdb6_t),                         0x42  },
  /* invalid couples (cdb_len is 0)*/
  { 0,                                      SCSI_CMD_READ_10 },
  { 0,                                      SCSI_CMD_INQUIRY },
  { 0,                                      SCSI_CMD_READ_CAPACITY_16 },
  { 0,                                      SCSI_CMD_READ_CAPACITY_10 },
  { 0,                                      SCSI_CMD_MODE_SELECT_6 },
  { 0,                                      SCSI_CMD_MODE_SELECT_10 },
  { 0,                                      SCSI_CMD_MODE_SENSE_6 },
  { 0,                                      SCSI_CMD_MODE_SENSE_10 },
  { 0,                                      SCSI_CMD_READ_FORMAT_CAPACITIES },
  { 0,                                      SCSI_CMD_REPORT_LUNS },
  { 0,                                      SCSI_CMD_REQUEST_SENSE },
  { 0,                                      SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL },
  { 0,                                      SCSI_CMD_READ_10 },
  { 0,                                      SCSI_CMD_READ_6 },
  { 0,                                      SCSI_CMD_TEST_UNIT_READY },
  { 0,                                      SCSI_CMD_WRITE_10 },
  { 0,                                      SCSI_CMD_WRITE_6 },

  /* EOT */
};

/*@
  @ requires \separated(&cbw,&ctx_list+(..),&GHOST_opaque_drv_privates,&scsi_ctx,&bbb_ctx);
  */
void test_fcn_driver_eva(void) {


    mbed_error_t errcode;
    usbctrl_context_t *ctx = NULL;
    ctx = &(ctx_list[0]);

    /* assert \valid(ctx); */

    uint16_t maxlen = Frama_C_interval_16(0,65535);
    /*@ assert ctx != (usbctrl_context_t*)NULL ; */
    uint8_t curr_cfg = 0; /* first cfg declared */
    uint8_t iface = 0; /* first iface declared */

    /* @ assert ctx->cfg[curr_cfg].interfaces[iface].configured == \true; */
    /* @ assert ctx->cfg[curr_cfg].interfaces[iface].usb_ep_number < MAX_EP_PER_INTERFACE; */

    uint8_t max_ep = ctx->cfg[curr_cfg].interfaces[iface].usb_ep_number ;  // cyril : meme chose que pour max_iface, wp passe maintenant
    /* @ assert max_ep < MAX_EP_PER_INTERFACE; */


    /* here, we got back the USB Ctrl context associated to the current USB interface. This
     * allows us to dirrectly manipulate the control plane context to activate/configure the
     * data endpoints and activate triggers.
     * the above assert checks that the usbctrl context we get is the very same one
     * we have declared in the first function checking valid behavior */

    uint8_t i = 0;
    struct scsi_cbw *cbw = usb_bbb_get_cbw();

    /* we have defined a full-duplex HID interface. Let's (manually) configure it.
     * This portion of code is representative of the Set_Configuration STD request */
    usb_backend_drv_configure_endpoint(1, /* EP 1 */
            USB_EP_TYPE_BULK,
            USB_BACKEND_DRV_EP_DIR_OUT,
            512, /* mpsize */
            USB_BACKEND_EP_ODDFRAME,
            usb_bbb_data_received);
    usb_backend_drv_configure_endpoint(2, /* EP 1 */
            USB_EP_TYPE_BULK,
            USB_BACKEND_DRV_EP_DIR_IN,
            512, /* mpsize */
            USB_BACKEND_EP_ODDFRAME,
            usb_bbb_data_sent);
    ctx->cfg[curr_cfg].interfaces[iface].eps[0].configured = true;
    ctx->cfg[curr_cfg].interfaces[iface].eps[1].configured = true;

    usbctrl_configuration_set();

    // 1) step1: GetMaxLun command (class request)
    usbctrl_setup_pkt_t pkt = { 0 };
    pkt.wIndex = Frama_C_interval_16(0,65535);
    pkt.bRequest = USB_RQST_GET_MAX_LUN;
    pkt.wLength = Frama_C_interval_16(0,65535);
    pkt.wValue = Frama_C_interval_16(0,65535);
    pkt.bmRequestType = Frama_C_interval_8(0,255);

    mass_storage_class_rqst_handler(usbxdci_handler, &pkt);


    // 2) step2: forging BBB requests as if the host was sending them, and
    // call usb_bbb_data_received();
    // To do that, the cbw structure must be set accordingly with any garbage
    // (or real cbw content) for each test.
    // Fixing cbw header
    cbw->sig = USB_BBB_CBW_SIG;
    cbw->flags.reserved = 0;
    cbw->lun.reserved = 0;
    cbw->lun.lun = 0;
    cbw->cdb_len.reserved = 0;

    /* because there is garbage in cdb content, these various calls may generate errors
     * (invalid size, invalid sector number, etc.), which may handle invalid state for the next
     * command, making this command refused (or dropped) at bbb or scsi level.
     * This is a **normal** behavior of the stack, but this impacts the capacity to check
     * the overall code. To avoid this, we loop on the following sequence */
    uint8_t table_size = sizeof(cmd_sequence) / sizeof(cmd_data_t);
    /*@
      @ loop invariant 0 <= i <= 40;
      @ loop variant 40-i;
      */
    for (uint8_t i = 0; i < 40; ++i) {
        cbw->cdb_len.cdb_len = cmd_sequence[i].cdb_len;
        cbw->cdb[0]  = cmd_sequence[i].cmd;
        launch_data_recv_and_exec(cbw);
    }

#if 0
    scsi_set_state(SCSI_ERROR);

    /* INQUIRY is not valid in ERROR state */
    cbw->cdb_len.cdb_len = sizeof(cdb6_inquiry_t);
    cbw->cdb[0]  = SCSI_CMD_INQUIRY;
    launch_data_recv_and_exec(cbw);

    scsi_set_state(SCSI_IDLE);
    /* INQUIRY is not valid in ERROR state */
    cbw->cdb_len.cdb_len = sizeof(cdb6_inquiry_t);
    cbw->cdb[0]  = SCSI_CMD_INQUIRY;
    launch_data_recv_and_exec(cbw);

    scsi_set_state(SCSI_IDLE);
#endif

    /* inexistant next state */
    scsi_next_state(SCSI_ERROR, SCSI_CMD_READ_6);


    /* invalid size */
    usb_bbb_data_received(7, sizeof(struct scsi_cbw)+1, 2);

    /* invalid sig */
    cbw->sig = USB_BBB_CBW_SIG-1;
    usb_bbb_data_received(7, sizeof(struct scsi_cbw), 2);
    cbw->sig = USB_BBB_CBW_SIG;

    /* invalid flags */
    cbw->flags.reserved = 1;
    usb_bbb_data_received(7, sizeof(struct scsi_cbw), 2);
    cbw->flags.reserved = 0;

    /* invalid lun */
    cbw->lun.reserved = 1;
    usb_bbb_data_received(7, sizeof(struct scsi_cbw), 2);
    cbw->lun.reserved = 0;

    /* invalid cdb_len */
    cbw->cdb_len.reserved = 1;
    usb_bbb_data_received(7, sizeof(struct scsi_cbw), 2);
    cbw->cdb_len.reserved = 0;

    /* invalid lun id */
    cbw->lun.lun = CONFIG_USR_LIB_MASSSTORAGE_SCSI_MAX_LUNS;
    usb_bbb_data_received(7, sizeof(struct scsi_cbw), 2);
    cbw->lun.lun = 0;

err:
    return;
}


void test_fcn_driver_eva_reset() {

    struct scsi_cbw *cbw = usb_bbb_get_cbw();
    // 1) step1: GetMaxLun command (class request)
    usbctrl_setup_pkt_t pkt = { 0 };
    pkt.wIndex = Frama_C_interval_16(0,65535);
    pkt.bRequest = USB_RQST_MS_RESET;
    pkt.wLength = Frama_C_interval_16(0,65535);
    pkt.wValue = Frama_C_interval_16(0,65535);
    pkt.bmRequestType = Frama_C_interval_8(0,255);

    mass_storage_class_rqst_handler(usbxdci_handler, &pkt);

    usbmsc_reinit();

    reset_requested = true;

    /* read while reset */
    cbw->sig = USB_BBB_CBW_SIG;
    cbw->flags.reserved = 0;
    cbw->lun.reserved = 0;
    cbw->lun.lun = 0;
    cbw->cdb_len.reserved = 0;
    cbw->cdb_len.cdb_len = sizeof(cdb6_t);
    cbw->cdb[0]  = SCSI_CMD_READ_6;
    launch_data_recv_and_exec(cbw);

    reset_requested = false;
    scsi_context_t *ctx = scsi_get_context();

    ctx->global_buf_len = 2048;
    ctx->size_to_process = 4096;
    scsi_data_sent();

    usbmsc_reinit();

    return;
}

// cbw content storage, set in usb_bbb.c
// Here we directly set some content in it.

int main(void)
{
    variable_errcode = Frama_C_interval_8(0,16);
    mbed_error_t errcode;

    errcode = prepare_ctrl_ctx();
    if (errcode != MBED_ERROR_NONE) {
        goto err;
    }
    test_fcn_massstorage() ;
    test_fcn_massstorage_errorcases() ;
    test_fcn_driver_eva() ;
    test_fcn_driver_eva_reset() ;
err:
    return errcode;
}
