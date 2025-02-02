/*
$Id: ts2secpes.c,v 1.15 2006/02/12 23:17:13 rasc Exp $


 DVBSNOOP

 a dvb sniffer  and mpeg2 stream analyzer tool
 http://dvbsnoop.sourceforge.net/

 (c) 2001-2006   Rainer.Scherg@gmx.de



 -- Transport Stream Sub-Decode  PES / SECTION

   


$Log: ts2secpes.c,v $
Revision 1.15  2006/02/12 23:17:13  rasc
TS 101 191 MIP - Mega-Frame Initialization Packet for DVB-T/H  (TS Pid 0x15)

Revision 1.14  2006/01/02 18:24:34  rasc
just update copyright and prepare for a new public tar ball

Revision 1.13  2005/11/08 23:15:27  rasc
 - New: DVB-S2 Descriptor and DVB-S2 changes (tnx to Axel Katzur)
 - Bugfix: PES packet stuffing
 - New:  PS/PES read redesign and some code changes

Revision 1.12  2005/10/25 18:41:41  rasc
minor code rewrite

Revision 1.11  2005/10/23 22:50:28  rasc
 - New:  started ISO 13818-2 StreamIDs
 - New:  decode multiple PS/PES packets within TS packets (-tssubdecode)

Revision 1.10  2005/10/23 20:58:15  rasc
subdecode multiple SI packets with TS packet using -tssubdecode

Revision 1.9  2005/10/20 22:25:31  rasc
 - Bugfix: tssubdecode check for PUSI and SI pointer offset
   still losing packets, when multiple sections in one TS packet.
 - Changed: some Code rewrite
 - Changed: obsolete option -nosync, do always packet sync

Revision 1.8  2005/09/09 14:20:31  rasc
TS continuity sequence check (cc verbose output)

Revision 1.7  2005/09/06 23:13:52  rasc
catch OS signals (kill ...) for smooth program termination

Revision 1.6  2004/04/18 19:30:32  rasc
Transport Stream payload sub-decoding (Section, PES data) improved

Revision 1.5  2004/04/15 23:22:58  rasc
no message

Revision 1.4  2004/04/15 22:29:23  rasc
PMT: some brainded section check
TS: filter single pids from multi-pid ts-input-file
minor enhancements

Revision 1.3  2004/04/15 10:53:22  rasc
minor changes

Revision 1.2  2004/04/15 04:08:49  rasc
no message

Revision 1.1  2004/04/15 03:40:39  rasc
new: TransportStream sub-decoding (ts2PES, ts2SEC)  [-tssubdecode]
checks for continuity errors, etc. and decode in TS enclosed sections/pes packets



*/


#include "dvbsnoop.h"
#include "ts2secpes.h"
#include "ts_misc.h"
#include "sections/sectables.h"
#include "pes/pespacket.h"
#include "misc/packet_mem.h"
#include "misc/output.h"
#include "misc/program_mem.h"
#include "strings/dvb_str.h"



#define TS_SUBDEC_BUFFER   (512*1024)
enum  { TSD_no_error = 0, TSD_output_done,
        TSD_no_pui, TSD_error, TSD_continuity_error,
        TSD_scrambled_error, TSD_mem_error};


typedef struct _TS_SUBDEC {
  int     mem_handle;
  int pid;
  int     status;                 // content is invalid?
  int     continuity_counter;     // 4 bit max !!
  int     packet_counter;
  int     payload_length;         // total length of PES or SECTION to be read, 0 = unspecified
  long pkt_nr;  // start packet number
} TS_SUBDEC;


static TS_SUBDEC tsds[MAX_PID+1];

static TS_SUBDEC* get_TSD(int pid) {
  TS_SUBDEC* tsd = &tsds[pid];
  if (tsd->mem_handle < 0) { // memory is not allocated yet
    if ((tsd->mem_handle = packetMem_acquire (TS_SUBDEC_BUFFER)) < 0) {
      tsd->status = TSD_mem_error;
    }
  }
  return tsd;
}

//------------------------------------------------------------ 

//
// -- init TS sub decoding buffer
// -- return: < 0: fail
//
int ts2SecPesInit (void)
{
  int i;
  for (i = 0; i < sizeof(tsds)/sizeof(TS_SUBDEC); ++i) {
    TS_SUBDEC* tsd = &tsds[i]; 
    tsd->mem_handle = -1;
    tsd->pid = i;
    tsd->status = TSD_no_pui;
    tsd->continuity_counter = -1;
    tsd->packet_counter = 0;
    tsd->payload_length = 0;
  }
  return 0;
}


//
// -- free TS sub decoding buffer
//
void ts2SecPesFree (void)
{
  int i;
  for (i = 0; i < sizeof(tsds)/sizeof(TS_SUBDEC); ++i) {
    TS_SUBDEC* tsd = &tsds[i];
    if (tsd->mem_handle >= 0) {
      packetMem_free (tsd->mem_handle);
    }
  }
}


//
// -- add TS data 
// -- return: 0 = fail
//
int ts2SecPes_AddPacketStart (long pkt_nr, int pid, int cc, u_char *b, u_int len)
{
  int l;
  TS_SUBDEC* tsd = get_TSD(pid);

  // -- duplicate packet ?
  if ((pid == tsd->pid) && (cc == tsd->continuity_counter)) {
    return 1;
  }

  tsd->status = TSD_no_error;
  tsd->pid = pid;
  tsd->continuity_counter = cc;
  tsd->packet_counter = 1;
  tsd->pkt_nr = pkt_nr;

  // -- Save PES/PS or SECTION length information of incoming packet
  // -- set 0 for unspecified length
  l = 0;

  // -- TS can contain multiple packets streamed in payload, so calc will be wrong!!!
  // -- so I skip this at this time...
  // -- $$$ code modification mark (1) start
  //    if (len > 6) {
  //      // Non-System PES (<= 0xBC) will have an unknown length (= 0)
  //      if (b[0]==0x00 && b[1]==0x00 && b[2]==0x01 && b[3]>=0xBC) {
  //              l = (b[4]<<8) + b[5];           // PES packet size...
  //              if (l) l += 6;                  // length with PES-sync, etc.
  //      } else {
  //              int pointer = b[0]+1;
  //              if (pointer+3 <= len) { // not out of this packet?
  //                      l = ((b[pointer+1] & 0x0F) << 8) + b[pointer+2]; // sect size  (get_bits)
  //              }
  //              if (l) l += pointer + 3;        // length with pointer & tableId
  //      }
  //   }
  // -- $$$ code modification mark (1) end
  //

  tsd->payload_length = l;


  packetMem_clear (tsd->mem_handle);
  if (! packetMem_add_data (tsd->mem_handle,b,len)) {
    tsd->status = TSD_mem_error;
    return 0;
  }

  return 1;
}


int ts2SecPes_AddPacketContinue (int pid, int cc, u_char *b, u_int len)
{
  TS_SUBDEC* tsd = get_TSD(pid);

  // -- duplicate packet?  (this would be ok, due to ISO13818-1)
  if ((pid == tsd->pid) && (cc == tsd->continuity_counter)) {
    return 1;
  }

  // -- discontinuity error in packet ?
  if ((tsd->status == TSD_no_error) && (cc != (++tsd->continuity_counter%16))) {
    tsd->status = TSD_continuity_error;
  }

  tsd->continuity_counter = cc;

  if (tsd->status == TSD_no_error) {
    if (!packetMem_add_data (tsd->mem_handle,b,len) ) {
      tsd->status = TSD_mem_error;
    } else {
      tsd->packet_counter++;
      return 1;
    }
  }

  return 0;
}



//------------------------------------------------------------ 


//
// -- TS  SECTION/PES  subdecoding
// -- check TS buffer and push data to sub decoding buffer
// -- on new packet start, output old packet data
//
void ts2SecPes_subdecode (u_char *b, int len, long pkt_nr, u_int opt_pid)
{
  u_int  transport_error_indicator;           
  u_int  payload_unit_start_indicator;                
  u_int  pid;         
  u_int  transport_scrambling_control;                
  u_int  continuity_counter;          
  u_int  adaptation_field_control;

  TS_SUBDEC* tsd = NULL;

  //fprintf(stdout,  "-># ts2SecPes_subdecode: len=%d, opt_pid=%u\n", len, opt_pid);

  pid                             = getBits (b, 0,11,13);

  tsd = get_TSD(pid);

  // -- filter pid?
  if (opt_pid >= 0 && opt_pid <= MAX_PID) {
    if (opt_pid != pid)  return;
  }

  // -- no ts subdecode for special pids...
  if (check_TS_PID_special (pid)) return;


  transport_error_indicator       = getBits (b, 0, 8, 1);
  payload_unit_start_indicator    = getBits (b, 0, 9, 1);
  transport_scrambling_control    = getBits (b, 0,24, 2);
  adaptation_field_control        = getBits (b, 0,26, 2);
  continuity_counter              = getBits (b, 0,28, 4);



  len -= 4;
  b   += 4;


  // -- skip adaptation field
  if (adaptation_field_control & 0x2) {
    int n;

    n = b[0] + 1;
    b += n;
    len -= n;
  }


  // -- push data to subdecoding collector buffer
  // -- on packet start, output collected data of buffer
  if (adaptation_field_control & 0x1) {

    // -- payload buffering/decoding

    // -- oerks, this we cannot use
    if (/*transport_scrambling_control || */transport_error_indicator) {
      tsd->status = TSD_scrambled_error;
      return;
    }

    // -- fillup scrambled data
    if (transport_scrambling_control) {
      int i; for (i = 0; i < len; ++i) b[i] = 0xCA;
    }

    // -- if payload_start, check PES/SECTION
    if (payload_unit_start_indicator) {

      // -- sections: pui-start && pointer != 0 push data to last section!
      // -- (PES would be also 0x00)

      int SI_offset = b[0];   // pointer
      if (SI_offset) {
        ts2SecPes_AddPacketContinue (pid, continuity_counter, b+1, (u_long)SI_offset);
        // -- because re-add data below, we have to fake cc
        tsd->continuity_counter--;
      }

      // $$$ TODO: here we have a flaw, when pointer != 0, we do not display the new 
      //           TS packet, but we are subdecoding (display) using the TS overflow data...
      //           Workaround: pass SI_offset to output to display, that we are using data
      //                       from next TS packet... (this should do for now)

      // -- output data of prev. collected packets
      // -- if not already decoded or length was unspecified
      if ((tsd->status != TSD_output_done) && packetMem_length(tsd->mem_handle))  {
        ts2SecPes_Output_subdecode (SI_offset, tsd->pid);
      }

      // -- first buffer data (also "old" prior to "pointer" offset...)
      ts2SecPes_AddPacketStart (pkt_nr, pid, continuity_counter, b, (u_long)len);

    } else {

      // -- add more data
      ts2SecPes_AddPacketContinue (pid, continuity_counter, b, (u_long)len);

    }

  }

}


//
// -- check if TS packet should already be sent to sub-decoding and output... 
// -- if so, do sub-decoding and do output
// -- return: 0 = no output, 1 = output done
//
// $$$ Remark: this routine is obsolete and in fact does nothing,
//             due to code modification mark (1)
//
int  ts2SecPes_checkAndDo_PacketSubdecode_Output (u_int pid)
{
  TS_SUBDEC* tsd = &tsds[pid];

  // -- subdecode section if we already have enough data
  if (tsd->status != TSD_output_done) {
    u_char* b = packetMem_buffer_start (tsd->mem_handle);
    u_int len = (u_int) packetMem_length (tsd->mem_handle);
    if (b && len && !(b[0]==0x00 && b[1]==0x00 && b[2]==0x01)) {
      u_int sect_len;
      u_int pointer = b[0]+1;
      b += pointer;
      sect_len = ((b[1] & 0x0F) << 8) + b[2] + 3; // sect size  (getBits)
      if (sect_len <= len) {
        ts2SecPes_Output_subdecode(0, tsd->pid);
        return 1;
      }
    }
  }

  return 0;
}


//
// -- last packet read subdecode output
// --- This is needed when eof arrives, when reading files
// --- and no new PUSI will follow.
// -- return: 0 = no output, 1 = output done
//
int  ts2SecPes_LastPacketReadSubdecode_Output (void)
{
  int i;
  for (i = 0; i < sizeof(tsds)/sizeof(TS_SUBDEC); ++i) {
    TS_SUBDEC* tsd = &tsds[i];
    if (tsd->mem_handle >= 0 && tsd->status != TSD_output_done) {
      ts2SecPes_Output_subdecode (0, tsd->pid);
    }
  }
  return 0;
}


//
// -- TS  SECTION/PES  subdecoding  output
// --  overleap_bytes: !=0 indicator how many bytes are from the "next" packet
// --                  (pointer!=0)
//
void ts2SecPes_Output_subdecode (u_int overleap_bytes, u_int pid)
{
  TS_SUBDEC* tsd = get_TSD(pid);
  //fprintf(stdout, "-># ts2SecPes_Output_subdecode: tsd.status=%u, overleap_bytes=%u\n", tsd.status, overleap_bytes);

  indent (+1);
  out_NL (3);
  if (tsd->pid > MAX_PID) {
    out_nl (3,"TS sub-decoding (%d packet(s) from %08lu):", tsd->packet_counter, tsd->pkt_nr);
  } else {
    out_nl (3,"TS sub-decoding %d packet(s) from %08lu stored for PID %u (0x%04x):",
        tsd->packet_counter,tsd->pkt_nr,tsd->pid & 0xFFFF,tsd->pid & 0xFFFF);
  }

  if (overleap_bytes) {
    out_nl (3,"Subdecoding takes %u bytes from next TS packet", overleap_bytes);
  }

  out_nl (3,"-----------------------------------------------------------------");

  if (tsd->status != TSD_no_error) {
    char *s = "";

    switch (tsd->status) {
      case TSD_error:              s = "unknown packet error"; break;
      case TSD_no_pui:             s = "no data collected, no payload start"; break;
      case TSD_continuity_error:   s = "packet continuity error"; break;
      case TSD_scrambled_error:    s = "packet scrambled or packet error"; break;
      case TSD_mem_error:          s = "subdecoding buffer (allocation) error"; break;
      case TSD_output_done:        s = "[data already displayed (this should never happen)]"; break;
    }
    out_nl (3,"STOP: %s",s);

  } else {
    u_char *b = packetMem_buffer_start (tsd->mem_handle);
    u_int len = (u_int) packetMem_length (tsd->mem_handle);

    if (b && len) {

      // -- PES/PS or SECTION

      if (b[0]==0x00 && b[1]==0x00 && b[2]==0x01) {  // som: look for PES packet_start_code_prefix (0x000001)
        // som: display the PMT stream_type
        u_int PMT_stream_type = get_StreamTypeFromMem(tsd->pid);

        if (PMT_stream_type == 0) {
          out_nl (3,"STOP: cannot find stream type for PID %u (PMT was not received yet)", tsd->pid);
        } else {
          out_S2B_NL (3,"PMT stream_type: ", PMT_stream_type, dvbstrStream_TYPE (PMT_stream_type));

          out_nl (3,"TS contains PES/PS packet (length=%u)...", len);
          ts2ps_pes_multipacket (b, len, tsd->pid, PMT_stream_type);
        }
      } else {
        int pointer = b[0]+1;
        b += pointer;

        out_nl (3,"TS contains Section (length=%u)...", len);
        ts2sec_multipacket (b, len-pointer, tsd->pid);

      }

    } else {
      out_nl (3,"No prev. packet start found...");
    }


  }

  //out_NL (3);
  out_NL (3);
  indent (-1);
  tsd->status = TSD_output_done;
}


//
// -- decode SI packets in saved TS data
// -- check for consecutive SI packets
//

void  ts2sec_multipacket (u_char *b, int len, u_int pid)
{
  int sect_len;

  while (len > 0) {

    if (b[0] == 0xFF) break;                        // stuffing, no more data

    // sect_len  = getBits (b, 0, 12, 12) + 3;
    sect_len = ((b[1] & 0x0F) << 8) + b[2] + 3;     // sect size  (getBits)
    if (sect_len > len) {                           // this should not happen!
      out_nl (3,"$$$ something is wrong here!!...");
      break;
    }

    out_nl (3,"SI packet (length=%d): ",sect_len);
    out_NL (9);
    print_databytes (9,"SI packet hexdump:", b, sect_len);
    out_NL (9);

    indent (+1);
    decodeSI_packet (b, sect_len, pid);
    indent (-1);
    out_NL (3);

    b += sect_len;
    len -= sect_len;

  }
}


//
// -- decode PS/PES packets in saved TS data
// -- check for consecutive PS/PES packets
//

void  ts2ps_pes_multipacket (u_char *b, int len, u_int pid, u_int stream_type)
{
  // we are on packet start:  b[0..2] =  0x000001
  u_int stream_id = b[3];  // PES stream_id

  int loop_count = 0;
  while (len > 0) {
    int pkt_len = 0;

    //
    // som: if the PES payload contains video data, split the entire
    // som: PES packet into multiple parts, i.e. PES header followed
    // som: by MPEG-2 headers or AVC/HEVC NAL units using the video
    // som: startcode as the delimiter
    //
    if (stream_id >= 0xE0 && stream_id <= 0xEF) {  // video PES
        int i = 5;
        while (i < (len-3)) {
          i++;                    // seek next 0x000001
          if (b[i]   != 0x00) continue;
          if (b[i+1] != 0x00) continue;
          if (b[i+2] != 0x01) continue;

          pkt_len = i;
          break;
        }
    }

    if (pkt_len == 0) {  // still not found, or last pkt in buffer
      pkt_len = len;
    }

    out_nl (3, "PS/PES %s (length=%d):", (loop_count ? "payload" : (pkt_len == len ? "packet" : "header")), pkt_len);

    out_NL (9);
    print_databytes (9,"PS/PES hexdump:", b, pkt_len);
    out_NL (9);

    indent (+1);
    decodePS_PES_packet (b, pkt_len, pid);
    indent (-1);
    out_NL (3);

    b += pkt_len;
    len -= pkt_len;
    ++loop_count;
  }
}

// 
// $$$ TODO: discontinuity signalling flag check?
//
//
// $$$ TODO: hexdump prior to decoding (-pd 9)
