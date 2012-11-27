#include <string.h>
#include <avr/eeprom.h>
#include "board.h"
#include "rf_send.h"
#include "rf_receive.h"
#include "fncollection.h"
#include "display.h"
#include "fht.h"
#include "delay.h"
#include "clock.h"
#include "cc1100.h"

// We have two different work models:

// 1. Control the FHT80b. In this mode we have to wait for a message from the
//    FHT80b, and then send all buffered messages to it, with a strange
//    ask/reply protocol.

// 2. Control the valves directly (8v mode). In this mode we have to send every
//    115+x second a message to the valves. Some messages (e.g. pair) are sent
//    directly. This mode is activated when we receive an actuator message for
//    our own housecode.
//    The 80b is designed to control multiple 8v's in one room. It is possible
//    to have up to 8 different 8v values, but only one value is sent every 2
//    minutes, it takes 16 minutes to set 8 different values.  We optimize
//    culfw to control 8v's in different rooms, for this purpose we need to
//    update them faster (idea and first implementation by Alexander).  For
//    this purpose up to eight different "own" house codes are used: The first
//    valve gets the house code directly.  Subsequent valves get the house code
//    increased by 0x0100. Values for each valve are sent directly one after
//    the other, but only once.



// Our housecode. The first byte for 80b communication, both together for
// direct 8v controlling
uint8_t fht_hc0, fht_hc1;



#ifdef HAS_FHT_80b

       uint8_t fht80b_timeout;
       uint8_t fht80b_state;    // 80b state machine
       uint8_t fht80b_minute;
static uint8_t fht80b_ldata;    // last data waiting for ack
static uint8_t fht80b_out[6];   // Last sent packet. Reserve 1 byte for checksum
static uint8_t fht80b_repeatcnt;
static uint8_t fht80b_buf[FHTBUF_SIZE];
static uint8_t fht80b_bufoff;   // offset in the current fht80b_buf

static void    fht80b_print(uint8_t level);
static void    fht80b_initbuf(void);
static void    fht_delbuf(uint8_t *buf);
static uint8_t fht_addbuf(char *in);
static uint8_t fht_getbuf(uint8_t *buf);
static void    fht80b_reset_state(void);
#if FHTBUF_SIZE > 255
static uint16_t fht_bufspace(void);
#else
static uint8_t fht_bufspace(void);
#endif

#endif 


#ifdef HAS_FHT_8v

uint16_t fht8v_timeout;
uint8_t fht8v_buf[2*FHT_8V_NUM];
uint8_t fht8v_ctsync;
#define FHT8V_CMD_SET  0x26
#define FHT8V_CMD_SYNC 0x2c
#define FHT8V_CMD_PAIR 0x2f

#endif

#undef FHTDEBUG        // Display all FHT traffic on USB

void
fht_display_buf(uint8_t ptr[])
{
#ifdef FHTDEBUG
#warning FHT USB DEBUGGING IS ACTIVE
  uint8_t odc = display_channel;
  display_channel = DISPLAY_USB;
  uint16_t *p = (uint16_t *)&ticks;
  DU(*p, 5);
  DC(' ');
  DH2(fht80b_state);
  DC(' ');
#else
  if(!(tx_report & REP_FHTPROTO))
    return;
#endif

  DC('T');
  for(uint8_t i = 0; i < 5; i++)
    DH2(ptr[i]);
  if(tx_report & REP_RSSI)
    DH2(250);
  DNL();
#ifdef FHTDEBUG
  display_channel = odc;
#endif
}

void
fht_init(void)
{
  fht_hc0 = erb(EE_FHTID);
  fht_hc1 = erb(EE_FHTID+1);
#ifdef HAS_FHT_8v
  for(uint8_t i = 0; i < FHT_8V_NUM; i++)
    fht8v_buf[2*i] = FHT_8V_DISABLED;
#endif

#ifdef HAS_FHT_80b
  fht80b_reset_state();
  fht80b_initbuf();
#endif
}

void
fhtsend(char *in)
{
  uint8_t hb[6], l;                    // Last byte needed for 8v checksum
  l = fromhex(in+1, hb, 5);

  if(l < 4) {

    if(hb[0] == 1) {                   // Set housecode, clear buffers
      if(l == 3) {
        ewb(EE_FHTID  , hb[1]);        // 1.st byte: 80b relevant
        ewb(EE_FHTID+1, hb[2]);        // 1.st+2.nd byte: 8v relevant
        fht_init();
        return;

      } else {
        DH2(fht_hc0);
        DH2(fht_hc1);
      }

#ifdef HAS_FHT_80b
    } else if(hb[0] == 2) {            // Return the 80b buffer
      fht80b_print(l==1 || hb[1]==1);

    } else if(hb[0] == 3) {            // Return the remaining fht buffer
#if FHTBUF_SIZE > 255
      DH(fht_bufspace(),4);
#else
      DH2(fht_bufspace());
#endif
#endif

#ifdef HAS_FHT_8v
    } else if(hb[0] == 0x10) {         // Return the 8v buffer

      uint8_t na=0;
      for(int i = 0; i < FHT_8V_NUM; i++) {
        if(fht8v_buf[2*i] == FHT_8V_DISABLED)
          continue;
        if(na)
          DC(' ');
        DH2(i);
        DC(':');
        DH2(fht8v_buf[2*i]);
        DH2(fht8v_buf[2*i+1]);
        na++;
      }

      if(na==0)
        DS_P( PSTR("N/A") );

    } else if(hb[0] == 0x11) {         // Return the next 8v timeout
      DH2(fht8v_timeout/125);
#endif

    }
    DNL();

  } else {

#ifdef HAS_FHT_8v
    if(hb[0]>=fht_hc0 &&
       hb[0]< fht_hc0+FHT_8V_NUM &&
       hb[1]==fht_hc1) {                 // FHT8v mode commands

      if(hb[3] == FHT8V_CMD_PAIR) {
        addParityAndSend(in, FHT_CSUM_START, 2);

      } else if(hb[3] == FHT8V_CMD_SYNC){// start syncprocess for _all_ 8v's
        fht8v_ctsync = hb[4];            // use it to shorten the sync-time
        fht8v_timeout=1;

        // Cheating on the 1%
        uint8_t cnt = 0;
        for(uint8_t i = 0 ; i < FHT_8V_NUM; i++ )
          if(fht8v_buf[2*i] != FHT_8V_DISABLED )
            cnt++;
        credit_10ms += (4*fht8v_ctsync);   // should be 3.75 = 75ms / 2 / 10
        
      } else {                           // Valve position
        uint8_t idx = (hb[0]-fht_hc0)*2;
        fht8v_buf[idx  ] = hb[3];        // Command or 0xff for disable this
        fht8v_buf[idx+1] = hb[4];        // slot (valve)

      }
      return;
    }
#endif

#ifdef HAS_FHT_80b
    if(!fht_addbuf(in))                  // FHT80b mode: Queue everything
      DS_P( PSTR("EOB\r\n") );
#endif

  }
}

//////////////////////////////////////////////////////////////
#ifdef HAS_FHT_8v
void
fht8v_timer(void)
{
  uint8_t hb[6], i;

  hb[1] = fht_hc1;
  hb[2] = 0;                             // Valve idx

  if(fht8v_ctsync) {                     // Count down in 1sec steps
    fht8v_timeout = (fht8v_ctsync == 3) ? 3*125 : 125;

  } else {
    fht8v_timeout = (125*(230+(fht_hc1&0x7)))>>1;

  }

  for(i = 0 ; i < FHT_8V_NUM; i++ ) {
    if(fht8v_buf[2*i] == FHT_8V_DISABLED )
      continue;
    hb[0] = fht_hc0+i;
    if(fht8v_ctsync) {
      hb[3] = FHT8V_CMD_SYNC;
      hb[4] = fht8v_ctsync;

    } else {
      hb[3] = fht8v_buf[2*i  ];
      hb[4] = fht8v_buf[2*i+1];

    }
    addParityAndSendData(hb, 5, FHT_CSUM_START, 1);
    fht_display_buf(hb);

  }

  if(fht8v_ctsync) {
    if(fht8v_ctsync == 3) {
      fht8v_ctsync = 0;

    } else {
      fht8v_ctsync -= 2;

    }

  }

}
#endif


//////////////////////////////////////////////////////////////
#ifdef HAS_FHT_80b
// one FHT needs 2.1 sec RF send time per hour (75ms*7*4)

const PROGMEM prog_uint8_t fht80b_state_tbl[] = {
  //FHT80b          //CUL, 0 for no answer
  FHT_ACTUATOR,     FHT_CAN_XMIT,       // We are deaf for the second ACTUATOR
  FHT_CAN_XMIT,     0,
  FHT_CAN_RCV,      FHT_START_XMIT,
  FHT_START_XMIT,   FHT_DATA,
  FHT_DATA,         FHT_ACK,
  FHT_ACK,          FHT_END_XMIT,
  FHT_END_XMIT,     0
};
#define RCV_OFFSET 6

static void
fht80b_sendpacket(void)
{
  ccStrobe(CC1100_SIDLE);               // Don't let the CC1101 to disturb us

  // avg. FHT packet is 75ms.
  // The first delay is larger, as we don't know if we received the first or
  // second FHT actuator message.
  my_delay_ms(fht80b_out[2]==FHT_CAN_XMIT ? 155 : 75);
  addParityAndSendData(fht80b_out, 5, FHT_CSUM_START, 1);
  ccRX();                               // reception might be lost due to LOVF

  fht_display_buf(fht80b_out);
}

static void
fht80b_reset_state(void)
{
  fht80b_state = 0;
  fht80b_ldata = 0;
  fht80b_bufoff = 3;
  fht80b_timeout = FHT_TIMER_DISABLED;
}

void
fht80b_timer(void)
{
  if(fht80b_repeatcnt) {
    fht80b_sendpacket();
    fht80b_timeout = 41;               // repeat if there is no msg for 0.3sec
    fht80b_repeatcnt--;
  } else {
    fht80b_reset_state();
  }
}

static void
fht80b_send_repeated(void)
{
  fht80b_sendpacket(); 
  if(fht80b_out[2]==FHT_ACK2)
    return;
  fht80b_repeatcnt = 1;                // Never got reply for the 3.rd data
  fht80b_timeout = 41;                 // 330+75+70 = 475ms
}

void
fht_hook(uint8_t *fht_in)
{
  uint8_t fi0 = fht_in[0];             // Makes code 18 bytes smaller...
  uint8_t fi1 = fht_in[1];
  uint8_t fi2 = fht_in[2];             // FHT Command with extension bit
  uint8_t fi3 = fht_in[3];             // Originator: CUL or FHT
  uint8_t fi4 = fht_in[4];             // Command Argument
#ifdef FHTDEBUG
  fht_display_buf(fht_in); 
#endif

  fht80b_timeout = 41;                 // Activate the timer, delay RFR...
  fht80b_repeatcnt = 0;                // but do not send anything per default
  if(fht_hc0 == 0 && fht_hc1 == 0)     // FHT processing is off
    return;

  if(fht80b_out[0] != fi0 ||           // A different FHT is sending data
     fht80b_out[1] != fi1) {
    fht80b_reset_state();
    fht80b_out[0] = fi0;
    fht80b_out[1] = fi1;
  }

  if(fht80b_state == FHT_FOREIGN)       // This is not our FHT, forget it
    return;

  uint8_t inb, outb;
  fht80b_out[2] = fi2;                  // copy the in buffer as it cannot
  fht80b_out[3] = fi3;                  // be used for sending it out again
  fht80b_out[4] = fi4;


  //////////////////////////////
  // FHT->CUL part: ack everything.
  if(fi2 && fht80b_state == 0) {            

    if(fi4 != fht_hc0 &&                // Foreign (not our) FHT/FHZ, forget it
       fi4 != 100 &&                    // not the unassigned one
       (fi2 == FHT_CAN_XMIT ||          // FHZ start seq
        fi2 == FHT_CAN_RCV  ||          // FHZ start seq?
        fi2 == FHT_START_XMIT)) {       // FHT start seq
      fht80b_state = FHT_FOREIGN;
      return;
    }

    // Setting fht80b_out[4] to our housecode for FHT_CAN_RCV & FHT_START_XMIT
    // won't change the FHT pairing
    if(fi2 == FHT_CAN_RCV)
      return;

    if((fi3 & 0xf0) == 0x60) {          // Ack only 0x6? packets, else we get
      fht80b_out[3] = fi3|0x70;         // strange "endless" loops when other
      fht80b_send_repeated();           // CUL's / FHT's are involved
    }
    return;

  }


  //////////////////////////////
  // CUL->FHT part
  if(!fht_getbuf(fht80b_out) && fht80b_state == 0)
    return;

  // What is the FHT80 supposed to send?
  inb = (fht80b_ldata ? fht80b_ldata :
                                __LPM(fht80b_state_tbl+fht80b_state));


  // We frequently loose the FHT_CAN_XMIT msg from the FHT
  // so skip this state if the next msg comes in.
  if(inb == FHT_CAN_XMIT && fi2 == FHT_CAN_RCV) {
    inb = FHT_CAN_RCV;
    fht80b_state += 2;
  }
    
  // Ack-Check: correct ack from the FHT?
  if(inb != fi2) {
    fht80b_reset_state();
    return;
  }

  outb = 0;                             // What should the CUL send back?

  if(fht80b_ldata) {
    fht80b_bufoff += 2;

    if(fht_getbuf(fht80b_out)) {        // Search for next
      outb = FHT_DATA;

    } else {
      fht80b_state+=2;
    }
  }

  if(!outb)
    outb = __LPM(fht80b_state_tbl+fht80b_state+1);

  if(outb == FHT_DATA) {            

    fht80b_ldata = fht80b_out[2];

    if(fht80b_ldata == FHT_MINUTE)      // Adjust the minute offset...
      fht80b_out[4] = fht80b_minute;

  } else {                              // Follow the protocol table
    fht80b_out[2] = outb;
    fht80b_out[3] = 0x77;
    fht80b_out[4] = fht_hc0;
    fht80b_state += 2;
    fht80b_ldata = 0;
    if(fht80b_state == sizeof(fht80b_state_tbl)) {
      // If we delete the buffer earlier, and our conversation aborts, then the
      // FHT will send every 10 minutes from here on a can_rcv telegram, and
      // will stop sending temperature messages
      fht_delbuf(fht80b_out);
      fht80b_reset_state();
    }

  }

  if(outb)
    fht80b_send_repeated();

}

///////////////////////
// FHT buffer management functions
// "Dumb" FHZ model: store every incoming message in a separate buffer.
// Transmit only one buffer at a time.  Delete a complete buffer if a
// transmission is complete.
static uint8_t*
fht_lookbuf(uint8_t *buf)
{
  uint8_t *p = fht80b_buf;
  uint8_t *bmax = fht80b_buf+FHTBUF_SIZE;

  while(p[0] && p < bmax) {
    if(buf != 0 && p[1] == buf[0] && p[2] == buf[1])
      return p;
    p += p[0];
  }
  if(p > bmax)
    p = bmax;
  return (buf ? 0 : p);
}

static void
fht_delbuf(uint8_t *buf)
{
  uint8_t *p = fht_lookbuf(buf);
  if(p == 0)
    return;

  uint8_t sz = p[0];
  uint8_t *lim = fht80b_buf+FHTBUF_SIZE-sz; 

  while(p < lim) {
    p[0]=p[sz];
    p++;
  }
  p[0] = 0;
}


static uint8_t
fht_addbuf(char *in)
{
  uint8_t *p = fht_lookbuf(0);  // get a free slot
  uint8_t l = strlen(in+1)/2+1; // future size of the message
  uint8_t i, j;

  if((p+l-fht80b_buf) > FHTBUF_SIZE)
    return 0;

  p[0] = l;
  for(i=1, j=1 ; j < l; i+=2, j++) {
    fromhex(in+i, p+j, 1);
    if(j > 2 && ((j&1) == 0)) {
      if(p[j-1] == FHT_MINUTE) 
        fht80b_minute = p[j];
    }
  }
  if(p < (fht80b_buf+FHTBUF_SIZE))
    p[j] = 0;
  return 1;
}

#if FHTBUF_SIZE > 255
static uint16_t
fht_bufspace(void)
{
  return (FHTBUF_SIZE - (uint16_t)(fht_lookbuf(0)-fht80b_buf));
}
#else
static uint8_t
fht_bufspace(void)
{
  return (FHTBUF_SIZE - (uint8_t)(fht_lookbuf(0)-fht80b_buf));
}
#endif

static uint8_t
fht_getbuf(uint8_t *buf)
{
  uint8_t *p = fht_lookbuf(buf);
  if(p == 0 || p[0] <= fht80b_bufoff)
    return 0;
  buf[2] = p[fht80b_bufoff];
  buf[3] = 0x79;
  buf[4] = p[fht80b_bufoff+1];
  return 1;
}

static void
fht80b_initbuf()
{
  fht80b_buf[0] = 0;
}

static void
fht80b_print(uint8_t full)
{
  uint8_t *p = fht80b_buf;

  if(!p[0])
    DS_P( PSTR("N/A") );
  while(p[0] && (p < (fht80b_buf+FHTBUF_SIZE))) {
    if(p != fht80b_buf)
      DC(' ');
    uint8_t i = 1;
    DH2(p[i++]);
    DH2(p[i++]);
    if(full) {
      DC(':');
      while(i < p[0]) {
        if(i > 3 && (i&1))
          DC(',');
        DH2(p[i++]);
      }
    }
    p += p[0];
  }
}

#endif
